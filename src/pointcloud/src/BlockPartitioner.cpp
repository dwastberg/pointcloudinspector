#include <pci/pointcloud/BlockPartitioner.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pci {
namespace {

constexpr double targetPointsPerCell = 262'144.0;
constexpr std::uint64_t maximumCellsPerAxis = 1024;

void expandBounds(Bounds3d &bounds,
                  const Vec3d position,
                  const bool first) noexcept
{
    if (first) {
        bounds.minimum = {position.x, position.y, position.z};
        bounds.maximum = bounds.minimum;
        return;
    }
    bounds.extend(position);
}

} // namespace

BlockPartitioner::BlockPartitioner(const Bounds3d &bounds,
                                   const std::uint64_t expectedPointCount,
                                   BlockReady blockReady,
                                   const std::size_t initialBlockCapacity)
    : bounds_(bounds)
    , initialBlockCapacity_(
          std::min<std::size_t>(maximumPointsPerBlock, initialBlockCapacity))
    , blockReady_(std::move(blockReady))
{
    if (!blockReady_) {
        throw std::invalid_argument(
            "block partitioner requires a block-ready callback");
    }
    const double extent = std::max(bounds_.maximumExtent(), 1e-9);
    const auto cellsPerAxis = static_cast<std::uint64_t>(std::clamp(
        std::ceil(std::cbrt(static_cast<double>(std::max<std::uint64_t>(
                                expectedPointCount, 1)) /
                            targetPointsPerCell)),
        1.0,
        static_cast<double>(maximumCellsPerAxis)));
    edge_ = extent / static_cast<double>(cellsPerAxis);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double axisExtent = bounds_.maximum[axis] - bounds_.minimum[axis];
        cellCounts_[axis] =
            std::max<std::uint64_t>(1,
                                    static_cast<std::uint64_t>(std::ceil(
                                        std::max(axisExtent, 0.0) / edge_)));
    }
}

double BlockPartitioner::cellEdge() const noexcept
{
    return edge_;
}

std::uint64_t BlockPartitioner::cellKeyFor(const Vec3d position) const noexcept
{
    std::array<std::uint64_t, 3> index{};
    const std::array<double, 3> components{position.x, position.y, position.z};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double offset = components[axis] - bounds_.minimum[axis];
        const auto cell = static_cast<std::int64_t>(std::floor(offset / edge_));
        index[axis] = static_cast<std::uint64_t>(std::clamp<std::int64_t>(
            cell, 0, static_cast<std::int64_t>(cellCounts_[axis]) - 1));
    }
    return index[0] + index[1] * cellCounts_[0] +
           index[2] * cellCounts_[0] * cellCounts_[1];
}

Vec3d BlockPartitioner::cellOrigin(const std::uint64_t key) const noexcept
{
    const std::uint64_t xy = cellCounts_[0] * cellCounts_[1];
    const std::uint64_t iz = key / xy;
    const std::uint64_t iy = (key % xy) / cellCounts_[0];
    const std::uint64_t ix = key % cellCounts_[0];
    return {
        bounds_.minimum[0] + static_cast<double>(ix) * edge_,
        bounds_.minimum[1] + static_cast<double>(iy) * edge_,
        bounds_.minimum[2] + static_cast<double>(iz) * edge_,
    };
}

void BlockPartitioner::add(const PointSample &sample)
{
    const std::uint64_t key = cellKeyFor(sample.position);
    auto &block = building_[key];
    if (!block) {
        block = std::make_shared<PointBlock>();
        block->origin = cellOrigin(key);
        block->scale = blockScaleForEdge(edge_);
        const std::size_t capacity =
            initialBlockCapacity_ != 0
                ? initialBlockCapacity_
                : std::min<std::uint64_t>(maximumPointsPerBlock, 4096);
        block->points.reserve(capacity);
        block->attributes.reserve(capacity);
    }

    const bool firstPoint = block->points.empty();
    const auto quantized =
        quantizeToBlock(sample.position, block->origin, block->scale);
    expandBounds(block->bounds, sample.position, firstPoint);
    block->points.push_back({
        .x = quantized[0],
        .y = quantized[1],
        .z = quantized[2],
        .attributes = sample.packedAttributes,
        .rgba = sample.rgba,
        .packedProperties = sample.packedProperties,
    });
    block->attributes.push_back(sample.attributes);
    if (firstPoint) {
        block->intensityMinimum = sample.attributes.intensity;
    } else {
        block->intensityMinimum =
            std::min(block->intensityMinimum, sample.attributes.intensity);
    }
    block->intensityMaximum =
        std::max(block->intensityMaximum, sample.attributes.intensity);

    if (block->points.size() >= maximumPointsPerBlock) {
        seal(block);
        building_.erase(key);
    }
}

void BlockPartitioner::finish()
{
    for (auto &[key, block] : building_) {
        if (block && !block->points.empty()) {
            seal(block);
        }
    }
    building_.clear();
}

std::size_t BlockPartitioner::flushLargest(const std::size_t maximumBlocks)
{
    if (maximumBlocks == 0 || building_.empty()) {
        return 0;
    }

    std::vector<std::uint64_t> candidates;
    candidates.reserve(building_.size());
    for (const auto &[key, block] : building_) {
        if (block && !block->points.empty()) {
            candidates.push_back(key);
        }
    }
    std::ranges::sort(
        candidates,
        [this](const std::uint64_t left, const std::uint64_t right) {
            const std::size_t leftSize = building_.at(left)->points.size();
            const std::size_t rightSize = building_.at(right)->points.size();
            return leftSize != rightSize ? leftSize > rightSize : left < right;
        });

    const std::size_t count = std::min(maximumBlocks, candidates.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto found = building_.find(candidates[index]);
        if (found == building_.end()) {
            continue;
        }
        seal(found->second);
        building_.erase(found);
    }
    return count;
}

void BlockPartitioner::seal(std::shared_ptr<PointBlock> &block)
{
    blockReady_(PointBlockPtr(std::move(block)));
}

} // namespace pci
