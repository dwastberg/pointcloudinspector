#include <pci/pointcloud/PointBlock.h>

#include <algorithm>
#include <cmath>

namespace pci {

std::array<std::uint16_t, 3> quantizeToBlock(const Vec3d position,
                                             const Vec3d origin,
                                             const double scale) noexcept
{
    const double invScale = scale > 0.0 ? 1.0 / scale : 0.0;
    return quantizeToBlockFast(position, origin, invScale);
}

std::array<std::uint16_t, 3> quantizeToBlockFast(const Vec3d position,
                                                 const Vec3d origin,
                                                 const double invScale) noexcept
{
    return {
        static_cast<std::uint16_t>(
            std::clamp(std::lround((position.x - origin.x) * invScale),
                       0L,
                       static_cast<long>(blockQuantizationSteps))),
        static_cast<std::uint16_t>(
            std::clamp(std::lround((position.y - origin.y) * invScale),
                       0L,
                       static_cast<long>(blockQuantizationSteps))),
        static_cast<std::uint16_t>(
            std::clamp(std::lround((position.z - origin.z) * invScale),
                       0L,
                       static_cast<long>(blockQuantizationSteps))),
    };
}

Vec3d decodeBlockPosition(const PointBlock &block,
                          const GpuPoint &point) noexcept
{
    return block.origin + Vec3d{
                              static_cast<double>(point.x),
                              static_cast<double>(point.y),
                              static_cast<double>(point.z),
                          } * block.scale;
}

double blockScaleForEdge(const double cellEdge) noexcept
{
    return cellEdge / blockQuantizationSteps;
}

} // namespace pci
