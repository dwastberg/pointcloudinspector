#include "renderer/planning/FrustumCuller.h"

#include <cmath>
#include <numbers>

namespace pci {

FrustumCuller FrustumCuller::fromCamera(const Vec3d position,
                                        const Vec3d forward,
                                        const Vec3d up,
                                        const Vec3d right,
                                        const double verticalFovDegrees,
                                        const double aspect,
                                        const double nearPlane,
                                        const double farPlane) noexcept
{
    const double halfVertical =
        std::tan(verticalFovDegrees * 0.5 * std::numbers::pi / 180.0);
    const double halfHorizontal = halfVertical * aspect;

    FrustumCuller culler;
    auto plane = [](const Vec3d normal, const Vec3d point) {
        const Vec3d unit = normalized(normal);
        return Plane{unit, -dot(unit, point)};
    };
    culler.planes_[0] = plane(forward, position + forward * nearPlane);
    culler.planes_[1] = plane(forward * -1.0, position + forward * farPlane);
    // Inward side-plane normals: tan(half-angle)*forward -/+ axis.
    culler.planes_[2] = plane(forward * halfVertical - up, position);
    culler.planes_[3] = plane(forward * halfVertical + up, position);
    culler.planes_[4] = plane(forward * halfHorizontal - right, position);
    culler.planes_[5] = plane(forward * halfHorizontal + right, position);
    return culler;
}

FrustumCuller FrustumCuller::fromOrthographic(const Vec3d position,
                                              const Vec3d forward,
                                              const Vec3d up,
                                              const Vec3d right,
                                              const double halfVertical,
                                              const double aspect,
                                              const double nearPlane,
                                              const double farPlane) noexcept
{
    const double halfHorizontal = halfVertical * aspect;
    FrustumCuller culler;
    auto plane = [](const Vec3d normal, const Vec3d point) {
        const Vec3d unit = normalized(normal);
        return Plane{unit, -dot(unit, point)};
    };
    culler.planes_[0] = plane(forward, position + forward * nearPlane);
    culler.planes_[1] = plane(forward * -1.0, position + forward * farPlane);
    culler.planes_[2] = plane(up, position - up * halfVertical);
    culler.planes_[3] = plane(up * -1.0, position + up * halfVertical);
    culler.planes_[4] = plane(right, position - right * halfHorizontal);
    culler.planes_[5] = plane(right * -1.0, position + right * halfHorizontal);
    return culler;
}

bool FrustumCuller::intersects(const Bounds3d &bounds) const noexcept
{
    const Vec3d center{
        (bounds.minimum[0] + bounds.maximum[0]) * 0.5,
        (bounds.minimum[1] + bounds.maximum[1]) * 0.5,
        (bounds.minimum[2] + bounds.maximum[2]) * 0.5,
    };
    const Vec3d halfExtent{
        (bounds.maximum[0] - bounds.minimum[0]) * 0.5,
        (bounds.maximum[1] - bounds.minimum[1]) * 0.5,
        (bounds.maximum[2] - bounds.minimum[2]) * 0.5,
    };
    for (const Plane &plane : planes_) {
        const double radius = halfExtent.x * std::abs(plane.normal.x) +
                              halfExtent.y * std::abs(plane.normal.y) +
                              halfExtent.z * std::abs(plane.normal.z);
        if (dot(plane.normal, center) + plane.distance < -radius) {
            return false;
        }
    }
    return true;
}

} // namespace pci
