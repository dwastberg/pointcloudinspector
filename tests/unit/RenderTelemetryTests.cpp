#include <pci/rendering/RenderTelemetry.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>
#include <type_traits>

namespace {

using namespace std::chrono_literals;

static_assert(std::is_same_v<decltype(pci::RenderBackendTelemetry::deviceName),
                             std::string>);

TEST_CASE("portable render telemetry accumulates deterministic windows",
          "[unit][render-telemetry][metrics]")
{
    pci::RenderTelemetryAccumulator telemetry;
    const auto start = pci::RenderTelemetryAccumulator::TimePoint{};
    telemetry.setSourceTotals(1000, 800);
    telemetry.setPicking({4, 2, 400, 50});
    telemetry.recordFrame({
        .frameTime = 10ms,
        .outputWidth = 1280,
        .outputHeight = 800,
        .snapshotTime = 1ms,
        .selectionTime = 2ms,
        .uploadTime = 3ms,
        .commandTime = 4ms,
        .requestedPoints = 900,
        .selectedPoints = 700,
        .submittedPoints = 600,
        .drawCalls = 6,
        .rootOnlyLayerCount = 2,
        .uploadedPointBytes = 100,
        .protectedGpuPointBytes = 1'600,
        .uploadOperations = 2,
        .resourceUpdateBatches = 1,
        .uniformUpdateOperations = 6,
    });
    telemetry.recordFrame({
        .frameTime = 20ms,
        .outputWidth = 1280,
        .outputHeight = 800,
        .snapshotTime = 3ms,
        .selectionTime = 4ms,
        .uploadTime = 5ms,
        .commandTime = 6ms,
        .requestedPoints = 950,
        .selectedPoints = 750,
        .submittedPoints = 650,
        .drawCalls = 7,
        .rootOnlyLayerCount = 1,
        .uploadedPointBytes = 200,
        .protectedGpuPointBytes = 3'200,
        .uploadOperations = 3,
        .resourceUpdateBatches = 1,
        .uniformUpdateOperations = 7,
    });

    const auto snapshot = telemetry.takeSnapshot(start, false);
    REQUIRE(snapshot);
    CHECK(snapshot->frame.submittedFrameCount == 2);
    CHECK(snapshot->frame.frameMilliseconds == 11.0);
    CHECK(snapshot->frame.sampledFrameMilliseconds == 20.0);
    CHECK(snapshot->frame.outputWidth == 1280);
    CHECK(snapshot->frame.outputHeight == 800);
    CHECK(snapshot->frame.sceneSnapshotMilliseconds == 2.0);
    CHECK(snapshot->frame.commandRecordingMilliseconds == 5.0);
    CHECK(snapshot->selection.submittedPoints == 650);
    CHECK(snapshot->selection.rootOnlyLayerCount == 1);
    CHECK(snapshot->selection.sourcePoints == 1000);
    CHECK(snapshot->upload.uploadedPointBytes == 300);
    CHECK(snapshot->upload.protectedGpuPointBytes == 3'200);
    CHECK(snapshot->upload.uniformUpdateOperations == 13);
    CHECK(snapshot->picking.candidatePoints == 50);
    CHECK_FALSE(telemetry.takeSnapshot(start + 100ms, false));
    CHECK(telemetry.takeSnapshot(start + 100ms, true));
}

TEST_CASE(
    "portable render telemetry saturates counters and tracks special frames",
    "[unit][render-telemetry][metrics]")
{
    pci::RenderTelemetryAccumulator telemetry;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    telemetry.recordFrame({
        .includedPick = true,
        .uploadedPointBytes = maximum,
        .uploadOperations = maximum,
    });
    telemetry.recordFrame({
        .uploadedPointBytes = 1,
        .uploadOperations = 1,
    });
    REQUIRE(telemetry.specialFrameCooldownActive());
    telemetry.advanceSpecialFrameCooldown();
    telemetry.advanceSpecialFrameCooldown();
    CHECK(telemetry.specialFrameCooldownActive());
    telemetry.advanceSpecialFrameCooldown();
    CHECK_FALSE(telemetry.specialFrameCooldownActive());

    const auto snapshot = telemetry.takeSnapshot({}, true);
    REQUIRE(snapshot);
    CHECK(snapshot->upload.uploadedPointBytes == maximum);
    CHECK(snapshot->upload.uploadOperations == maximum);
}

} // namespace
