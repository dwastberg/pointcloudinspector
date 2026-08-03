#include "renderer/planning/FrameCamera.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pci {

double FrameCamera::worldUnitsPerPixelAtDepth(const double depth) const noexcept
{
    const double height = static_cast<double>(std::max(1, outputHeight));
    if (orthographic) {
        return std::isfinite(orthographicScale) && orthographicScale > 0.0
                   ? orthographicScale / height
                   : 0.0;
    }
    if (!std::isfinite(depth) || depth <= 0.0 ||
        !std::isfinite(verticalFovDegrees)) {
        return 0.0;
    }
    return 2.0 * std::tan(verticalFovDegrees * std::numbers::pi / 360.0) *
           depth / height;
}

float FrameCamera::shaderNearPlaneW() const noexcept
{
    return orthographic ? 0.0F : static_cast<float>(nearPlane);
}

} // namespace pci
