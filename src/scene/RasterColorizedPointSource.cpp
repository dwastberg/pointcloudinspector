#include "scene/RasterColorizedPointSource.h"

#include <algorithm>
#include <stdexcept>

namespace pci {

RasterColorizedPointSource::RasterColorizedPointSource(
    PointCloudDataSourcePtr source,
    std::vector<std::uint32_t> colors,
    std::vector<RasterColorizeRange> ranges,
    PointMemoryBudget::ReservationPtr reservation)
    : source_(std::move(source))
    , colors_(std::move(colors))
    , ranges_(std::move(ranges))
    , reservation_(std::move(reservation))
{
    if (!source_ || !reservation_) {
        throw std::invalid_argument(
            "Raster-colored source requires source data and a reservation");
    }
    std::vector<RasterColorizeRange> byOffset = ranges_;
    std::ranges::sort(byOffset, {}, &RasterColorizeRange::offset);
    for (std::size_t index = 0; index < byOffset.size(); ++index) {
        const RasterColorizeRange &range = byOffset[index];
        if (range.offset > colors_.size() ||
            range.count > colors_.size() - range.offset) {
            throw std::invalid_argument("Raster color range is out of bounds");
        }
        if (index > 0) {
            const RasterColorizeRange &before = byOffset[index - 1];
            if (before.offset + before.count > range.offset) {
                throw std::invalid_argument("Raster color ranges overlap");
            }
        }
    }
    std::ranges::sort(ranges_, {}, &RasterColorizeRange::node);
    if (std::ranges::adjacent_find(ranges_, {}, &RasterColorizeRange::node) !=
        ranges_.end()) {
        throw std::invalid_argument("Raster color ranges repeat a node");
    }
}

const PointCloudDataSourcePtr &RasterColorizedPointSource::base() const noexcept
{
    return source_;
}

std::uint64_t RasterColorizedPointSource::byteSize() const noexcept
{
    return colors_.capacity() * sizeof(std::uint32_t);
}

std::uint64_t RasterColorizedPointSource::countMismatchCount() const noexcept
{
    return countMismatches_.load(std::memory_order_relaxed);
}

PointCloudNode RasterColorizedPointSource::rootNode() const
{
    return source_->rootNode();
}

PointCloudNode RasterColorizedPointSource::node(const PointCloudNodeId id) const
{
    return source_->node(id);
}

PointCloudNodePayloadPtr
RasterColorizedPointSource::loadNode(const PointCloudNodeId id,
                                     const std::stop_token stopToken) const
{
    PointCloudNodePayloadPtr payload = source_->loadNode(id, stopToken);
    const auto range =
        std::ranges::lower_bound(ranges_, id, {}, &RasterColorizeRange::node);
    if (range == ranges_.end() || range->node != id) {
        return payload;
    }
    if (!payload || pointCloudNodePayloadPoints(*payload) != range->count) {
        countMismatches_.fetch_add(1, std::memory_order_relaxed);
        return payload;
    }

    auto colored = std::make_shared<PointCloudNodePayload>();
    colored->nodeId = payload->nodeId;
    colored->sourcePointCount = payload->sourcePointCount;
    colored->blocks.reserve(payload->blocks.size());
    std::uint64_t offset = range->offset;
    for (const PointBlockPtr &block : payload->blocks) {
        if (!block) {
            colored->blocks.push_back({});
            continue;
        }
        auto copy = std::make_shared<PointBlock>(*block);
        for (GpuPoint &point : copy->points) {
            point.rgba = colors_[offset++];
        }
        colored->blocks.push_back(std::move(copy));
    }
    return colored;
}

PointCloudDataSourceMetrics RasterColorizedPointSource::metrics() const
{
    return source_->metrics();
}

PointCloudStorageMetrics RasterColorizedPointSource::storageMetrics() const
{
    return source_->storageMetrics();
}

PointCloudScalarRanges RasterColorizedPointSource::scalarRanges() const
{
    return source_->scalarRanges();
}

std::optional<PointCloudFullDetailInfo>
RasterColorizedPointSource::fullDetailInfo() const
{
    return source_->fullDetailInfo();
}

std::optional<std::vector<PointCloudStoredNode>>
RasterColorizedPointSource::storedNodeIndex() const
{
    return source_->storedNodeIndex();
}

} // namespace pci
