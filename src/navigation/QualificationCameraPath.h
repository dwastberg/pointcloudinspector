#pragma once

#include "foundation/Bounds3d.h"
#include "foundation/Vec3d.h"

#include <cstdint>
#include <span>

namespace pci {

enum class QualificationFramePhase : std::uint8_t {
    Warmup,
    Interacting,
    Dwell,
};

struct QualificationCameraPose {
    Vec3d position;
    Vec3d pivot;
};

struct QualificationCameraKeyframe {
    std::uint32_t frameIndex = 0;
    // Fractions of the source-bounds span. (0.5, 0.5, 0.5) is the scene
    // centre, independent of source coordinates and units.
    Vec3d normalizedPivot;
    // Offset from the pivot in units of the scene's maximum extent.
    Vec3d eyeOffset;
    QualificationFramePhase phase = QualificationFramePhase::Interacting;
};

struct QualificationCameraFrame {
    QualificationCameraPose pose;
    QualificationFramePhase phase = QualificationFramePhase::Warmup;
    std::uint32_t frameIndex = 0;
    std::uint32_t totalFrames = 0;
    bool finalFrame = false;
};

// Frame-indexed camera replay used by native qualification runs. The path is
// scene-relative so the same definition works for local test fixtures and the
// real six-LAS corpus without embedding machine-specific world coordinates.
class QualificationCameraPath final {
public:
    explicit QualificationCameraPath(Bounds3d sceneBounds);

    [[nodiscard]] QualificationCameraFrame current() const noexcept;
    [[nodiscard]] bool advance() noexcept;

    [[nodiscard]] static std::span<const QualificationCameraKeyframe>
    keyframes() noexcept;

private:
    Bounds3d sceneBounds_;
    std::uint32_t frameIndex_ = 0;
};

[[nodiscard]] const char *
qualificationFramePhaseName(QualificationFramePhase phase) noexcept;

} // namespace pci
