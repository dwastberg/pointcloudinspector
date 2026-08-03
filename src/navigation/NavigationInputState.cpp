#include "navigation/NavigationInputState.h"

#include <algorithm>
#include <cmath>

namespace pci {

void NavigationInputState::press(const MovementKey key) noexcept
{
    if (key != MovementKey::Count) {
        movement_[index(key)] = true;
    }
}

void NavigationInputState::release(const MovementKey key) noexcept
{
    if (key != MovementKey::Count) {
        movement_[index(key)] = false;
    }
}

void NavigationInputState::clearMovement() noexcept
{
    movement_.fill(false);
    fast_ = false;
    fine_ = false;
}

void NavigationInputState::setFast(const bool enabled) noexcept
{
    fast_ = enabled;
}

void NavigationInputState::setFine(const bool enabled) noexcept
{
    fine_ = enabled;
}

Vec3d NavigationInputState::movementDirection() const noexcept
{
    Vec3d direction{
        .x = (movement_[index(MovementKey::Right)] ? 1.0 : 0.0) -
             (movement_[index(MovementKey::Left)] ? 1.0 : 0.0),
        .y = (movement_[index(MovementKey::Up)] ? 1.0 : 0.0) -
             (movement_[index(MovementKey::Down)] ? 1.0 : 0.0),
        .z = (movement_[index(MovementKey::Forward)] ? 1.0 : 0.0) -
             (movement_[index(MovementKey::Backward)] ? 1.0 : 0.0),
    };
    return normalized(direction);
}

bool NavigationInputState::hasMovement() const noexcept
{
    return std::ranges::any_of(movement_, [](const bool pressed) {
        return pressed;
    });
}

double NavigationInputState::speedMultiplier() const noexcept
{
    return (fast_ ? 5.0 : 1.0) * (fine_ ? 0.1 : 1.0);
}

double NavigationInputState::boundedDeltaSeconds(const double seconds) noexcept
{
    if (!std::isfinite(seconds)) {
        return 0.0;
    }
    return std::clamp(seconds, 0.0, 0.1);
}

void NavigationInputState::queueWheel(const PixelPosition position,
                                      const double wheelUnits) noexcept
{
    if (!std::isfinite(wheelUnits) || wheelUnits == 0.0) {
        return;
    }
    if (pendingPick_) {
        if (pendingPick_->kind == PickKind::MeasureHover) {
            pendingPick_ = PickRequest{
                .kind = PickKind::Wheel,
                .position = position,
                .wheelUnits = wheelUnits,
                .serial = ++requestSerial_,
            };
            return;
        }
        if (pendingPick_->kind != PickKind::Wheel) {
            return;
        }
        pendingPick_->position = position;
        pendingPick_->wheelUnits += wheelUnits;
        pendingPick_->serial = ++requestSerial_;
        return;
    }
    pendingPick_ = PickRequest{
        .kind = PickKind::Wheel,
        .position = position,
        .wheelUnits = wheelUnits,
        .serial = ++requestSerial_,
    };
}

void NavigationInputState::queuePivot(const PixelPosition position) noexcept
{
    pendingPick_ = PickRequest{
        .kind = PickKind::Pivot,
        .position = position,
        .serial = ++requestSerial_,
    };
}

std::uint64_t
NavigationInputState::queueMeasureHover(const PixelPosition position) noexcept
{
    if (pendingPick_ && pendingPick_->kind != PickKind::MeasureHover) {
        return 0;
    }
    const std::uint64_t serial = ++requestSerial_;
    pendingPick_ = PickRequest{
        .kind = PickKind::MeasureHover,
        .position = position,
        .serial = serial,
    };
    return serial;
}

std::uint64_t
NavigationInputState::queueMeasureCommit(const PixelPosition position) noexcept
{
    if (pendingPick_ && pendingPick_->kind == PickKind::Raw) {
        return 0;
    }
    const std::uint64_t serial = ++requestSerial_;
    pendingPick_ = PickRequest{
        .kind = PickKind::MeasureCommit,
        .position = position,
        .serial = serial,
    };
    return serial;
}

void NavigationInputState::queueRaw(const PixelPosition position) noexcept
{
    pendingPick_ = PickRequest{
        .kind = PickKind::Raw,
        .position = position,
        .serial = ++requestSerial_,
    };
}

const std::optional<PickRequest> &
NavigationInputState::pendingPick() const noexcept
{
    return pendingPick_;
}

bool NavigationInputState::hasInFlightPick() const noexcept
{
    return inFlightPick_.has_value();
}

std::optional<PickRequest> NavigationInputState::takePendingPick(
    const std::uint64_t cameraRevision,
    const std::uint64_t documentRevision,
    const std::uint64_t selectionGeneration) noexcept
{
    if (inFlightPick_ || !pendingPick_) {
        return std::nullopt;
    }
    pendingPick_->cameraRevision = cameraRevision;
    pendingPick_->documentRevision = documentRevision;
    pendingPick_->selectionGeneration = selectionGeneration;
    pendingPick_->inputGeneration = pickGeneration_;
    inFlightPick_ = pendingPick_;
    pendingPick_.reset();
    return inFlightPick_;
}

void NavigationInputState::completeInFlight() noexcept
{
    inFlightPick_.reset();
}

bool NavigationInputState::isCurrentPick(
    const PickRequest &request) const noexcept
{
    return inFlightPick_ &&
           inFlightPick_->inputGeneration == request.inputGeneration &&
           inFlightPick_->serial == request.serial &&
           request.inputGeneration == pickGeneration_;
}

void NavigationInputState::markStale(const PickRequest &request) noexcept
{
    inFlightPick_.reset();
    if (request.inputGeneration != pickGeneration_) {
        return;
    }
    PickRequest stale = request;
    stale.cameraRevision = 0;
    stale.documentRevision = 0;
    stale.selectionGeneration = 0;

    if (!pendingPick_) {
        pendingPick_ = stale;
        return;
    }
    if (pendingPick_->kind != PickKind::Wheel) {
        return;
    }
    if (stale.kind != PickKind::Wheel) {
        pendingPick_ = stale;
        return;
    }
    pendingPick_->wheelUnits += stale.wheelUnits;
}

void NavigationInputState::cancelPicks() noexcept
{
    ++pickGeneration_;
    pendingPick_.reset();
    inFlightPick_.reset();
}

std::size_t NavigationInputState::index(const MovementKey key) noexcept
{
    return static_cast<std::size_t>(key);
}

} // namespace pci
