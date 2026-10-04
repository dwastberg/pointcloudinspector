#pragma once

#include <pci/navigation/NavigationInputState.h>
#include <pci/rendering/planning/Measurement.h>

#include <chrono>
#include <cstdint>
#include <optional>

namespace pci {

struct MeasurementRevisions {
    std::uint64_t camera = 0;
    std::uint64_t document = 0;
    std::uint64_t selection = 0;

    bool operator==(const MeasurementRevisions &) const = default;
};

struct MeasurementViewState {
    PixelPosition hoverPosition;
    std::optional<Vec3d> hover;
    std::optional<Vec3d> anchor;
    std::optional<DistanceMeasurement> measurement;
};

class MeasurementController {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static constexpr auto hoverInterval = std::chrono::milliseconds(33);

    [[nodiscard]] std::optional<std::chrono::milliseconds>
    scheduleHover(PixelPosition position, TimePoint now) noexcept;
    [[nodiscard]] std::optional<PixelPosition> takeScheduledHover() noexcept;
    void setHoverPosition(PixelPosition position) noexcept;
    void recordHoverQueued(std::uint64_t serial, TimePoint now) noexcept;

    [[nodiscard]] bool acceptHoverResult(std::uint64_t serial,
                                         MeasurementRevisions requestRevisions,
                                         MeasurementRevisions currentRevisions,
                                         std::optional<Vec3d> point) noexcept;
    void acceptCommitResult(std::optional<Vec3d> point,
                            MeasurementRevisions revisions) noexcept;
    void invalidateHover(MeasurementRevisions currentRevisions) noexcept;
    void clearHover() noexcept;
    void clear() noexcept;

    [[nodiscard]] const MeasurementViewState &state() const noexcept;

private:
    MeasurementViewState state_;
    std::optional<MeasurementRevisions> hoverRevisions_;
    std::optional<TimePoint> lastHoverQueuedAt_;
    bool hoverScheduled_ = false;
    std::uint64_t latestHoverSerial_ = 0;
};

} // namespace pci
