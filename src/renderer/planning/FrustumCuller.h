#pragma once

#include "foundation/Bounds3d.h"
#include "foundation/Vec3d.h"

#include <array>

namespace pci {

class FrustumCuller {
public:
    [[nodiscard]] static FrustumCuller fromCamera(Vec3d position,
                                                  Vec3d forward,
                                                  Vec3d up,
                                                  Vec3d right,
                                                  double verticalFovDegrees,
                                                  double aspect,
                                                  double nearPlane,
                                                  double farPlane) noexcept;
    [[nodiscard]] static FrustumCuller
    fromOrthographic(Vec3d position,
                     Vec3d forward,
                     Vec3d up,
                     Vec3d right,
                     double halfVertical,
                     double aspect,
                     double nearPlane,
                     double farPlane) noexcept;

    [[nodiscard]] bool intersects(const Bounds3d &bounds) const noexcept;

private:
    struct Plane {
        Vec3d normal;    // points into the visible half-space
        double distance; // signedDistance(p) = dot(normal, p) + distance
    };

    std::array<Plane, 6> planes_{};
};

} // namespace pci
