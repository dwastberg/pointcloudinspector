#pragma once

#include <pci/rendering/planning/FrustumCuller.h>

#include <cstdint>

namespace pci {

// Immutable per-frame camera inputs shared by point planning and vector
// overlay culling. Dimensions are render-target pixels, never logical/DPR
// scaled widget units.
struct FrameCamera {
    Vec3d eye;
    Vec3d forward;
    Vec3d up;
    Vec3d right;
    FrustumCuller culler;
    int outputWidth = 1;
    int outputHeight = 1;
    double nearPlane = 0.0;
    double farPlane = 0.0;
    double verticalFovDegrees = 0.0;
    double orthographicScale = 0.0;
    bool orthographic = false;

    [[nodiscard]] double worldUnitsPerPixelAtDepth(double depth) const noexcept;
    [[nodiscard]] float shaderNearPlaneW() const noexcept;
};

} // namespace pci
