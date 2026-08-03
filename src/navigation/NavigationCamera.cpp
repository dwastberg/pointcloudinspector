#include "navigation/NavigationCamera.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pci {
namespace {

constexpr double degreesPerPixel = 0.25;
constexpr double maximumPitchDegrees = 89.9;

[[nodiscard]] double radians(const double degrees) noexcept
{
    return degrees * std::numbers::pi / 180.0;
}

} // namespace

Vec3d NavigationCamera::position() const noexcept
{
    return position_;
}

Vec3d NavigationCamera::pivot() const noexcept
{
    return pivot_;
}

Vec3d NavigationCamera::forward() const noexcept
{
    return forward_;
}

Vec3d NavigationCamera::right() const noexcept
{
    const Vec3d result = normalized(cross(forward_, NavigationCamera::worldUp));
    return length(result) > 0.0 ? result : Vec3d{1.0, 0.0, 0.0};
}

Vec3d NavigationCamera::up() const noexcept
{
    return normalized(cross(right(), forward_));
}

std::uint64_t NavigationCamera::revision() const noexcept
{
    return revision_;
}

void NavigationCamera::setScene(const Vec3d center,
                                const double diameter) noexcept
{
    if (!isFinite(center) || !std::isfinite(diameter) || diameter <= 0.0) {
        return;
    }
    sceneCenter_ = center;
    sceneDiameter_ = diameter;
    markChanged();
}

double NavigationCamera::sceneDiameter() const noexcept
{
    return sceneDiameter_;
}

void NavigationCamera::frameScene() noexcept
{
    frameScene(2.0);
}

void NavigationCamera::frameScene(const double distanceMultiplier) noexcept
{
    if (!std::isfinite(distanceMultiplier) || distanceMultiplier <= 0.0) {
        return;
    }
    position_ =
        sceneCenter_ + Vec3d{0.0, -distanceMultiplier * sceneDiameter_, 0.0};
    pivot_ = sceneCenter_;
    forward_ = {0.0, 1.0, 0.0};
    navigationReference_.reset();
    orthographicScale_ = sceneDiameter_ * distanceMultiplier;
    markChanged();
}

void NavigationCamera::frameTopDown(const double distanceMultiplier) noexcept
{
    if (!std::isfinite(distanceMultiplier) || distanceMultiplier <= 0.0) {
        return;
    }
    position_ =
        sceneCenter_ + Vec3d{0.0, 0.0, distanceMultiplier * sceneDiameter_};
    pivot_ = sceneCenter_;
    forward_ = {0.0, 0.0, -1.0};
    navigationReference_.reset();
    orthographicScale_ = sceneDiameter_ * distanceMultiplier;
    markChanged();
}

void NavigationCamera::setOrthographic(const bool enabled) noexcept
{
    if (orthographic_ == enabled) {
        return;
    }
    const double halfFovTangent =
        std::tan(radians(verticalFieldOfViewDegrees * 0.5));
    if (enabled) {
        // Match the current perspective distance with the equivalent
        // orthographic vertical span.
        orthographicScale_ = std::max(
            2.0 * std::max(length(position_ - pivot_), sceneDiameter_ * 1e-7) *
                halfFovTangent,
            sceneDiameter_ * 1e-5);
    } else {
        // Restore the perspective camera distance that produces the same
        // vertical framing as the current orthographic scale.
        const double distance = std::max(
            orthographicScale_ / (2.0 * halfFovTangent), sceneDiameter_ * 1e-7);
        const Vec3d offset = normalized(position_ - pivot_);
        const Vec3d direction = length(offset) > 0.0 ? offset : forward_ * -1.0;
        position_ = pivot_ + direction * distance;
    }
    orthographic_ = enabled;
    markChanged();
}

bool NavigationCamera::isOrthographic() const noexcept
{
    return orthographic_;
}

double NavigationCamera::orthographicScale() const noexcept
{
    return orthographicScale_;
}

void NavigationCamera::setPivot(const Vec3d pivot) noexcept
{
    if (!isFinite(pivot)) {
        return;
    }
    pivot_ = pivot;
    markChanged();
}

void NavigationCamera::setNavigationReference(const Vec3d point) noexcept
{
    if (!isFinite(point)) {
        return;
    }
    navigationReference_ = point;
    markChanged();
}

void NavigationCamera::orbitFromDrag(const double horizontalPixels,
                                     const double verticalPixels) noexcept
{
    if (!std::isfinite(horizontalPixels) || !std::isfinite(verticalPixels)) {
        return;
    }

    Vec3d offset = position_ - pivot_;
    Vec3d newForward = forward_;
    const double yawRadians = radians(-horizontalPixels * degreesPerPixel);
    offset = rotateAroundAxis(offset, NavigationCamera::worldUp, yawRadians);
    newForward = normalized(
        rotateAroundAxis(newForward, NavigationCamera::worldUp, yawRadians));

    const double currentPitchDegrees =
        std::asin(std::clamp(-newForward.z, -1.0, 1.0)) * 180.0 /
        std::numbers::pi;
    const double requestedPitchDegrees =
        currentPitchDegrees + verticalPixels * degreesPerPixel;
    const double clampedPitchDegrees = std::clamp(
        requestedPitchDegrees, -maximumPitchDegrees, maximumPitchDegrees);
    const double appliedPitchRadians =
        radians(-(clampedPitchDegrees - currentPitchDegrees));
    Vec3d pitchAxis = normalized(cross(newForward, NavigationCamera::worldUp));
    if (length(pitchAxis) == 0.0) {
        pitchAxis = {1.0, 0.0, 0.0};
    }
    offset = rotateAroundAxis(offset, pitchAxis, appliedPitchRadians);
    newForward = normalized(
        rotateAroundAxis(newForward, pitchAxis, appliedPitchRadians));

    const Vec3d newPosition = pivot_ + offset;
    if (!isFinite(newPosition) || !isFinite(newForward)) {
        return;
    }
    position_ = newPosition;
    forward_ = newForward;
    markChanged();
}

