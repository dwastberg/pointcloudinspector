#pragma once

#include <pci/foundation/Bounds3d.h>
#include <pci/foundation/Vec3d.h>
#include <pci/pointcloud/GpuPoint.h>
#include <pci/pointcloud/PointAttributes.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace pci {

// One immutable block is also one GPU upload. Keep it small enough to publish
// and upload during an interactive frame while allowing a quantization cell to
// emit any number of blocks with the same origin and scale.
inline constexpr std::uint32_t maximumPointsPerBlock = 65'536;
inline constexpr double blockQuantizationSteps = 65535.0;

struct PointBlock {
    Vec3d origin;
    double scale = 1.0; // world units per quantization step
    Bounds3d bounds;    // tight world-space bounds of contained points
    std::uint16_t intensityMinimum = 0;
    std::uint16_t intensityMaximum = 0;
    std::vector<GpuPoint> points;
    std::vector<PointAttributes> attributes;
};

using PointBlockPtr = std::shared_ptr<const PointBlock>;

[[nodiscard]] std::array<std::uint16_t, 3>
quantizeToBlock(Vec3d position, Vec3d origin, double scale) noexcept;
[[nodiscard]] Vec3d decodeBlockPosition(const PointBlock &block,
                                        const GpuPoint &point) noexcept;
[[nodiscard]] double blockScaleForEdge(double cellEdge) noexcept;

} // namespace pci
