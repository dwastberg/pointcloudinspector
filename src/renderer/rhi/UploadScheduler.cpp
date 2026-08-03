#include "renderer/rhi/UploadScheduler.h"

#include "foundation/CheckedArithmetic.h"

#include <QByteArray>
#include <rhi/qrhi.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace pci {
std::size_t planUploadCount(const std::vector<std::uint64_t> &pendingByteSizes,
                            const std::uint64_t byteBudget) noexcept
{
    std::size_t count = 0;
    std::uint64_t used = 0;
    for (const std::uint64_t size : pendingByteSizes) {
        if (size > byteBudget - std::min(used, byteBudget)) {
            // An oversized first block must still be attempted so a small
            // frame budget cannot stall forever. Normal point blocks are
            // smaller than the default frame allowance.
            if (count == 0) {
                ++count;
            }
            break;
        }
        used += size;
        ++count;
    }
    return count;
}

std::vector<GpuBlockKey>
planGpuEvictions(const std::span<const GpuResidencyRecord> records,
                 const std::uint64_t residentBytes,
                 const std::uint64_t requestedBytes,
                 const std::uint64_t byteBudget,
                 const std::span<const GpuBlockKey> protectedBlocks)
{
    const std::uint64_t targetResident =
        requestedBytes >= byteBudget ? 0 : byteBudget - requestedBytes;
    if (residentBytes <= targetResident) {
        return {};
    }
    const std::unordered_set<GpuBlockKey, GpuBlockKeyHash> protectedSet(
        protectedBlocks.begin(), protectedBlocks.end());
    std::vector<GpuResidencyRecord> candidates;
    candidates.reserve(records.size());
    for (const GpuResidencyRecord &record : records) {
        if (!protectedSet.contains(record.key)) {
            candidates.push_back(record);
        }
    }
    std::ranges::sort(
        candidates,
        [](const GpuResidencyRecord &left, const GpuResidencyRecord &right) {
            if (left.lastVisibleFrame != right.lastVisibleFrame) {
                return left.lastVisibleFrame < right.lastVisibleFrame;
            }
            if (left.lastUsedFrame != right.lastUsedFrame) {
                return left.lastUsedFrame < right.lastUsedFrame;
            }
            return left.key < right.key;
        });

    std::vector<GpuBlockKey> result;
    std::uint64_t remaining = residentBytes;
    for (const GpuResidencyRecord &candidate : candidates) {
        if (remaining <= targetResident) {
            break;
        }
        result.push_back(candidate.key);
        remaining =
            candidate.bytes > remaining ? 0 : remaining - candidate.bytes;
    }
    return result;
}

UploadScheduler::~UploadScheduler()
{
    releaseResources();
}

UploadScheduler::UploadScheduler(const std::uint64_t residencyByteBudget)
    : residencyByteBudget_(residencyByteBudget)
{
    if (residencyByteBudget == 0) {
        throw std::invalid_argument("GPU residency budget must be positive");
    }
}

