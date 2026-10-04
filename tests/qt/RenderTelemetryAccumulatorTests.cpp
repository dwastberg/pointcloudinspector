#include <pci/desktop/viewport/RenderMetricsProjection.h>

#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("render telemetry projects grouped state to the stable API",
          "[unit][renderer][metrics]")
{
    pci::RenderTelemetrySnapshot grouped;
    grouped.backend.selectedBackend = "Metal";
    grouped.frame.submittedFrameCount = 12;
    grouped.frame.sampledFrameMilliseconds = 8.0;
    grouped.frame.outputWidth = 1920;
    grouped.frame.outputHeight = 1080;
    grouped.selection.requestedPoints = 100;
    grouped.selection.rootOnlyLayerCount = 3;
    grouped.upload.uploadedPointBytes = 200;
    grouped.upload.protectedGpuPointBytes = 250;
    grouped.residency.gpuPointBytes = 300;
    grouped.decode.decodedPointBytes = 400;
    grouped.decode.decodeRequestsCompleted = 9;
    grouped.picking.candidateBlocks = 5;

    const pci::RenderMetrics projected = pci::projectRenderMetrics(grouped);
    CHECK(projected.selectedBackend == QStringLiteral("Metal"));
    CHECK(projected.submittedFrameCount == 12);
    CHECK(projected.sampledFrameMilliseconds == 8.0);
    CHECK(projected.outputWidth == 1920);
    CHECK(projected.outputHeight == 1080);
    CHECK(projected.requestedPoints == 100);
    CHECK(projected.rootOnlyLayerCount == 3);
    CHECK(projected.uploadedPointBytes == 200);
    CHECK(projected.protectedGpuPointBytes == 250);
    CHECK(projected.gpuPointBytes == 300);
    CHECK(projected.decodedPointBytes == 400);
    CHECK(projected.decodeRequestsCompleted == 9);
    CHECK(projected.pickCandidateBlocks == 5);
}

} // namespace
