#include "vector/VectorLayerStyle.h"

#include <algorithm>
#include <cmath>

namespace pci {
namespace {

[[nodiscard]] float clampChannel(const float value,
                                 const float fallback) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : fallback;
}

[[nodiscard]] VectorRgba clampColor(const VectorRgba value,
                                    const VectorRgba fallback) noexcept
{
    return {
        .red = clampChannel(value.red, fallback.red),
        .green = clampChannel(value.green, fallback.green),
        .blue = clampChannel(value.blue, fallback.blue),
        .alpha = clampChannel(value.alpha, fallback.alpha),
    };
}

[[nodiscard]] float clampFinite(const float value,
                                const float minimum,
                                const float maximum,
                                const float fallback) noexcept
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum)
                                : fallback;
}

} // namespace

VectorLayerStyle defaultVectorLayerStyle(const VectorGeometryKind kind) noexcept
{
    VectorLayerStyle result;
    if (kind == VectorGeometryKind::Line) {
        result.fill.alpha = 0.0F;
    } else if (kind == VectorGeometryKind::Point) {
        result.fill.alpha = 0.0F;
        result.stroke.alpha = 0.0F;
        result.markerSizePixels = 7.0F;
    }
    return result;
}

VectorLayerStyle clampVectorLayerStyle(VectorLayerStyle style) noexcept
{
    const VectorLayerStyle defaults;
    style.fill = clampColor(style.fill, defaults.fill);
    style.stroke = clampColor(style.stroke, defaults.stroke);
    style.marker = clampColor(style.marker, defaults.marker);
    style.strokeWidthPixels = clampFinite(style.strokeWidthPixels,
                                          0.0F,
                                          maximumVectorStrokeWidthPixels,
                                          defaults.strokeWidthPixels);
    style.markerSizePixels = clampFinite(style.markerSizePixels,
                                         1.0F,
                                         maximumVectorMarkerSizePixels,
                                         defaults.markerSizePixels);
    style.opacity = clampFinite(style.opacity, 0.0F, 1.0F, defaults.opacity);
    style.zOffset = std::isfinite(style.zOffset)
                        ? std::clamp(style.zOffset,
                                     -maximumVectorZOffsetMagnitude,
                                     maximumVectorZOffsetMagnitude)
                        : defaults.zOffset;
    if (style.markerShape != VectorMarkerShape::Circle &&
        style.markerShape != VectorMarkerShape::Square) {
        style.markerShape = defaults.markerShape;
    }
    return style;
}

} // namespace pci
