#include <pci/rendering/planning/MeasurementController.h>

#include <algorithm>

namespace pci {

std::optional<std::chrono::milliseconds>
MeasurementController::scheduleHover(const PixelPosition position,
                                     const TimePoint now) noexcept
{
    state_.hoverPosition = position;
    if (hoverScheduled_) {
        return std::nullopt;
    }
    hoverScheduled_ = true;
    if (!lastHoverQueuedAt_) {
        return hoverInterval;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - *lastHoverQueuedAt_);
    return std::max(std::chrono::milliseconds::zero(), hoverInterval - elapsed);
}

std::optional<PixelPosition>
MeasurementController::takeScheduledHover() noexcept
{
    if (!hoverScheduled_) {
        return std::nullopt;
    }
    hoverScheduled_ = false;
    return state_.hoverPosition;
}

void MeasurementController::setHoverPosition(
    const PixelPosition position) noexcept
{
    state_.hoverPosition = position;
}

void MeasurementController::recordHoverQueued(const std::uint64_t serial,
                                              const TimePoint now) noexcept
{
    if (serial == 0) {
        return;
    }
    latestHoverSerial_ = serial;
    lastHoverQueuedAt_ = now;
}

bool MeasurementController::acceptHoverResult(
    const std::uint64_t serial,
    const MeasurementRevisions requestRevisions,
    const MeasurementRevisions currentRevisions,
    std::optional<Vec3d> point) noexcept
{
    if (serial != latestHoverSerial_ || requestRevisions != currentRevisions) {
        return false;
    }
    state_.hover = point;
    hoverRevisions_ = currentRevisions;
    return true;
}

void MeasurementController::acceptCommitResult(
    std::optional<Vec3d> point, const MeasurementRevisions revisions) noexcept
{
    if (!point) {
        clearHover();
        return;
    }
    state_.hover = *point;
    hoverRevisions_ = revisions;
    if (state_.measurement) {
        state_.measurement.reset();
        state_.anchor = *point;
        return;
    }
    if (!state_.anchor) {
        state_.anchor = *point;
        return;
    }
    state_.measurement = makeDistanceMeasurement(*state_.anchor, *point);
    state_.anchor.reset();
}

void MeasurementController::invalidateHover(
    const MeasurementRevisions currentRevisions) noexcept
{
    if (state_.hover && hoverRevisions_ &&
        *hoverRevisions_ != currentRevisions) {
        clearHover();
    }
}

void MeasurementController::clearHover() noexcept
{
    state_.hover.reset();
    hoverRevisions_.reset();
}

void MeasurementController::clear() noexcept
{
    state_ = {};
    hoverRevisions_.reset();
    lastHoverQueuedAt_.reset();
    hoverScheduled_ = false;
    latestHoverSerial_ = 0;
}

const MeasurementViewState &MeasurementController::state() const noexcept
{
    return state_;
}

} // namespace pci
