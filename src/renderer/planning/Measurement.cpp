#include "renderer/planning/Measurement.h"

#include <cmath>

namespace pci {

DistanceMeasurement makeDistanceMeasurement(const Vec3d start,
                                            const Vec3d end) noexcept
{
    const Vec3d delta = end - start;
    return {
        .start = start,
        .end = end,
        .distance3d = length(delta),
        .horizontalDistance = std::hypot(delta.x, delta.y),
        .verticalDelta = delta.z,
    };
}

} // namespace pci
