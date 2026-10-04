#pragma once

#include <pci/foundation/Vec3d.h>

#include <array>
#include <cstdint>
#include <optional>

namespace pci {

enum class MovementKey : std::uint8_t {
    Forward,
    Backward,
    Left,
    Right,
    Down,
    Up,
    Count,
};

enum class PickKind : std::uint8_t {
    Wheel,
    Pivot,
    MeasureHover,
    MeasureCommit,
    Raw,
};

struct PixelPosition {
    int x = 0;
    int y = 0;

    bool operator==(const PixelPosition &) const = default;
};

struct PickRequest {
    PickKind kind = PickKind::Wheel;
    PixelPosition position;
    double wheelUnits = 0.0;
    std::uint64_t cameraRevision = 0;
    std::uint64_t documentRevision = 0;
    std::uint64_t selectionGeneration = 0;
    std::uint64_t inputGeneration = 0;
    std::uint64_t serial = 0;
};

class NavigationInputState {
public:
    void press(MovementKey key) noexcept;
    void release(MovementKey key) noexcept;
    void clearMovement() noexcept;
    void setFast(bool enabled) noexcept;
    void setFine(bool enabled) noexcept;

    [[nodiscard]] Vec3d movementDirection() const noexcept;
    [[nodiscard]] bool hasMovement() const noexcept;
    [[nodiscard]] double speedMultiplier() const noexcept;
    [[nodiscard]] static double boundedDeltaSeconds(double seconds) noexcept;

    void queueWheel(PixelPosition position, double wheelUnits) noexcept;
    void queuePivot(PixelPosition position) noexcept;
    [[nodiscard]] std::uint64_t
    queueMeasureHover(PixelPosition position) noexcept;
    [[nodiscard]] std::uint64_t
    queueMeasureCommit(PixelPosition position) noexcept;
    void queueRaw(PixelPosition position) noexcept;
    [[nodiscard]] const std::optional<PickRequest> &
    pendingPick() const noexcept;
    [[nodiscard]] bool hasInFlightPick() const noexcept;
    [[nodiscard]] std::optional<PickRequest>
    takePendingPick(std::uint64_t cameraRevision,
                    std::uint64_t documentRevision,
                    std::uint64_t selectionGeneration) noexcept;
    void completeInFlight() noexcept;
    [[nodiscard]] bool isCurrentPick(const PickRequest &request) const noexcept;
    void markStale(const PickRequest &request) noexcept;
    void cancelPicks() noexcept;

private:
    [[nodiscard]] static std::size_t index(MovementKey key) noexcept;

    std::array<bool, static_cast<std::size_t>(MovementKey::Count)> movement_{};
    bool fast_ = false;
    bool fine_ = false;
    std::optional<PickRequest> pendingPick_;
    std::optional<PickRequest> inFlightPick_;
    std::uint64_t pickGeneration_ = 0;
    std::uint64_t requestSerial_ = 0;
};

} // namespace pci
