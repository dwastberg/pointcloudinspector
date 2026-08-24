#pragma once

#include "renderer/planning/PointFrameCoordinator.h"
#include "renderer/rhi/RhiResource.h"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;

namespace pci {

using GpuBlockKey = PointFrameBlockKey;
using GpuBlockKeyHash = PointFrameBlockKeyHash;
using UploadBlock = PointFrameUpload;

struct GpuResidencyRecord {
    GpuBlockKey key;
    std::uint64_t bytes = 0;
    std::uint64_t lastUsedFrame = 0;
    std::uint64_t lastVisibleFrame = 0;
};

struct UploadSchedulerFrameMetrics {
    std::uint64_t uploadedBytes = 0;
    std::uint64_t pendingBytes = 0;
    std::uint64_t uploadOperations = 0;
    std::uint64_t resourceUpdateBatches = 0;
    // Uploads the residency budget could not admit this frame. A protected set
    // that fills residency leaves nothing evictable, so these blocks never
    // become drawable and the layers that own them can stall indefinitely.
    // Reporting them keeps that failure observable instead of silent.
    std::uint64_t droppedUploads = 0;
    std::uint64_t droppedBytes = 0;
    std::uint64_t protectedBytes = 0;
};

[[nodiscard]] std::size_t
planUploadCount(const std::vector<std::uint64_t> &pendingByteSizes,
                std::uint64_t byteBudget) noexcept;
[[nodiscard]] std::vector<GpuBlockKey>
planGpuEvictions(std::span<const GpuResidencyRecord> records,
                 std::uint64_t residentBytes,
                 std::uint64_t requestedBytes,
                 std::uint64_t byteBudget,
                 std::span<const GpuBlockKey> protectedBlocks = {});

class UploadScheduler {
public:
    static constexpr std::uint64_t defaultFrameByteBudget =
        std::uint64_t{48} * 1024 * 1024;
    static constexpr std::uint64_t defaultResidencyByteBudget =
        std::uint64_t{512} * 1024 * 1024;

    explicit UploadScheduler(
        std::uint64_t residencyByteBudget = defaultResidencyByteBudget);
    ~UploadScheduler();

    UploadScheduler(const UploadScheduler &) = delete;
    UploadScheduler &operator=(const UploadScheduler &) = delete;

    std::size_t
    uploadPending(QRhi *rhi,
                  QRhiCommandBuffer *commandBuffer,
                  const std::vector<UploadBlock> &blocks,
                  std::span<const GpuBlockKey> protectedBlocks,
                  std::uint64_t frameByteBudget = defaultFrameByteBudget);
    void beginFrame(std::uint64_t frameNumber) noexcept;
    void touch(const GpuBlockKey &key, bool visible) noexcept;
    void evictToBudget(std::span<const GpuBlockKey> protectedBlocks = {});
    void invalidateNode(PointCloudLayerId layerId, PointCloudNodeId nodeId);
    void invalidateLayer(PointCloudLayerId layerId);
    void retainLayers(std::span<const PointCloudLayerId> layers);
    [[nodiscard]] QRhiBuffer *bufferFor(const GpuBlockKey &key) const noexcept;
    [[nodiscard]] std::uint64_t residentPointCount() const noexcept;
    [[nodiscard]] std::uint64_t
    residentPointCount(PointCloudLayerId layerId) const noexcept;
    [[nodiscard]] std::uint64_t
    residentBytes(PointCloudLayerId layerId) const noexcept;
    [[nodiscard]] std::uint64_t residentBytes() const noexcept;
    [[nodiscard]] std::uint64_t peakResidentBytes() const noexcept;
    [[nodiscard]] std::uint64_t evictionCount() const noexcept;
    [[nodiscard]] std::uint64_t residencyByteBudget() const noexcept;
    [[nodiscard]] const UploadSchedulerFrameMetrics &
    frameMetrics() const noexcept;
    void setResidencyByteBudget(std::uint64_t byteBudget);
    void releaseResources();

private:
    struct ResidentBlock {
        RhiResourcePtr<QRhiBuffer> buffer;
        PointCloudLayerId layerId;
        std::uint64_t bytes = 0;
        std::uint64_t points = 0;
        std::uint64_t lastUsedFrame = 0;
        std::uint64_t lastVisibleFrame = 0;
    };

    void evictUntilFits(std::uint64_t requestedBytes,
                        std::span<const GpuBlockKey> protectedBlocks);
    void erase(std::unordered_map<GpuBlockKey, ResidentBlock, GpuBlockKeyHash>::
                   iterator block,
               bool countEviction = true);

    QRhi *rhi_ = nullptr;
    std::uint64_t frameNumber_ = 0;
    std::uint64_t residencyByteBudget_;
    std::uint64_t residentBytes_ = 0;
    std::uint64_t peakResidentBytes_ = 0;
    std::uint64_t evictionCount_ = 0;
    std::uint64_t residentPoints_ = 0;
    UploadSchedulerFrameMetrics frameMetrics_;
    std::unordered_map<PointCloudLayerId, std::uint64_t> layerResidentPoints_;
    std::unordered_map<PointCloudLayerId, std::uint64_t> layerResidentBytes_;
    std::unordered_map<GpuBlockKey, ResidentBlock, GpuBlockKeyHash> buffers_;
};

} // namespace pci