void NavigationCamera::panFromDrag(const double horizontalPixels,
                                   const double verticalPixels,
                                   const double viewportHeightPixels) noexcept
{
    if (!std::isfinite(horizontalPixels) || !std::isfinite(verticalPixels) ||
        !std::isfinite(viewportHeightPixels) || viewportHeightPixels <= 0.0) {
        return;
    }

    const double unitsPerPixel =
        orthographic_
            ? std::max(orthographicScale_, sceneDiameter_ * 1e-7) /
                  viewportHeightPixels
            : 2.0 *
                  std::max(length(position_ - pivot_), sceneDiameter_ * 1e-7) *
                  std::tan(radians(verticalFieldOfViewDegrees * 0.5)) /
                  viewportHeightPixels;
    translate(right() * (-horizontalPixels * unitsPerPixel) +
              up() * (verticalPixels * unitsPerPixel));
}

void NavigationCamera::translate(const Vec3d worldDelta) noexcept
{
    if (!isFinite(worldDelta)) {
        return;
    }
    const Vec3d newPosition = position_ + worldDelta;
    const Vec3d newPivot = pivot_ + worldDelta;
    if (!isFinite(newPosition) || !isFinite(newPivot)) {
        return;
    }
    position_ = newPosition;
    pivot_ = newPivot;
    markChanged();
}

void NavigationCamera::dollyToward(const Vec3d target,
                                   const double wheelUnits) noexcept
{
    if (!isFinite(target) || !std::isfinite(wheelUnits)) {
        return;
    }
    if (orthographic_) {
        const double exponent = std::clamp(-0.25 * wheelUnits, -80.0, 80.0);
        orthographicScale_ = std::clamp(orthographicScale_ * std::exp(exponent),
                                        sceneDiameter_ * 1e-5,
                                        sceneDiameter_ * 1e5);
        navigationReference_ = target;
        markChanged();
        return;
    }
    const Vec3d offset = position_ - target;
    if (length(offset) == 0.0) {
        dollyForward(wheelUnits);
        return;
    }
    const double exponent = std::clamp(-0.25 * wheelUnits, -80.0, 80.0);
    const Vec3d newPosition = target + offset * std::exp(exponent);
    if (!isFinite(newPosition)) {
        return;
    }
    position_ = newPosition;
    navigationReference_ = target;
    markChanged();
}

void NavigationCamera::dollyForward(const double wheelUnits) noexcept
{
    if (!std::isfinite(wheelUnits)) {
        return;
    }
    if (orthographic_) {
        const double exponent = std::clamp(-0.25 * wheelUnits, -80.0, 80.0);
        orthographicScale_ = std::clamp(orthographicScale_ * std::exp(exponent),
                                        sceneDiameter_ * 1e-5,
                                        sceneDiameter_ * 1e5);
        markChanged();
        return;
    }
    const double scale =
        std::max(length(position_ - pivot_), sceneDiameter_ * 1e-5);
    const double exponent = std::clamp(-0.25 * wheelUnits, -80.0, 80.0);
    const Vec3d newPosition =
        position_ + forward_ * scale * (1.0 - std::exp(exponent));
    if (!isFinite(newPosition)) {
        return;
    }
    position_ = newPosition;
    markChanged();
}

double NavigationCamera::movementSpeed(const double multiplier) const noexcept
{
    if (!std::isfinite(multiplier) || multiplier < 0.0) {
        return 0.0;
    }
    return std::clamp(length(position_ - pivot_),
                      sceneDiameter_ * 1e-5,
                      sceneDiameter_ * 2.0) *
           multiplier;
}

NavigationCamera::ClipPlanes NavigationCamera::clipPlanes() const noexcept
{
    const Vec3d reference = navigationReference_.value_or(pivot_);
    const double referenceDistance = length(position_ - reference);
    const double nearPlane = std::clamp(referenceDistance * 0.01,
                                        sceneDiameter_ * 1e-7,
                                        sceneDiameter_ * 0.005);
    constexpr double halfSqrt3 = 0.8660254037844386;
    const double sceneRadius = sceneDiameter_ * halfSqrt3;
    const double farPlane = std::max(sceneDiameter_ * 4.0,
                                     length(position_ - sceneCenter_) +
                                         sceneRadius + sceneDiameter_);
    return {
        .nearPlane = nearPlane,
        .farPlane = std::max(farPlane, nearPlane * 2.0),
    };
}

void NavigationCamera::markChanged() noexcept
{
    ++revision_;
}

} // namespace pci
