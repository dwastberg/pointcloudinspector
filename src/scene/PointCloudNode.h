#pragma once

#include "foundation/Bounds3d.h"
#include "scene/PointBlock.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pci {

struct PointCloudNodeId {
    std::uint8_t level = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;

    auto operator<=>(const PointCloudNodeId &) const = default;
};

struct PointCloudNodeIdHash {
    [[nodiscard]] std::size_t
    operator()(const PointCloudNodeId &id) const noexcept;
};

inline constexpr PointCloudNodeId rootPointCloudNode{};

struct PointCloudNode {
    PointCloudNodeId id;
    Bounds3d bounds;
    double geometricError = 0.0;
    std::uint64_t estimatedPointCount = 0;
    bool leaf = true;
};

struct PointCloudNodePayload {
    PointCloudNodeId nodeId;
    std::vector<PointBlockPtr> blocks;
    std::uint64_t sourcePointCount = 0;
};

using PointCloudNodePayloadPtr = std::shared_ptr<const PointCloudNodePayload>;

[[nodiscard]] PointCloudNodeId childNodeId(PointCloudNodeId parent,
                                           std::uint8_t octant) noexcept;
[[nodiscard]] std::array<PointCloudNodeId, 8>
childNodeIds(PointCloudNodeId parent) noexcept;
[[nodiscard]] Bounds3d pointCloudNodeBounds(const Bounds3d &rootBounds,
                                            PointCloudNodeId id) noexcept;
[[nodiscard]] std::uint64_t
pointCloudNodePayloadBytes(const PointCloudNodePayload &payload) noexcept;
[[nodiscard]] std::uint64_t
pointCloudNodePayloadPoints(const PointCloudNodePayload &payload) noexcept;

} // namespace pci
