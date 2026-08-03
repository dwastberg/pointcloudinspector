#pragma once

#include "vector/VectorGeometry.h"

#include <cstdint>

namespace pci {

struct VectorRgba {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 1.0F;
    bool operator==(const VectorRgba &) const = default;
};

enum class VectorMarkerShape : std::int32_t {
    Circle = 0,
    Square = 1
};
static_assert(static_cast<int>(VectorMarkerShape::Circle) == 0);
static_assert(static_cast<int>(VectorMarkerShape::Square) == 1);

inline constexpr float maximumVectorStrokeWidthPixels = 20.0F;
inline constexpr float maximumVectorMarkerSizePixels = 64.0F;
inline constexpr double maximumVectorZOffsetMagnitude = 1.0e6;

struct VectorLayerStyle {
    VectorRgba fill{0.298F, 0.553F, 1.0F, 0.35F};
    VectorRgba stroke{0.298F, 0.553F, 1.0F, 1.0F};
    VectorRgba marker{1.0F, 0.72F, 0.20F, 1.0F};
    float strokeWidthPixels = 1.5F;
    float markerSizePixels = 6.0F;
    VectorMarkerShape markerShape = VectorMarkerShape::Circle;
    float opacity = 1.0F;
    double zOffset = 0.0;
    bool alwaysOnTop = false;
    bool operator==(const VectorLayerStyle &) const = default;
};

[[nodiscard]] VectorLayerStyle
defaultVectorLayerStyle(VectorGeometryKind kind) noexcept;
[[nodiscard]] VectorLayerStyle
clampVectorLayerStyle(VectorLayerStyle style) noexcept;

} // namespace pci
