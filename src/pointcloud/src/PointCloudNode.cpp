#include <pci/pointcloud/PointCloudNode.h>

#include <pci/foundation/Hash.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace pci {
std::size_t
PointCloudNodeIdHash::operator()(const PointCloudNodeId &id) const noexcept
{
    std::size_t result = std::hash<std::uint8_t>{}(id.level);
    result = hashCombine(result, std::hash<std::uint32_t>{}(id.x));
    result = hashCombine(result, std::hash<std::uint32_t>{}(id.y));
    return hashCombine(result, std::hash<std::uint32_t>{}(id.z));
}

PointCloudNodeId childNodeId(const PointCloudNodeId parent,
                             const std::uint8_t octant) noexcept
{
    return {
        .level = static_cast<std::uint8_t>(parent.level + 1U),
        .x = static_cast<std::uint32_t>((parent.x << 1U) | (octant & 0x1U)),
        .y = static_cast<std::uint32_t>((parent.y << 1U) |
                                        ((octant >> 1U) & 0x1U)),
        .z = static_cast<std::uint32_t>((parent.z << 1U) |
                                        ((octant >> 2U) & 0x1U)),
    };
}

std::array<PointCloudNodeId, 8>
childNodeIds(const PointCloudNodeId parent) noexcept
{
    std::array<PointCloudNodeId, 8> children;
    for (std::size_t octant = 0; octant < children.size(); ++octant) {
        children[octant] =
            childNodeId(parent, static_cast<std::uint8_t>(octant));
    }
    return children;
}

Bounds3d pointCloudNodeBounds(const Bounds3d &rootBounds,
                              const PointCloudNodeId id) noexcept
{
    Bounds3d result;
    const std::uint64_t cells = std::uint64_t{1} << id.level;
    const std::array<std::uint32_t, 3> indices{id.x, id.y, id.z};
    for (std::size_t axis = 0; axis < indices.size(); ++axis) {
        const double extent =
            rootBounds.maximum[axis] - rootBounds.minimum[axis];
        const double edge = extent / static_cast<double>(cells);
        result.minimum[axis] = rootBounds.minimum[axis] +
                               static_cast<double>(indices[axis]) * edge;
        result.maximum[axis] = indices[axis] + 1U == cells
                                   ? rootBounds.maximum[axis]
                                   : result.minimum[axis] + edge;
    }
    return result;
}

std::uint64_t
pointCloudNodePayloadBytes(const PointCloudNodePayload &payload) noexcept
{
    std::uint64_t bytes = 0;
    for (const PointBlockPtr &block : payload.blocks) {
        if (!block) {
            continue;
        }
        bytes += block->points.capacity() * sizeof(GpuPoint);
        bytes += block->attributes.capacity() * sizeof(PointAttributes);
    }
    return bytes;
}

std::uint64_t
pointCloudNodePayloadPoints(const PointCloudNodePayload &payload) noexcept
{
    std::uint64_t points = 0;
    for (const PointBlockPtr &block : payload.blocks) {
        if (block) {
            points += block->points.size();
        }
    }
    return points;
}

std::uint64_t
maximumRootBytesPerPoint(const PointCloudNodePayload &payload) noexcept
{
    std::uint64_t result = 0;
    for (const PointBlockPtr &block : payload.blocks) {
        if (!block || block->points.empty()) {
            continue;
        }
        result = std::max<std::uint64_t>(
            result,
            sizeof(GpuPoint) + (block->attributes.size() == block->points.size()
                                    ? sizeof(PointAttributes)
                                    : 0));
    }
    return result;
}

