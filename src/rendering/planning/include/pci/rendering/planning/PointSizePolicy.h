#pragma once

#include <pci/rendering/planning/FrameCamera.h>

namespace pci {

// Projects a representative world-space point spacing into screen pixels.
// The calculation supports both perspective and orthographic cameras and is
// shared by point rendering and its policy tests.
[[nodiscard]] double
projectedPointSpacingPixels(const Bounds3d &bounds,
                            double pointSpacing,
                            const FrameCamera &camera) noexcept;

// Converts projected spacing to a bounded sprite size. coverageFactor is
// greater than one for non-spatially-stratified hierarchy samples; userScale
// is the user's point-size preference normalized so the default is 1.0.
[[nodiscard]] float adaptivePointSizePixels(const Bounds3d &bounds,
                                            double pointSpacing,
                                            double coverageFactor,
                                            double userScale,
                                            float minimumPixels,
                                            float maximumPixels,
                                            const FrameCamera &camera) noexcept;

} // namespace pci
