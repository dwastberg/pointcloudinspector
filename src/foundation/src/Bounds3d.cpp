#include <pci/foundation/Bounds3d.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace pci {

void Bounds3d::extend(const Bounds3d &bounds) noexcept
{
    for (std::size_t axis = 0; axis < minimum.size(); ++axis) {
        minimum[axis] = std::min(minimum[axis], bounds.minimum[axis]);
        maximum[axis] = std::max(maximum[axis], bounds.maximum[axis]);
    }
}

void Bounds3d::extend(const Vec3d point) noexcept
{
    const std::array components{point.x, point.y, point.z};
    for (std::size_t axis = 0; axis < minimum.size(); ++axis) {
        minimum[axis] = std::min(minimum[axis], components[axis]);
        maximum[axis] = std::max(maximum[axis], components[axis]);
    }
}

bool Bounds3d::valid() const noexcept
{
    for (std::size_t axis = 0; axis < minimum.size(); ++axis) {
        if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) ||
            minimum[axis] > maximum[axis]) {
            return false;
        }
    }
    return true;
}

std::array<double, 3> Bounds3d::center() const noexcept
{
    return {
        std::midpoint(minimum[0], maximum[0]),
        std::midpoint(minimum[1], maximum[1]),
        std::midpoint(minimum[2], maximum[2]),
    };
}

double Bounds3d::maximumExtent() const noexcept
{
    return std::max({
        maximum[0] - minimum[0],
        maximum[1] - minimum[1],
        maximum[2] - minimum[2],
    });
}

} // namespace pci