std::size_t UploadScheduler::uploadPending(
    QRhi *rhi,
    QRhiCommandBuffer *commandBuffer,
    const std::vector<UploadBlock> &blocks,
    const std::span<const GpuBlockKey> protectedBlocks,
    const std::uint64_t frameByteBudget)
{
    frameMetrics_ = {};
    if (!rhi || !commandBuffer) {
        throw std::invalid_argument("block uploads require QRhi resources");
    }
    if (rhi_ && rhi_ != rhi) {
        releaseResources();
    }
    rhi_ = rhi;

    std::vector<UploadBlock> pending;
    std::vector<std::uint64_t> pendingSizes;
    for (const UploadBlock &upload : blocks) {
        if (!upload.block) {
            continue;
        }
        if (!buffers_.contains(upload.key)) {
            pending.push_back(upload);
            pendingSizes.push_back(upload.block->points.size() *
                                   sizeof(GpuPoint));
            const std::uint64_t bytes = pendingSizes.back();
            frameMetrics_.pendingBytes =
                saturatingAdd(frameMetrics_.pendingBytes, bytes);
        }
    }
    const std::size_t uploadCount =
        planUploadCount(pendingSizes, frameByteBudget);
    for (const GpuBlockKey &key : protectedBlocks) {
        const auto resident = buffers_.find(key);
        if (resident != buffers_.end()) {
            frameMetrics_.protectedBytes += resident->second.bytes;
        }
    }
    const auto recordDrop = [this](const std::uint64_t bytes) {
        ++frameMetrics_.droppedUploads;
        frameMetrics_.droppedBytes += bytes;
    };

    std::size_t uploaded = 0;
    RhiResourceUpdateBatchPtr updates;
    std::vector<GpuBlockKey> unsubmittedKeys;
    unsubmittedKeys.reserve(uploadCount);
    const auto submitUpdates = [&] {
        if (!updates) {
            return;
        }
        commandBuffer->resourceUpdate(updates.release());
        unsubmittedKeys.clear();
        ++frameMetrics_.resourceUpdateBatches;
    };
    try {
        for (std::size_t i = 0; i < uploadCount; ++i) {
            const UploadBlock &upload = pending[i];
            const PointBlock *block = upload.block.get();
            const auto bytes =
                static_cast<quint32>(block->points.size() * sizeof(GpuPoint));
            if (bytes > residencyByteBudget_) {
                recordDrop(bytes);
                continue;
            }
            evictUntilFits(bytes, protectedBlocks);
            if (residentBytes_ + bytes > residencyByteBudget_) {
                recordDrop(bytes);
                continue;
            }
            RhiResourcePtr<QRhiBuffer> buffer(rhi_->newBuffer(
                QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, bytes));
            buffer->setName(QByteArrayLiteral("Point block"));
            if (!buffer->create()) {
                throw std::runtime_error(
                    "Could not create QRhi point-block buffer");
            }
            if (!updates) {
                updates.reset(rhi_->nextResourceUpdateBatch());
            }
            QByteArray staging(
                reinterpret_cast<const char *>(block->points.data()),
                static_cast<qsizetype>(bytes));
            const std::uint64_t points = block->points.size();
            const auto [resident, inserted] =
                buffers_.emplace(upload.key,
                                 ResidentBlock{
                                     .buffer = std::move(buffer),
                                     .layerId = upload.key.layerId,
                                     .bytes = bytes,
                                     .points = points,
                                     .lastUsedFrame = frameNumber_,
                                     .lastVisibleFrame = frameNumber_,
                                 });
            if (!inserted) {
                throw std::logic_error(
                    "point-block buffer became resident during upload");
            }
            unsubmittedKeys.push_back(upload.key);
            updates->uploadStaticBuffer(
                resident->second.buffer.get(), 0, std::move(staging));
            residentBytes_ += bytes;
            peakResidentBytes_ = std::max(peakResidentBytes_, residentBytes_);
            residentPoints_ += points;
            layerResidentBytes_[upload.key.layerId] += bytes;
            layerResidentPoints_[upload.key.layerId] += points;
            ++uploaded;
            frameMetrics_.uploadedBytes += bytes;
            ++frameMetrics_.uploadOperations;
            if (!updates->hasOptimalCapacity()) {
                submitUpdates();
            }
        }
        submitUpdates();
    } catch (...) {
        updates.reset();
        for (const GpuBlockKey &key : unsubmittedKeys) {
            const auto found = buffers_.find(key);
            if (found != buffers_.end()) {
                erase(found, false);
            }
        }
        throw;
    }
    frameMetrics_.pendingBytes =
        frameMetrics_.uploadedBytes >= frameMetrics_.pendingBytes
            ? 0
            : frameMetrics_.pendingBytes - frameMetrics_.uploadedBytes;
    return uploaded;
}

void UploadScheduler::beginFrame(const std::uint64_t frameNumber) noexcept
{
    frameNumber_ = frameNumber;
}

void UploadScheduler::touch(const GpuBlockKey &key, const bool visible) noexcept
{
    const auto found = buffers_.find(key);
    if (found == buffers_.end()) {
        return;
    }
    found->second.lastUsedFrame = frameNumber_;
    if (visible) {
        found->second.lastVisibleFrame = frameNumber_;
    }
}

void UploadScheduler::evictToBudget(
    const std::span<const GpuBlockKey> protectedBlocks)
{
    evictUntilFits(0, protectedBlocks);
}

void UploadScheduler::invalidateNode(const PointCloudLayerId layerId,
                                     const PointCloudNodeId nodeId)
{
    for (auto current = buffers_.begin(); current != buffers_.end();) {
        if (current->first.layerId == layerId &&
            current->first.nodeId == nodeId) {
            const auto victim = current++;
            erase(victim);
        } else {
            ++current;
        }
    }
}

