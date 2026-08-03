#include "scene/PointCloudNode.h"

#include "foundation/Hash.h"

#include <algorithm>
#include <bit>
#include <limits>

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

} // namespace pci
