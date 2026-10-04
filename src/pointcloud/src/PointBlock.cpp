#include <pci/pointcloud/PointBlock.h>

#include <algorithm>
#include <cmath>

namespace pci {
namespace {

std::uint16_t quantizeComponent(const double value,
                                const double origin,
                                const double scale) noexcept
{
    const double steps = (value - origin) / scale;
    return static_cast<std::uint16_t>(std::clamp(
        std::lround(steps), 0L, static_cast<long>(blockQuantizationSteps)));
}

} // namespace

std::array<std::uint16_t, 3> quantizeToBlock(const Vec3d position,
                                             const Vec3d origin,
                                             const double scale) noexcept
{
    return {
        quantizeComponent(position.x, origin.x, scale),
        quantizeComponent(position.y, origin.y, scale),
        quantizeComponent(position.z, origin.z, scale),
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
