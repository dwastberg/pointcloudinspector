#include "development/SyntheticScene.h"
#include "renderer/RenderViewport.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace {

TEST_CASE("render viewport exposes its public widget contract",
          "[ui][renderer][api]")
{
    auto viewport = pci::createRenderViewport(true);
    REQUIRE(viewport != nullptr);
    REQUIRE(viewport->widget() != nullptr);
    CHECK_FALSE(viewport->backendName().isEmpty());

    auto document = std::make_shared<pci::SceneDocument>();
    const pci::PointCloudLayerId layerId =
        document->addLayer(pci::buildSyntheticScene(1'000));
    viewport->setDocument(document->snapshot(), true);
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
    viewport->setEyeDomeLightingEnabled(false);
    CHECK_FALSE(viewport->eyeDomeLightingEnabled());
    viewport->setPointSizePixels(pci::maximumPointSizePixels);
    CHECK(viewport->pointSizePixels() == pci::maximumPointSizePixels);
    viewport->setActiveTool(pci::ViewportTool::Measure);
    CHECK(viewport->activeTool() == pci::ViewportTool::Measure);
}

} // namespace
