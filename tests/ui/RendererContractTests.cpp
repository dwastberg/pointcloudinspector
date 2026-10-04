#include "support/SceneRuntimeFixture.h"
#include "support/TestPointColorMaps.h"
#include <pci/desktop/viewport/RenderViewport.h>
#include <pci/development/SyntheticScene.h>
#include <pci/document/SceneDocument.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>

namespace {

TEST_CASE("render viewport exposes its public widget contract",
          "[ui][renderer][api]")
{
    const auto colorMaps = pci::test::createTestPointColorMapCatalog();
    auto viewport = pci::createRenderViewport(true,
                                              std::uint64_t{512} * 1024 * 1024,
                                              pci::GraphicsApi::Auto,
                                              false,
                                              colorMaps);
    REQUIRE(viewport != nullptr);
    REQUIRE(viewport->widget() != nullptr);
    CHECK_FALSE(viewport->backendName().isEmpty());

    auto document = std::make_shared<pci::SceneDocument>(colorMaps);
    pci::test::SceneRuntimeFixture runtime(document);
    const pci::PointCloudLayerId layerId =
        runtime.addPointLayer(pci::buildSyntheticScene(1'000));
    runtime.setDocument(*viewport, true);
    CHECK(viewport->totalPointCount() == 1'000);

    CHECK(document->setLayerColorMode(layerId,
                                      {
                                          .source = pci::PointColorSource::Z,
                                          .colorMap = pci::PointColorMap::Turbo,
                                      }));
    REQUIRE(document->layer(layerId));
    CHECK(document->layer(layerId)->colorMode.colorMap ==
          pci::PointColorMap::Turbo);

    viewport->setOrthographic(true);
    CHECK(viewport->isOrthographic());
    viewport->setOrthographic(false);
    viewport->setMapView(true);
    CHECK(viewport->isMapView());
    CHECK(viewport->isOrthographic());
    viewport->setMapView(false);
    CHECK_FALSE(viewport->isMapView());
    CHECK_FALSE(viewport->isOrthographic());
    viewport->setEyeDomeLightingEnabled(false);
    CHECK_FALSE(viewport->eyeDomeLightingEnabled());
    viewport->setPointSizePixels(pci::maximumPointSizePixels);
    CHECK(viewport->pointSizePixels() == pci::maximumPointSizePixels);
    viewport->setActiveTool(pci::ViewportTool::Measure);
    CHECK(viewport->activeTool() == pci::ViewportTool::Measure);
}

} // namespace
