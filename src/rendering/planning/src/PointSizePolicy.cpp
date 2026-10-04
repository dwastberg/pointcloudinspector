#include <pci/rendering/planning/PointSizePolicy.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pci {

double projectedPointSpacingPixels(const Bounds3d &bounds,
                                   const double pointSpacing,
                                   const FrameCamera &camera) noexcept
{
    if (!bounds.valid() || !std::isfinite(pointSpacing) ||
        pointSpacing <= 0.0 || camera.outputHeight <= 0) {
        return 0.0;
    }
    if (camera.orthographic) {
        if (!std::isfinite(camera.orthographicScale) ||
            camera.orthographicScale <= 0.0) {
            return 0.0;
        }
        return pointSpacing * static_cast<double>(camera.outputHeight) /
               camera.orthographicScale;
    }
    if (!isFinite(camera.eye) || !std::isfinite(camera.verticalFovDegrees) ||
        camera.verticalFovDegrees <= 0.0 ||
        camera.verticalFovDegrees >= 180.0) {
        return 0.0;
    }

    const auto centerArray = bounds.center();
    const Vec3d center{centerArray[0], centerArray[1], centerArray[2]};
    const Vec3d diagonal{
        bounds.maximum[0] - bounds.minimum[0],
        bounds.maximum[1] - bounds.minimum[1],
        bounds.maximum[2] - bounds.minimum[2],
    };
    const double radius = length(diagonal) * 0.5;
    const double distance = std::max(length(center - camera.eye) - radius,
                                     std::max(pointSpacing * 0.25, 1e-9));
    const double halfFovRadians =
        camera.verticalFovDegrees * std::numbers::pi / 360.0;
    const double focalPixels = static_cast<double>(camera.outputHeight) /
                               (2.0 * std::tan(halfFovRadians));
    const double result = pointSpacing * focalPixels / distance;
    return std::isfinite(result) && result > 0.0 ? result : 0.0;
}

float adaptivePointSizePixels(const Bounds3d &bounds,
                              const double pointSpacing,
                              const double coverageFactor,
                              const double userScale,
                              const float minimumPixels,
                              const float maximumPixels,
                              const FrameCamera &camera) noexcept
{
    if (!std::isfinite(minimumPixels) || !std::isfinite(maximumPixels) ||
        minimumPixels <= 0.0F || maximumPixels < minimumPixels) {
        return 1.0F;
    }
    const double projected =
        projectedPointSpacingPixels(bounds, pointSpacing, camera);
    const double requested = projected * coverageFactor * userScale;
    if (!std::isfinite(requested) || requested <= 0.0) {
        return minimumPixels;
    }
    return static_cast<float>(std::clamp(requested,
                                         static_cast<double>(minimumPixels),
                                         static_cast<double>(maximumPixels)));
}

} // namespace pci
