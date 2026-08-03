#pragma once

#include "foundation/Vec3d.h"

namespace pci {

struct DistanceMeasurement {
    Vec3d start;
    Vec3d end;
    double distance3d = 0.0;
    double horizontalDistance = 0.0;
    double verticalDelta = 0.0;
};

[[nodiscard]] DistanceMeasurement makeDistanceMeasurement(Vec3d start,
                                                          Vec3d end) noexcept;

} // namespace pci
