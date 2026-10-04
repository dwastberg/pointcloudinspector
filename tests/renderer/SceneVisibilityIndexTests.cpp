#include <pci/rendering/planning/SceneVisibilityIndex.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace {

pci::Bounds3d bounds(const double minimum, const double maximum)
{
    return {
        .minimum = {minimum, minimum, minimum},
        .maximum = {maximum, maximum, maximum},
    };
}

TEST_CASE("scene visibility indexing follows immutable document revisions",
          "[unit][renderer-planning][snapshot][bounds-index]")
{
    const pci::PointCloudLayerId near{1};
    const pci::PointCloudLayerId far{2};

    pci::SceneVisibilityIndex index;
    const std::vector first{
        pci::SceneVisibilityLayer{.layerId = near,
                                  .sourceBounds = bounds(-1.0, 1.0)},
    };
    index.update(pci::DocumentGeneration{1}, 1, first);
    CHECK(index.visibleLayersIntersecting([](const pci::Bounds3d &bounds) {
        return bounds.minimum[0] < 10.0;
    }) == std::vector<pci::PointCloudLayerId>{near});

    const std::vector second{
        pci::SceneVisibilityLayer{.layerId = far,
                                  .sourceBounds = bounds(100.0, 101.0)},
    };
    // A replacement document may restart its local revision sequence. Its
    // generation still forces the index to rebuild.
    index.update(pci::DocumentGeneration{2}, 1, second);
    CHECK(index.visibleLayersIntersecting([](const pci::Bounds3d &bounds) {
        return bounds.minimum[0] > 10.0;
    }) == std::vector<pci::PointCloudLayerId>{far});
}

TEST_CASE("scene visibility indexing uses stable point source domains",
          "[unit][renderer-planning][snapshot][bounds-index]")
{
    const pci::PointCloudLayerId layer{7};
    const std::vector layers{
        pci::SceneVisibilityLayer{
            .layerId = layer,
            .sourceBounds = bounds(100.0, 200.0),
        },
    };

    pci::SceneVisibilityIndex index;
    index.update(pci::DocumentGeneration{1}, 1, layers);

    CHECK(index.visibleLayersIntersecting([](const pci::Bounds3d &bounds) {
        return bounds.minimum[0] >= 100.0;
    }) == std::vector<pci::PointCloudLayerId>{layer});
    CHECK(index
              .visibleLayersIntersecting([](const pci::Bounds3d &bounds) {
                  return bounds.maximum[0] <= 2.0;
              })
              .empty());
}

} // namespace