void UploadScheduler::retainLayers(
    const std::span<const PointCloudLayerId> layers)
{
    const std::unordered_set<PointCloudLayerId> retained(layers.begin(),
                                                         layers.end());
    for (auto current = buffers_.begin(); current != buffers_.end();) {
        if (!retained.contains(current->second.layerId)) {
            const auto victim = current++;
            erase(victim);
        } else {
            ++current;
        }
    }
}

QRhiBuffer *UploadScheduler::bufferFor(const GpuBlockKey &key) const noexcept
{
    const auto found = buffers_.find(key);
    return found == buffers_.end() ? nullptr : found->second.buffer.get();
}

std::uint64_t UploadScheduler::residentPointCount() const noexcept
{
    return residentPoints_;
}

std::uint64_t UploadScheduler::residentPointCount(
    const PointCloudLayerId layerId) const noexcept
{
    const auto found = layerResidentPoints_.find(layerId);
    return found == layerResidentPoints_.end() ? 0 : found->second;
}

std::uint64_t
UploadScheduler::residentBytes(const PointCloudLayerId layerId) const noexcept
{
    const auto found = layerResidentBytes_.find(layerId);
    return found == layerResidentBytes_.end() ? 0 : found->second;
}

std::uint64_t UploadScheduler::residentBytes() const noexcept
{
    return residentBytes_;
}

std::uint64_t UploadScheduler::peakResidentBytes() const noexcept
{
    return peakResidentBytes_;
}

std::uint64_t UploadScheduler::evictionCount() const noexcept
{
    return evictionCount_;
}

std::uint64_t UploadScheduler::residencyByteBudget() const noexcept
{
    return residencyByteBudget_;
}

const UploadSchedulerFrameMetrics &
UploadScheduler::frameMetrics() const noexcept
{
    return frameMetrics_;
}

void UploadScheduler::setResidencyByteBudget(const std::uint64_t byteBudget)
{
    if (byteBudget == 0) {
        throw std::invalid_argument("GPU residency budget must be positive");
    }
    residencyByteBudget_ = byteBudget;
    evictToBudget();
}

void UploadScheduler::releaseResources()
{
    buffers_.clear();
    layerResidentPoints_.clear();
    layerResidentBytes_.clear();
    residentBytes_ = 0;
    peakResidentBytes_ = 0;
    evictionCount_ = 0;
    residentPoints_ = 0;
    frameMetrics_ = {};
    rhi_ = nullptr;
}

void UploadScheduler::evictUntilFits(
    const std::uint64_t requestedBytes,
    const std::span<const GpuBlockKey> protectedBlocks)
{
    std::vector<GpuResidencyRecord> records;
    records.reserve(buffers_.size());
    for (const auto &[key, resident] : buffers_) {
        records.push_back({
            .key = key,
            .bytes = resident.bytes,
            .lastUsedFrame = resident.lastUsedFrame,
            .lastVisibleFrame = resident.lastVisibleFrame,
        });
    }
    for (const GpuBlockKey &victim : planGpuEvictions(records,
                                                      residentBytes_,
                                                      requestedBytes,
                                                      residencyByteBudget_,
                                                      protectedBlocks)) {
        const auto found = buffers_.find(victim);
        if (found != buffers_.end()) {
            erase(found);
        }
    }
}

void UploadScheduler::erase(
    std::unordered_map<GpuBlockKey, ResidentBlock, GpuBlockKeyHash>::iterator
        block,
    const bool countEviction)
{
    residentBytes_ -= block->second.bytes;
    residentPoints_ -= block->second.points;
    auto layerBytes = layerResidentBytes_.find(block->second.layerId);
    if (layerBytes != layerResidentBytes_.end()) {
        layerBytes->second -= block->second.bytes;
        if (layerBytes->second == 0) {
            layerResidentBytes_.erase(layerBytes);
        }
    }
    auto layer = layerResidentPoints_.find(block->second.layerId);
    if (layer != layerResidentPoints_.end()) {
        layer->second -= block->second.points;
        if (layer->second == 0) {
            layerResidentPoints_.erase(layer);
        }
    }
    buffers_.erase(block);
    if (countEviction) {
        ++evictionCount_;
    }
}

} // namespace pci
