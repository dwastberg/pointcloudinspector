#pragma once

#include <span>

namespace pci {

struct PointRgba {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 1.0F;

    bool operator==(const PointRgba &) const = default;
};

struct PointColorStop {
    float position = 0.0F;
    PointRgba color;
};

// The caller owns the meaning of an empty ramp. Point color maps use a
// transparent black fallback, while raster scalar display uses grayscale.
// Separate overloads preserve each caller's established arithmetic precision.
[[nodiscard]] PointRgba
interpolateColorRamp(std::span<const PointColorStop> stops,
                     float position,
                     PointRgba emptyFallback) noexcept;
[[nodiscard]] PointRgba
interpolateColorRamp(std::span<const PointColorStop> stops,
                     double position,
                     PointRgba emptyFallback) noexcept;

} // namespace pci