PointCloudNodePayloadPtr
reduceRootPayloadToCount(const PointCloudNodePayload &source,
                         const std::uint64_t targetPoints)
{
    const std::uint64_t totalPoints = pointCloudNodePayloadPoints(source);
    if (targetPoints == 0 || targetPoints > totalPoints) {
        throw std::invalid_argument("invalid hierarchy root point count");
    }

    std::vector<std::uint64_t> selected;
    selected.reserve(static_cast<std::size_t>(targetPoints));
    for (std::uint64_t rank = 0; rank < targetPoints; ++rank) {
        const long double position = static_cast<long double>(rank) *
                                     static_cast<long double>(totalPoints) /
                                     static_cast<long double>(targetPoints);
        selected.push_back(static_cast<std::uint64_t>(position));
    }

    std::vector<std::size_t> selectedPerBlock(source.blocks.size(), 0);
    std::size_t blockIndex = 0;
    std::uint64_t blockBegin = 0;
    for (const std::uint64_t point : selected) {
        while (blockIndex < source.blocks.size()) {
            const PointBlockPtr &block = source.blocks[blockIndex];
            const std::uint64_t blockEnd =
                blockBegin + (block ? block->points.size() : 0);
            if (point < blockEnd) {
                ++selectedPerBlock[blockIndex];
                break;
            }
            blockBegin = blockEnd;
            ++blockIndex;
        }
    }

    std::vector<std::shared_ptr<PointBlock>> mutableBlocks(
        source.blocks.size());
    for (std::size_t index = 0; index < source.blocks.size(); ++index) {
        const PointBlockPtr &block = source.blocks[index];
        if (!block || selectedPerBlock[index] == 0) {
            continue;
        }
        auto reduced = std::make_shared<PointBlock>();
        reduced->origin = block->origin;
        reduced->scale = block->scale;
        // Retain conservative source-block bounds. Root sampling must not make
        // later detail regions disappear from frustum selection.
        reduced->bounds = block->bounds;
        reduced->intensityMinimum = block->intensityMinimum;
        reduced->intensityMaximum = block->intensityMaximum;
        reduced->points.reserve(selectedPerBlock[index]);
        if (block->attributes.size() == block->points.size()) {
            reduced->attributes.reserve(selectedPerBlock[index]);
        }
        mutableBlocks[index] = std::move(reduced);
    }

    blockIndex = 0;
    blockBegin = 0;
    for (const std::uint64_t point : selected) {
        while (blockIndex < source.blocks.size()) {
            const PointBlockPtr &block = source.blocks[blockIndex];
            const std::uint64_t blockEnd =
                blockBegin + (block ? block->points.size() : 0);
            if (point < blockEnd) {
                const std::size_t local =
                    static_cast<std::size_t>(point - blockBegin);
                mutableBlocks[blockIndex]->points.push_back(
                    block->points[local]);
                if (block->attributes.size() == block->points.size()) {
                    mutableBlocks[blockIndex]->attributes.push_back(
                        block->attributes[local]);
                }
                break;
            }
            blockBegin = blockEnd;
            ++blockIndex;
        }
    }

    auto result = std::make_shared<PointCloudNodePayload>();
    result->nodeId = rootPointCloudNode;
    result->sourcePointCount = source.sourcePointCount;
    for (std::shared_ptr<PointBlock> &block : mutableBlocks) {
        if (block) {
            result->blocks.push_back(std::move(block));
        }
    }
    return result;
}

PointCloudNodePayloadPtr reducedRootPayload(const PointCloudNodePayload &source,
                                            const std::uint64_t maximumBytes)
{
    const std::uint64_t totalPoints = pointCloudNodePayloadPoints(source);
    const std::uint64_t bytesPerPoint = maximumRootBytesPerPoint(source);
    if (totalPoints == 0 || bytesPerPoint == 0 ||
        maximumBytes < bytesPerPoint) {
        throw std::length_error(
            "hierarchy root budget cannot retain one preview point");
    }
    std::uint64_t targetPoints =
        std::min(totalPoints, maximumBytes / bytesPerPoint);

    while (targetPoints > 0) {
        PointCloudNodePayloadPtr result =
            reduceRootPayloadToCount(source, targetPoints);
        const std::uint64_t actualBytes = pointCloudNodePayloadBytes(*result);
        if (actualBytes <= maximumBytes) {
            return result;
        }
        const std::uint64_t excess = actualBytes - maximumBytes;
        const std::uint64_t reduction = std::max<std::uint64_t>(
            1, (excess + bytesPerPoint - 1) / bytesPerPoint);
        targetPoints = reduction >= targetPoints ? 0 : targetPoints - reduction;
    }
    throw std::length_error(
        "hierarchy root budget cannot retain one preview point");
}

} // namespace pci
