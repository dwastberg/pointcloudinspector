#include <pci/navigation/QualificationCameraPath.h>

#include <algorithm>
#include <array>

namespace pci {
namespace {

constexpr std::array pathKeyframes{
    QualificationCameraKeyframe{
        .frameIndex = 0,
        .normalizedPivot = {0.5, 0.5, 0.5},
        .eyeOffset = {0.0, -1.40, 0.80},
        .phase = QualificationFramePhase::Warmup,
    },
    QualificationCameraKeyframe{
        .frameIndex = 29,
        .normalizedPivot = {0.5, 0.5, 0.5},
        .eyeOffset = {0.0, -1.40, 0.80},
        .phase = QualificationFramePhase::Warmup,
    },
    QualificationCameraKeyframe{
        .frameIndex = 30,
        .normalizedPivot = {0.5, 0.5, 0.5},
        .eyeOffset = {0.0, -1.40, 0.80},
        .phase = QualificationFramePhase::Interacting,
    },
    QualificationCameraKeyframe{
        .frameIndex = 89,
        .normalizedPivot = {0.25, 0.48, 0.5},
        .eyeOffset = {0.0, -1.10, 0.62},
        .phase = QualificationFramePhase::Interacting,
    },
    QualificationCameraKeyframe{
        .frameIndex = 149,
        .normalizedPivot = {0.75, 0.52, 0.5},
        .eyeOffset = {0.0, -0.92, 0.50},
        .phase = QualificationFramePhase::Interacting,
    },
    QualificationCameraKeyframe{
        .frameIndex = 239,
        .normalizedPivot = {0.68, 0.58, 0.5},
        .eyeOffset = {0.0, -0.32, 0.18},
        .phase = QualificationFramePhase::Interacting,
    },
    QualificationCameraKeyframe{
        .frameIndex = 240,
        .normalizedPivot = {0.68, 0.58, 0.5},
        .eyeOffset = {0.0, -0.32, 0.18},
        .phase = QualificationFramePhase::Dwell,
    },
    QualificationCameraKeyframe{
        .frameIndex = 359,
        .normalizedPivot = {0.68, 0.58, 0.5},
        .eyeOffset = {0.0, -0.32, 0.18},
        .phase = QualificationFramePhase::Dwell,
    },
};

Vec3d interpolate(const Vec3d from,
                  const Vec3d to,
                  const double fraction) noexcept
{
    return from + (to - from) * fraction;
}

Vec3d worldPivot(const Bounds3d &bounds, const Vec3d normalized) noexcept
{
    return {
        bounds.minimum[0] +
            (bounds.maximum[0] - bounds.minimum[0]) * normalized.x,
        bounds.minimum[1] +
            (bounds.maximum[1] - bounds.minimum[1]) * normalized.y,
        bounds.minimum[2] +
            (bounds.maximum[2] - bounds.minimum[2]) * normalized.z,
    };
}

} // namespace

QualificationCameraPath::QualificationCameraPath(Bounds3d sceneBounds)
    : sceneBounds_(sceneBounds)
{
    if (!sceneBounds_.valid() || sceneBounds_.maximumExtent() <= 0.0) {
        sceneBounds_ = {
            .minimum = {-1.0, -1.0, -1.0},
            .maximum = {1.0, 1.0, 1.0},
        };
    }
}

QualificationCameraFrame QualificationCameraPath::current() const noexcept
{
    const auto upper =
        std::ranges::lower_bound(pathKeyframes,
                                 frameIndex_,
                                 {},
                                 &QualificationCameraKeyframe::frameIndex);
    const QualificationCameraKeyframe &to =
        upper == pathKeyframes.end() ? pathKeyframes.back() : *upper;
    const QualificationCameraKeyframe &from =
        upper == pathKeyframes.begin() ? *upper : *(upper - 1);
    const std::uint32_t span = to.frameIndex - from.frameIndex;
    const double fraction =
        span == 0 ? 0.0
                  : static_cast<double>(frameIndex_ - from.frameIndex) /
                        static_cast<double>(span);
    const Vec3d normalizedPivot =
        interpolate(from.normalizedPivot, to.normalizedPivot, fraction);
    const Vec3d eyeOffset = interpolate(from.eyeOffset, to.eyeOffset, fraction);
    const Vec3d pivot = worldPivot(sceneBounds_, normalizedPivot);
    const double diameter = sceneBounds_.maximumExtent();
    return {
        .pose = {.position = pivot + eyeOffset * diameter, .pivot = pivot},
        // Phase transitions are keyframes in their own right, so using the
        // destination phase gives exact boundaries at frames 30 and 240.
        .phase = to.phase,
        .frameIndex = frameIndex_,
        .totalFrames = pathKeyframes.back().frameIndex + 1U,
        .finalFrame = frameIndex_ == pathKeyframes.back().frameIndex,
    };
}

bool QualificationCameraPath::advance() noexcept
{
    if (frameIndex_ >= pathKeyframes.back().frameIndex) {
        return false;
    }
    ++frameIndex_;
    return true;
}

std::span<const QualificationCameraKeyframe>
QualificationCameraPath::keyframes() noexcept
{
    return pathKeyframes;
}

const char *
qualificationFramePhaseName(const QualificationFramePhase phase) noexcept
{
    switch (phase) {
    case QualificationFramePhase::Warmup:
        return "warmup";
    case QualificationFramePhase::Interacting:
        return "interacting";
    case QualificationFramePhase::Dwell:
        return "dwell";
    }
    return "warmup";
}

} // namespace pci
