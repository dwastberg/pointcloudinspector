#pragma once

#include "foundation/Bounds3d.h"
#include "foundation/Vec3d.h"

#include <array>
#include <span>
#include <vector>

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
    [[nodiscard]] const std::array<Vec3d, 8> &corners() const noexcept
    {
        return corners_;
    }

    // Clips a convex polygon against all six planes, returning the surviving
    // convex region and an empty result when nothing is visible. Clipping a
    // convex polygon against six half-spaces yields at most 4 + 6 vertices, so
    // no general-polygon machinery is needed.
    //
    // The LOD planner uses this to derive a raster's visible region directly,
    // which is what makes planning cost proportional to visible screen
    // coverage rather than to catalog extent. intersects() answers a different
    // question and cannot express it.
    [[nodiscard]] std::vector<Vec3d>
    clipConvexPolygon(std::span<const Vec3d> polygon) const;

private:
    struct Plane {
        Vec3d normal;    // points into the visible half-space
        double distance; // signedDistance(p) = dot(normal, p) + distance
    };

    std::array<Plane, 6> planes_{};
    std::array<Vec3d, 8> corners_{};
};

} // namespace pci
