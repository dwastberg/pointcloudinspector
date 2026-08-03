#include "scene/SceneDocument.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <memory>
#include <stop_token>

namespace {

pci::PointCloudScenePtr sceneWith(const pci::Bounds3d bounds,
                                  const std::uint64_t sourcePointCount,
                                  const bool hasColor)
{
    pci::PointCloudMetadata metadata;
    metadata.sourceBounds = bounds;
    metadata.sourcePointCount = sourcePointCount;
    metadata.hasColor = hasColor;
    return std::make_shared<pci::PointCloudScene>(std::move(metadata));
}

class RootOnlyHierarchySource final : public pci::PointCloudDataSource {
public:
    explicit RootOnlyHierarchySource(const pci::Bounds3d bounds)
        : bounds_(bounds)
    {
    }

    [[nodiscard]] pci::PointCloudNode rootNode() const override
    {
        return node(pci::rootPointCloudNode);
    }

    [[nodiscard]] pci::PointCloudNode
    node(const pci::PointCloudNodeId id) const override
    {
        return {
            .id = id,
            .bounds = pci::pointCloudNodeBounds(bounds_, id),
            .estimatedPointCount = 1,
            .leaf = true,
        };
    }

    [[nodiscard]] pci::PointCloudNodePayloadPtr
    loadNode(const pci::PointCloudNodeId, const std::stop_token) const override
    {
        throw std::logic_error("root-only test source cannot decode children");
    }

private:
    pci::Bounds3d bounds_;
};

pci::PointCloudScenePtr
hierarchicalSceneWith(const pci::Bounds3d bounds,
                      const std::size_t rootPointCount,
                      const std::uint64_t standaloneBudget)
{
    pci::PointCloudMetadata metadata;
    metadata.sourceBounds = bounds;
    metadata.sourcePointCount = rootPointCount;
    auto block = std::make_shared<pci::PointBlock>();
    block->points.resize(rootPointCount);
    block->attributes.resize(rootPointCount);
    auto root = std::make_shared<pci::PointCloudNodePayload>();
    root->nodeId = pci::rootPointCloudNode;
    root->sourcePointCount = rootPointCount;
    root->blocks.push_back(std::move(block));
    return std::make_shared<pci::PointCloudScene>(
        metadata,
        std::make_shared<RootOnlyHierarchySource>(bounds),
        std::move(root),
        standaloneBudget);
}

TEST_CASE("point-cloud document owns independently configured layers",
          "[unit][scene]")
{
    pci::SceneDocument document;
    const auto colorLayer = document.addLayer(sceneWith(
        {.minimum = {-2.0, -1.0, 0.0}, .maximum = {1.0, 2.0, 3.0}}, 10, true));
    const auto scalarLayer = document.addLayer(sceneWith(
        {.minimum = {4.0, 5.0, 6.0}, .maximum = {7.0, 8.0, 9.0}}, 20, false));

    CHECK(document.hasPointCloudLayers());
    CHECK(document.layerCount() == 2);
    CHECK(document.revision() == 2);
    REQUIRE(document.layer(colorLayer));
    CHECK(document.layer(colorLayer)->colorMode ==
          pci::PointColorMode{
              .source = pci::PointColorSource::Rgb,
              .colorMap = pci::PointColorMap::Rgb,
          });
    REQUIRE(document.layer(scalarLayer));
    CHECK(document.layer(scalarLayer)->colorMode ==
          pci::PointColorMode{
              .source = pci::PointColorSource::Z,
              .colorMap = pci::PointColorMap::Viridis,
          });

    CHECK(document.setLayerVisible(scalarLayer, false));
    CHECK(document.setLayerColorMode(colorLayer,
                                     {.source = pci::PointColorSource::X,
                                      .colorMap = pci::PointColorMap::Turbo}));
    CHECK(document.revision() == 4);
    CHECK_FALSE(document.layer(scalarLayer)->visible);
    CHECK(document.layer(colorLayer)->colorMode.colorMap ==
          pci::PointColorMap::Turbo);
}

TEST_CASE("point-cloud document aggregates only visible layers",
          "[unit][scene]")
{
    pci::SceneDocument document;
    const auto first = document.addLayer(sceneWith(
        {.minimum = {-2.0, -1.0, 0.0}, .maximum = {1.0, 2.0, 3.0}}, 10, true));
    const auto second = document.addLayer(sceneWith(
        {.minimum = {4.0, 5.0, 6.0}, .maximum = {7.0, 8.0, 9.0}}, 20, true));

    CHECK(document.visiblePointCount() == 0);
    CHECK(document.visibleExpectedPointCount() == 30);
    REQUIRE(document.visibleBounds());
    CHECK(document.visibleBounds()->minimum == std::array{-2.0, -1.0, 0.0});
    CHECK(document.visibleBounds()->maximum == std::array{7.0, 8.0, 9.0});

    CHECK(document.setLayerVisible(second, false));
    CHECK(document.visibleExpectedPointCount() == 10);
    REQUIRE(document.visibleBounds());
    CHECK(document.visibleBounds()->minimum == std::array{-2.0, -1.0, 0.0});
    CHECK(document.visibleBounds()->maximum == std::array{1.0, 2.0, 3.0});
    CHECK(document.removeLayer(first));
    CHECK_FALSE(document.visibleBounds());
    CHECK_FALSE(document.removeLayer(first));
}

TEST_CASE("point-cloud document saturates extreme multi-source totals",
          "[unit][scene][multi-layer][overflow]")
{
    pci::SceneDocument document;
    static_cast<void>(document.addLayer(
        sceneWith({}, std::numeric_limits<std::uint64_t>::max(), false)));
    static_cast<void>(document.addLayer(sceneWith({}, 1, false)));

    CHECK(document.visibleExpectedPointCount() ==
          std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE(
    "document color bounds include hidden layers and change with membership",
    "[unit][scene][color][multi-layer]")
{
    pci::SceneDocument document;
    const auto first = document.addLayer(sceneWith(
        {.minimum = {10.0, 20.0, 30.0}, .maximum = {20.0, 40.0, 60.0}},
        10,
        false));
    const auto second = document.addLayer(sceneWith(
        {.minimum = {-5.0, 25.0, 15.0}, .maximum = {100.0, 35.0, 90.0}},
        10,
        false));

    REQUIRE(document.bounds());
    CHECK(document.bounds()->minimum == std::array{-5.0, 20.0, 15.0});
    CHECK(document.bounds()->maximum == std::array{100.0, 40.0, 90.0});
    CHECK(document.setLayerVisible(second, false));
    CHECK(document.bounds()->minimum == std::array{-5.0, 20.0, 15.0});
    CHECK(document.bounds()->maximum == std::array{100.0, 40.0, 90.0});

    CHECK(document.removeLayer(second));
    REQUIRE(document.bounds());
    CHECK(document.bounds()->minimum == std::array{10.0, 20.0, 30.0});
    CHECK(document.bounds()->maximum == std::array{20.0, 40.0, 60.0});
    REQUIRE(document.layer(first));
}

TEST_CASE("document rejects incompatible maps and invalid manual ranges",
          "[unit][scene][color]")
{
    pci::SceneDocument document;
    const auto layer = document.addLayer(sceneWith(
        {.minimum = {0.0, 0.0, 0.0}, .maximum = {1.0, 1.0, 1.0}}, 10, true));

    CHECK_FALSE(document.setLayerColorMode(
        layer,
        {
            .source = pci::PointColorSource::Z,
            .colorMap = pci::PointColorMap::LasClassification,
        }));
    CHECK_FALSE(document.setLayerColorMode(
        layer,
        {
            .source = pci::PointColorSource::Z,
            .colorMap = pci::PointColorMap::Viridis,
            .manualRange = pci::PointScalarRange{5.0, 5.0},
        }));
    CHECK(document.setLayerColorMode(
        layer,
        {
            .source = pci::PointColorSource::Z,
            .colorMap = pci::PointColorMap::Viridis,
            .manualRange = pci::PointScalarRange{0.25, 0.75},
        }));
}

TEST_CASE("document stores classification visibility per layer",
          "[unit][scene][classification]")
{
    pci::PointCloudMetadata classifiedMetadata;
    classifiedMetadata.hasClassification = true;
    auto classifiedScene =
        std::make_shared<pci::PointCloudScene>(classifiedMetadata);

    pci::SceneDocument document;
    const pci::PointCloudLayerId classified =
        document.addLayer(classifiedScene);
    const pci::PointCloudLayerId unclassified =
        document.addLayer(sceneWith({}, 1, false));
    const std::uint64_t initialRevision = document.revision();

    pci::PointClassificationFilter filter =
        pci::PointClassificationFilter::noneVisible();
    filter.setVisible(2, true);
    filter.setVisible(6, true);
    CHECK(document.setLayerClassificationFilter(classified, filter));
    REQUIRE(document.layer(classified));
    CHECK(document.layer(classified)->classificationFilter == filter);
    CHECK(document.revision() == initialRevision + 1);

    CHECK_FALSE(document.setLayerClassificationFilter(unclassified, filter));
    CHECK_FALSE(document.setLayerClassificationFilter(
        pci::PointCloudLayerId{999}, filter));
}

TEST_CASE("document layer churn releases shared cache entries",
          "[unit][scene][hierarchy][residency][churn]")
{
    const pci::Bounds3d bounds{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    constexpr std::size_t layerCount = 25;
    pci::SceneDocument document(1024 * 1024, 2);
    std::vector<pci::PointCloudLayerId> ids;
    ids.reserve(layerCount);
    for (std::size_t index = 0; index < layerCount; ++index) {
        ids.push_back(
            document.addLayer(hierarchicalSceneWith(bounds, 1, 4096)));
    }
    CHECK(document.decodedPageCache()->size() == layerCount);

    for (std::size_t index = 0; index < ids.size(); index += 2) {
        REQUIRE(document.setLayerVisible(ids[index], false));
    }
    for (std::size_t index = 0; index < ids.size(); index += 2) {
        REQUIRE(document.setLayerVisible(ids[index], true));
    }
    for (const pci::PointCloudLayerId id : ids) {
        REQUIRE(document.removeLayer(id));
    }
    CHECK_FALSE(document.hasPointCloudLayers());
    CHECK(document.decodedPageCache()->size() == 0);
    CHECK(document.decodedResidentBytes() == 0);
}

TEST_CASE("document refuses roots that exceed its shared CPU budget",
          "[unit][scene][hierarchy][residency][admission]")
{
    const pci::Bounds3d bounds{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    auto first = hierarchicalSceneWith(bounds, 1, 4096);
    auto second = hierarchicalSceneWith(bounds, 1, 4096);
    const std::uint64_t oneRoot = first->decodedResidentBytes();
    pci::SceneDocument document(oneRoot, 1);
    static_cast<void>(document.addLayer(first));
    CHECK_THROWS_AS(document.addLayer(second), std::length_error);
    CHECK(document.layerCount() == 1);
    CHECK(document.decodedResidentBytes() == oneRoot);
}

TEST_CASE("document thins root previews evenly before refusing a source",
          "[unit][scene][hierarchy][residency][fairness]")
{
    const pci::Bounds3d bounds{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    auto first = hierarchicalSceneWith(bounds, 4, 4096);
    auto second = hierarchicalSceneWith(bounds, 4, 4096);
    const std::uint64_t oneFullRoot = first->decodedResidentBytes();
    pci::SceneDocument document(oneFullRoot, 1);
    static_cast<void>(document.addLayer(first));
    static_cast<void>(document.addLayer(second));

    CHECK(document.layerCount() == 2);
    CHECK(first->decodedResidentPoints() == 2);
    CHECK(second->decodedResidentPoints() == 2);
    CHECK(document.decodedResidentBytes() == oneFullRoot);
    CHECK(document.decodedResidentBytes() <= document.decodedByteBudget());
}

TEST_CASE("document budget synchronization cannot strand pinned roots",
          "[unit][scene][hierarchy][residency][memory]")
{
    const pci::Bounds3d bounds{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    auto scene = hierarchicalSceneWith(bounds, 4, 4096);
    const std::uint64_t rootBytes = scene->decodedResidentBytes();
    pci::SceneDocument document(rootBytes + 100, 1);
    static_cast<void>(document.addLayer(scene));

    REQUIRE(document.memoryBudget()->setByteBudget(rootBytes - 1));
    document.syncResidencyBudgets();
    CHECK(document.memoryBudget()->byteBudget() == rootBytes - 1);
    CHECK(document.decodedPageCache()->byteBudget() == rootBytes - 1);
    CHECK(document.decodedResidentBytes() < rootBytes);
    CHECK(document.decodedResidentBytes() <=
          document.decodedPageCache()->byteBudget());
}

TEST_CASE("document owns one decoded cache shared by hierarchical layers",
          "[unit][scene][hierarchy][residency]")
{
    const pci::Bounds3d bounds{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    auto firstScene = hierarchicalSceneWith(bounds, 1, 4096);
    auto secondScene = hierarchicalSceneWith(bounds, 1, 4096);
    const std::uint64_t retainedRoots = firstScene->decodedResidentBytes() +
                                        secondScene->decodedResidentBytes();
    const std::uint64_t documentBudget = retainedRoots + 2000;
    pci::SceneDocument document(documentBudget, 1);

    const auto first = document.addLayer(firstScene);
    const auto second = document.addLayer(secondScene);

    CHECK(firstScene->decodedByteBudget() == documentBudget);
    CHECK(secondScene->decodedByteBudget() == documentBudget);
    CHECK(document.decodedPageCache()->byteBudget() == documentBudget);
    CHECK(document.decodedResidentBytes() == retainedRoots);
    REQUIRE(firstScene->nodePayload(pci::rootPointCloudNode));
    const pci::SceneDocumentMetrics initialMetrics =
        document.hierarchyMetrics();
    CHECK(initialMetrics.hierarchicalLayers == 2);
    CHECK(initialMetrics.cache.hits == 1);
    CHECK(initialMetrics.cache.insertions == 2);
    CHECK(initialMetrics.cache.residentBytes == retainedRoots);
    CHECK(initialMetrics.cache.byteBudget == documentBudget);
    CHECK(initialMetrics.cache.peakResidentBytes == retainedRoots);

    CHECK(document.setLayerVisible(second, false));
    CHECK(secondScene->decodedByteBudget() == documentBudget);
    CHECK(firstScene->decodedByteBudget() == documentBudget);
    CHECK(document.decodedPageCache()->residentBytes() == retainedRoots);

    CHECK(document.removeLayer(second));
    CHECK(firstScene->decodedByteBudget() == documentBudget);
    CHECK(document.decodedPageCache()->residentBytes() ==
          firstScene->decodedResidentBytes());
    REQUIRE(document.layer(first));
}

TEST_CASE("hierarchical scenes transition from standalone to document "
          "residency and back",
          "[unit][scene][hierarchy][residency][lifecycle]")
{
    const pci::Bounds3d bounds{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    constexpr std::uint64_t standaloneBudget = 4096;
    constexpr std::uint64_t documentBudget = 8192;
    auto scene = hierarchicalSceneWith(bounds, 1, standaloneBudget);

    CHECK(scene->decodedByteBudget() == standaloneBudget);
    {
        pci::SceneDocument document(documentBudget, 1);
        const pci::PointCloudLayerId id = document.addLayer(scene);
        CHECK(scene->decodedByteBudget() == documentBudget);

        REQUIRE(document.removeLayer(id));
        CHECK(scene->decodedByteBudget() == standaloneBudget);
    }

    scene.reset();
}

TEST_CASE("document inserts late previews at their user-facing order",
          "[unit][scene][multi-file][progressive]")
{
    pci::SceneDocument document;
    const auto secondScene = sceneWith({}, 2, false);
    secondScene->markLoadingComplete();
    const pci::PointCloudLayerId second = document.insertLayer(secondScene, 0);
    const auto thirdScene = sceneWith({}, 3, false);
    thirdScene->markLoadingComplete();
    const pci::PointCloudLayerId third = document.insertLayer(thirdScene, 1);
    const auto firstScene = sceneWith({}, 1, false);
    firstScene->markLoadingComplete();
    const pci::PointCloudLayerId first = document.insertLayer(firstScene, 0);

    const auto layers = document.layers();
    REQUIRE(layers.size() == 3);
    CHECK(layers[0].id == first);
    CHECK(layers[1].id == second);
    CHECK(layers[2].id == third);
}

} // namespace
