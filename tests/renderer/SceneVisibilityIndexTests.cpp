#include "renderer/planning/SceneVisibilityIndex.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

namespace {

pci::PointCloudScenePtr sceneWithBounds(const double minimum,
                                        const double maximum)
{
    pci::PointCloudMetadata metadata;
    metadata.sourceBounds = {
        .minimum = {minimum, minimum, minimum},
        .maximum = {maximum, maximum, maximum},
    };
    return std::make_shared<pci::PointCloudScene>(std::move(metadata));
}

TEST_CASE("scene visibility indexing follows immutable document revisions",
          "[unit][renderer-planning][snapshot][bounds-index]")
{
    pci::SceneDocument document;
    const pci::PointCloudLayerId near =
        document.addLayer(sceneWithBounds(-1.0, 1.0));
    const pci::PointCloudLayerId far =
        document.addLayer(sceneWithBounds(100.0, 101.0));
    REQUIRE(document.setLayerVisible(far, false));

    pci::SceneVisibilityIndex index;
    index.update(*document.snapshot());
    CHECK(index.visibleLayersIntersecting([](const pci::Bounds3d &bounds) {
        return bounds.minimum[0] < 10.0;
    }) == std::vector<pci::PointCloudLayerId>{near});

    REQUIRE(document.setLayerVisible(near, false));
    REQUIRE(document.setLayerVisible(far, true));
    index.update(*document.snapshot());
    CHECK(index.visibleLayersIntersecting([](const pci::Bounds3d &bounds) {
        return bounds.minimum[0] > 10.0;
    }) == std::vector<pci::PointCloudLayerId>{far});
}

} // namespace
