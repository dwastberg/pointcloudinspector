#pragma once

namespace pci {

inline constexpr int minimumPointSizePixels = 1;
inline constexpr int maximumPointSizePixels = 8;
inline constexpr int defaultPointSizePixels = 2;

inline constexpr float defaultBackgroundRed = 0.015F;
inline constexpr float defaultBackgroundGreen = 0.02F;
inline constexpr float defaultBackgroundBlue = 0.035F;
inline constexpr float minimumDepthEnhancementRadius = 0.5F;
inline constexpr float maximumDepthEnhancementRadius = 5.0F;
inline constexpr float defaultDepthEnhancementRadius = 1.0F;
inline constexpr float minimumDepthEnhancementStrength = 0.0F;
inline constexpr float maximumDepthEnhancementStrength = 100.0F;
inline constexpr float defaultDepthEnhancementStrength = 25.0F;

struct ViewportColor {
    float red = defaultBackgroundRed;
    float green = defaultBackgroundGreen;
    float blue = defaultBackgroundBlue;

    bool operator==(const ViewportColor &) const = default;
};

struct DepthEnhancementSettings {
    bool enabled = true;
    float radius = defaultDepthEnhancementRadius;
    float strength = defaultDepthEnhancementStrength;

    bool operator==(const DepthEnhancementSettings &) const = default;
};

struct ViewportSettings {
    ViewportColor backgroundColor;
    DepthEnhancementSettings depthEnhancement;

    bool operator==(const ViewportSettings &) const = default;
};

} // namespace pci
