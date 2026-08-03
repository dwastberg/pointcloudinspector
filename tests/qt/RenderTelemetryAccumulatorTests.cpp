#include "renderer/telemetry/RenderTelemetryAccumulator.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace {

using namespace std::chrono_literals;

TEST_CASE("render telemetry accumulates and publishes deterministic windows",
          "[unit][renderer][metrics]")
{
    pci::RenderTelemetryAccumulator telemetry;
    const auto start = pci::RenderTelemetryAccumulator::TimePoint{};
    telemetry.setSourceTotals(1000, 800);
    telemetry.setPicking({4, 2, 400, 50});
    telemetry.recordFrame({
        .frameTime = 10ms,
        .snapshotTime = 1ms,
        .selectionTime = 2ms,
        .uploadTime = 3ms,
        .commandTime = 4ms,
        .requestedPoints = 900,
        .selectedPoints = 700,
        .submittedPoints = 600,
        .drawCalls = 6,
        .uploadedPointBytes = 100,
        .uploadOperations = 2,
        .resourceUpdateBatches = 1,
        .uniformUpdateOperations = 6,
    });
    telemetry.recordFrame({
        .frameTime = 20ms,
        .snapshotTime = 3ms,
        .selectionTime = 4ms,
        .uploadTime = 5ms,
        .commandTime = 6ms,
        .requestedPoints = 950,
        .selectedPoints = 750,
        .submittedPoints = 650,
        .drawCalls = 7,
        .uploadedPointBytes = 200,
        .uploadOperations = 3,
        .resourceUpdateBatches = 1,
        .uniformUpdateOperations = 7,
    });

    const auto snapshot = telemetry.takeSnapshot(start, false);
    REQUIRE(snapshot);
    CHECK(snapshot->frame.submittedFrameCount == 2);
    CHECK(snapshot->frame.frameMilliseconds == 11.0);
    CHECK(snapshot->frame.sceneSnapshotMilliseconds == 2.0);
    CHECK(snapshot->frame.commandRecordingMilliseconds == 5.0);
    CHECK(snapshot->selection.submittedPoints == 650);
    CHECK(snapshot->selection.sourcePoints == 1000);
    CHECK(snapshot->upload.uploadedPointBytes == 300);
    CHECK(snapshot->upload.uniformUpdateOperations == 13);
    CHECK(snapshot->picking.candidatePoints == 50);
    CHECK_FALSE(telemetry.takeSnapshot(start + 100ms, false));
    CHECK(telemetry.takeSnapshot(start + 100ms, true));
}

TEST_CASE("render telemetry saturates counters and tracks special frames",
          "[unit][renderer][metrics]")
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

TEST_CASE("render telemetry projects grouped state to the stable API",
          "[unit][renderer][metrics]")
{
    pci::RenderTelemetrySnapshot grouped;
    grouped.backend.selectedBackend = QStringLiteral("Metal");
    grouped.frame.submittedFrameCount = 12;
    grouped.selection.requestedPoints = 100;
    grouped.upload.uploadedPointBytes = 200;
    grouped.residency.gpuPointBytes = 300;
    grouped.decode.decodedPointBytes = 400;
    grouped.decode.decodeRequestsCompleted = 9;
    grouped.picking.candidateBlocks = 5;

    const pci::RenderMetrics projected = pci::projectRenderMetrics(grouped);
    CHECK(projected.selectedBackend == QStringLiteral("Metal"));
    CHECK(projected.submittedFrameCount == 12);
    CHECK(projected.requestedPoints == 100);
    CHECK(projected.uploadedPointBytes == 200);
    CHECK(projected.gpuPointBytes == 300);
    CHECK(projected.decodedPointBytes == 400);
    CHECK(projected.decodeRequestsCompleted == 9);
    CHECK(projected.pickCandidateBlocks == 5);
}

} // namespace
