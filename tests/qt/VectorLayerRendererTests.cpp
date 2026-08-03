#include "renderer/rhi/VectorLayerRenderer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

pci::FrameCamera perspectiveCamera()
{
    return {
        .eye = {0.0, 0.0, 0.0},
        .forward = {0.0, 0.0, -1.0},
        .culler = pci::FrustumCuller::fromCamera({0.0, 0.0, 0.0},
                                                 {0.0, 0.0, -1.0},
                                                 {0.0, 1.0, 0.0},
                                                 {1.0, 0.0, 0.0},
                                                 60.0,
                                                 1.0,
                                                 1.0,
                                                 100.0),
        .outputWidth = 1000,
        .outputHeight = 1000,
        .nearPlane = 1.0,
        .farPlane = 100.0,
        .verticalFovDegrees = 60.0,
    };
}

pci::VectorLayerData vectorDataAtDepth(const double depth)
{
    pci::VectorLayerData data;
    data.bounds = {.minimum = {-0.1, -0.1, -depth},
                   .maximum = {0.1, 0.1, -depth}};
    data.markers.push_back({});
    return data;
}

} // namespace

TEST_CASE("vector pipelines have stable distinct slots",
          "[qt][renderer][vector]")
{
    CHECK(pci::vectorPipelineIndex(pci::VectorPrimitive::Fill,
                                   pci::VectorDepthMode::Tested) == 0);
    CHECK(pci::vectorPipelineIndex(pci::VectorPrimitive::Marker,
                                   pci::VectorDepthMode::AlwaysOnTop) == 5);
}

TEST_CASE("vector uniform staging preserves aligned records",
          "[qt][renderer][vector]")
{
    pci::VectorLayerDraw draw;
    draw.uniform.opacity = 0.25F;
    const auto bytes = pci::stageVectorLayerUniforms(
        {&draw, 1}, sizeof(pci::VectorLayerUniform) + 16);
    CHECK(bytes.size() == sizeof(pci::VectorLayerUniform) + 16);
}

TEST_CASE("vector culling rejects layers wholly before the near plane",
          "[qt][renderer][vector]")
{
    const pci::FrameCamera frame = perspectiveCamera();
    const pci::VectorLayerStyle style;
    const pci::VectorLayerData beforeNear = vectorDataAtDepth(0.5);
    CHECK_FALSE(pci::vectorLayerCullBounds(beforeNear, style, frame));

    const pci::VectorLayerData inFront = vectorDataAtDepth(10.0);
    CHECK(pci::vectorLayerCullBounds(inFront, style, frame));
}

TEST_CASE("vector culling inflates by farthest relevant depth and remains "
          "conservative",
          "[qt][renderer][vector]")
{
    pci::FrameCamera frame = perspectiveCamera();
    pci::VectorLayerStyle style;
    style.markerSizePixels = 20.0F;
    pci::VectorLayerData spanning = vectorDataAtDepth(10.0);
    spanning.bounds.maximum[2] = -2.0;
    const auto inflated = pci::vectorLayerCullBounds(spanning, style, frame);
    REQUIRE(inflated);
    // The far end determines the expansion. Nearest-depth inflation would be
    // five times smaller for this depth-spanning layer.
    const double pixelRadius =
        style.markerSizePixels * 0.5 + style.strokeWidthPixels * 0.5 + 1.0;
    const double expected =
        0.1 + pixelRadius * frame.worldUnitsPerPixelAtDepth(10.0);
    CHECK(inflated->maximum[0] == Catch::Approx(expected));

    pci::VectorLayerData beyondFar = vectorDataAtDepth(150.0);
    CHECK_FALSE(pci::vectorLayerCullBounds(beyondFar, style, frame));
}

TEST_CASE("vector culling retains a marker just outside a viewport edge",
          "[qt][renderer][vector]")
{
    pci::FrameCamera frame = perspectiveCamera();
    pci::VectorLayerStyle style;
    style.markerSizePixels = 20.0F;
    pci::VectorLayerData edge = vectorDataAtDepth(10.0);
    // The right frustum edge is ~5.77 at depth ten. The marker itself lies
    // just outside it, but its 11px footprint intersects the viewport.
    edge.bounds.minimum[0] = 5.80;
    edge.bounds.maximum[0] = 5.81;
    CHECK(pci::vectorLayerCullBounds(edge, style, frame));
}
