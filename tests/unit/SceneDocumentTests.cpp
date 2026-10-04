#include "support/TestPointColorMaps.h"
#include "support/TestPointDatasets.h"
#include <pci/document/SceneDocument.h>
#include <pci/document/SceneDocumentSnapshot.h>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pci {

class SceneDocumentTestAccess {
public:
    static void resetLayerLookupInspections(const SceneDocument &document)
    {
        document.layerLookupInspections_ = 0;
    }

    [[nodiscard]] static std::size_t
    layerLookupInspections(const SceneDocument &document)
    {
        return document.layerLookupInspections_;
    }

    [[nodiscard]] static bool
    hasConsistentLayerIndex(const SceneDocument &document)
    {
        if (document.layerIndices_.size() != document.sceneLayers_.size()) {
            return false;
        }
        for (std::size_t index = 0; index < document.sceneLayers_.size();
             ++index) {
            const auto found =
                document.layerIndices_.find(document.sceneLayers_[index].id);
            if (found == document.layerIndices_.end() ||
                found->second != index) {
                return false;
            }
        }
        return true;
    }
};

} // namespace pci

namespace {

template <typename T>
concept ExposesPointScene = requires(T value) { value.scene; };

template <typename T>
concept ExposesRasterData = requires(T value) { value.data; };

static_assert(!ExposesPointScene<pci::PointCloudLayerSnapshot>);
static_assert(!ExposesPointScene<pci::PointCloudLayer>);
static_assert(!ExposesPointScene<pci::PointCloudLayerState>);
static_assert(!ExposesRasterData<pci::RasterLayerSnapshot>);
static_assert(!ExposesRasterData<pci::RasterLayer>);
static_assert(!ExposesRasterData<pci::RasterLayerState>);
static_assert(
    std::same_as<decltype(std::declval<const pci::SceneDocumentSnapshot &>()
                              .pointLayers()),
                 pci::PointCloudLayerSnapshotView>);
static_assert(!std::same_as<pci::PointCloudLayerSnapshotView,
                            std::vector<pci::PointCloudLayerSnapshot>>);

pci::PointDatasetView datasetWith(const pci::Bounds3d bounds,
                                  const std::uint64_t sourcePointCount,
                                  const bool hasColor,
                                  const bool loadingComplete = false)
{
    pci::PointCloudMetadata metadata;
    metadata.sourceBounds = bounds;
    metadata.sourcePointCount = sourcePointCount;
    metadata.hasColor = hasColor;
    return pci::test::pointDataset(
        std::move(metadata),
        pci::PointDatasetAvailability{
            .bounds = bounds,
            .pointCount = loadingComplete ? sourcePointCount : 0,
            .colorizeAvailability =
                loadingComplete ? pci::PointColorizeAvailability::Ready
                                : pci::PointColorizeAvailability::Loading,
        });
}

TEST_CASE("point-cloud document owns independently configured layers",
          "[unit][scene]")
{
    pci::SceneDocument document(pci::test::createTestPointColorMapCatalog());
    const pci::PointDatasetView colorDataset = datasetWith(
        {.minimum = {-2.0, -1.0, 0.0}, .maximum = {1.0, 2.0, 3.0}}, 10, true);
    const auto colorLayer = document.addLayer(colorDataset);
    const auto scalarLayer = document.addLayer(datasetWith(
        {.minimum = {4.0, 5.0, 6.0}, .maximum = {7.0, 8.0, 9.0}}, 20, false));

    CHECK(document.hasPointCloudLayers());
    CHECK(document.layerCount() == 2);
    CHECK(document.revision() == 2);
    REQUIRE(document.layer(colorLayer));
    CHECK(document.layer(colorLayer)->descriptor.sourceId ==
          colorDataset.descriptor.sourceId);
    CHECK(document.snapshot()->layer(colorLayer)->descriptor.sourceId ==
          colorDataset.descriptor.sourceId);
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

TEST_CASE("point-cloud document retains metadata by value",
          "[unit][scene][ownership]")
{
    pci::SceneDocument document;
    pci::PointDatasetView dataset = datasetWith(
        {.minimum = {-2.0, -1.0, 0.0}, .maximum = {1.0, 2.0, 3.0}}, 10, true);
    const pci::PointCloudLayerId id = document.addLayer(dataset);

    dataset.descriptor.metadata.sourcePointCount = 999;
    dataset.availability.pointCount = 999;

    REQUIRE(document.layer(id));
    CHECK(document.layer(id)->descriptor.sourceId ==
          dataset.descriptor.sourceId);
    CHECK(document.layer(id)->descriptor.metadata.sourcePointCount == 10);
    CHECK(document.layer(id)->availability.pointCount == 0);
    REQUIRE(document.snapshot()->layer(id));
    CHECK(document.snapshot()->layer(id)->descriptor.sourceId ==
          dataset.descriptor.sourceId);
    CHECK(document.visibleExpectedPointCount() == 10);
}

TEST_CASE("point-cloud document aggregates only visible layers",
          "[unit][scene]")
{
    pci::SceneDocument document;
    const auto first = document.addLayer(datasetWith(
        {.minimum = {-2.0, -1.0, 0.0}, .maximum = {1.0, 2.0, 3.0}}, 10, true));
    const auto second = document.addLayer(datasetWith(
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

TEST_CASE("document keeps source-domain and available point bounds distinct",
          "[unit][scene][bounds][progressive]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourceBounds = {
        .minimum = {100.0, 200.0, 300.0},
        .maximum = {400.0, 500.0, 600.0},
    };
    const pci::PointDatasetView dataset = pci::test::pointDataset(
        metadata,
        pci::PointDatasetAvailability{
            .bounds = {.minimum = {1.0, 2.0, 3.0}, .maximum = {4.0, 5.0, 6.0}},
            .pointCount = 1,
            .colorizeAvailability = pci::PointColorizeAvailability::Loading,
        });

    pci::SceneDocument document;
    const pci::PointCloudLayerId id = document.addLayer(dataset);

    REQUIRE(document.layerBounds(id));
    CHECK(document.layerBounds(id)->minimum == std::array{100.0, 200.0, 300.0});
    REQUIRE(document.sceneBounds());
    CHECK(document.sceneBounds()->maximum == std::array{400.0, 500.0, 600.0});
    REQUIRE(document.bounds());
    CHECK(document.bounds()->minimum == std::array{100.0, 200.0, 300.0});

    REQUIRE(document.visibleSceneBounds());
    CHECK(document.visibleSceneBounds()->minimum == std::array{1.0, 2.0, 3.0});
    REQUIRE(document.visibleBounds());
    CHECK(document.visibleBounds()->maximum == std::array{4.0, 5.0, 6.0});

    REQUIRE(document.setLayerVisible(id, false));
    CHECK_FALSE(document.visibleSceneBounds());
    CHECK_FALSE(document.visibleBounds());
    REQUIRE(document.bounds());
    CHECK(document.bounds()->minimum == std::array{100.0, 200.0, 300.0});
}

TEST_CASE("document snapshots freeze published point availability metadata",
          "[unit][scene][snapshot][progressive]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourceBounds = {
        .minimum = {100.0, 200.0, 300.0},
        .maximum = {400.0, 500.0, 600.0},
    };
    metadata.sourcePointCount = 2;
    metadata.hasIntensity = true;
    const pci::PointDatasetView dataset = pci::test::pointDataset(
        metadata,
        pci::PointDatasetAvailability{
            .bounds = metadata.sourceBounds,
            .pointCount = 0,
            .colorizeAvailability = pci::PointColorizeAvailability::Loading,
        });

    pci::SceneDocument document;
    const pci::PointCloudLayerId id = document.addLayer(dataset);
    const std::uint64_t initialPointRevision = document.pointRevision();
    const pci::SceneDocumentSnapshotPtr before = document.snapshot();
    REQUIRE(before);
    const std::optional<pci::PointCloudLayerSnapshot> beforeLayer =
        before->layer(id);
    REQUIRE(beforeLayer);
    CHECK(beforeLayer->availablePointCount == 0);
    CHECK(beforeLayer->availableBounds.minimum ==
          metadata.sourceBounds.minimum);
    CHECK(beforeLayer->availableBounds.maximum ==
          metadata.sourceBounds.maximum);
    CHECK(beforeLayer->colorizeAvailability ==
          pci::PointColorizeAvailability::Loading);
    CHECK_FALSE(beforeLayer->scalarRanges.intensity);
    CHECK_FALSE(beforeLayer->presentClassifications.anyVisible());

    pci::PointDatasetAvailability published{
        .bounds =
            {
                .minimum = {1.0, 2.0, 3.0},
                .maximum = {4.0, 5.0, 6.0},
            },
        .pointCount = 2,
        .colorizeAvailability = pci::PointColorizeAvailability::Ready,
    };
    published.scalarRanges.intensity =
        pci::PointScalarRange{.minimum = 11.0, .maximum = 22.0};
    published.presentClassifications.setVisible(7, true);
    published.presentClassifications.setVisible(9, true);

    // Preparing a new availability value cannot leak through a previously
    // published snapshot or invalidate the document cache by itself.
    const std::optional<pci::PointCloudLayerSnapshot> frozenBeforeRefresh =
        before->layer(id);
    REQUIRE(frozenBeforeRefresh);
    CHECK(frozenBeforeRefresh->availablePointCount == 0);
    CHECK(frozenBeforeRefresh->colorizeAvailability ==
          pci::PointColorizeAvailability::Loading);
    CHECK_FALSE(frozenBeforeRefresh->scalarRanges.intensity);
    CHECK(document.snapshot() == before);
    CHECK(document.pointRevision() == initialPointRevision);

    CHECK(document.setPointLayerAvailability(id, published));
    CHECK(document.pointRevision() == initialPointRevision + 1);
    const pci::SceneDocumentSnapshotPtr after = document.snapshot();
    REQUIRE(after != before);
    const std::optional<pci::PointCloudLayerSnapshot> afterLayer =
        after->layer(id);
    REQUIRE(afterLayer);
    CHECK(afterLayer->availablePointCount == 2);
    CHECK(afterLayer->availableBounds.minimum == std::array{1.0, 2.0, 3.0});
    CHECK(afterLayer->availableBounds.maximum == std::array{4.0, 5.0, 6.0});
    CHECK(afterLayer->colorizeAvailability ==
          pci::PointColorizeAvailability::Ready);
    REQUIRE(afterLayer->scalarRanges.intensity);
    CHECK(afterLayer->scalarRanges.intensity->minimum == 11.0);
    CHECK(afterLayer->scalarRanges.intensity->maximum == 22.0);
    CHECK(afterLayer->presentClassifications.isVisible(7));
    CHECK(afterLayer->presentClassifications.isVisible(9));

    // Republishing the same value is idempotent.
    CHECK(document.setPointLayerAvailability(id, published));
    CHECK(document.snapshot() == after);
    CHECK(document.pointRevision() == initialPointRevision + 1);

    const std::optional<pci::PointCloudLayerSnapshot> stillFrozen =
        before->layer(id);
    REQUIRE(stillFrozen);
    CHECK(stillFrozen->availablePointCount == 0);
    CHECK(stillFrozen->colorizeAvailability ==
          pci::PointColorizeAvailability::Loading);
    CHECK_FALSE(stillFrozen->scalarRanges.intensity);
}

TEST_CASE("point availability publication is validated and invalidates once",
          "[unit][scene][snapshot][progressive]")
{
    pci::SceneDocument document;
    const pci::PointDatasetView firstDataset = datasetWith(
        {.minimum = {0.0, 0.0, 0.0}, .maximum = {1.0, 1.0, 1.0}}, 10, true);
    const pci::PointDatasetView secondDataset = datasetWith(
        {.minimum = {2.0, 2.0, 2.0}, .maximum = {3.0, 3.0, 3.0}}, 20, false);
    const pci::PointCloudLayerId first = document.addLayer(firstDataset);
    const pci::PointCloudLayerId second = document.addLayer(secondDataset);
    const std::uint64_t revision = document.revision();
    const std::uint64_t pointRevision = document.pointRevision();

    std::array updates{
        pci::PointLayerAvailabilityUpdate{
            .layerId = first,
            .availability = {.bounds = {.minimum = {4.0, 5.0, 6.0},
                                        .maximum = {7.0, 8.0, 9.0}},
                             .pointCount = 4}},
        pci::PointLayerAvailabilityUpdate{
            .layerId = second,
            .availability = {.bounds = {.minimum = {10.0, 11.0, 12.0},
                                        .maximum = {13.0, 14.0, 15.0}},
                             .pointCount = 5}},
    };

    CHECK(document.setPointLayerAvailabilities(updates));
    CHECK(document.revision() == revision + 1);
    CHECK(document.pointRevision() == pointRevision + 1);
    CHECK(document.layer(first)->availability.pointCount == 4);
    CHECK(document.layer(second)->availability.pointCount == 5);

    CHECK(document.setPointLayerAvailabilities(updates));
    CHECK(document.revision() == revision + 1);
    CHECK(document.pointRevision() == pointRevision + 1);

    updates[0].availability.pointCount = 40;
    updates[1].layerId = pci::PointCloudLayerId{999};
    CHECK_FALSE(document.setPointLayerAvailabilities(updates));
    CHECK(document.layer(first)->availability.pointCount == 4);
    CHECK(document.revision() == revision + 1);

    updates[1] = updates[0];
    CHECK_FALSE(document.setPointLayerAvailabilities(updates));
    CHECK(document.layer(first)->availability.pointCount == 4);
    CHECK(document.revision() == revision + 1);
}

TEST_CASE("point-cloud document saturates extreme multi-source totals",
          "[unit][scene][multi-layer][overflow]")
{
    pci::SceneDocument document;
    static_cast<void>(document.addLayer(
        datasetWith({}, std::numeric_limits<std::uint64_t>::max(), false)));
    static_cast<void>(document.addLayer(datasetWith({}, 1, false)));

    CHECK(document.visibleExpectedPointCount() ==
          std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE(
    "document color bounds include hidden layers and change with membership",
    "[unit][scene][color][multi-layer]")
{
    pci::SceneDocument document;
    const auto first = document.addLayer(datasetWith(
        {.minimum = {10.0, 20.0, 30.0}, .maximum = {20.0, 40.0, 60.0}},
        10,
        false));
    const auto second = document.addLayer(datasetWith(
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
    pci::SceneDocument document(pci::test::createTestPointColorMapCatalog());
    const auto layer = document.addLayer(datasetWith(
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

    pci::SceneDocument document;
    const pci::PointCloudLayerId classified =
        document.addLayer(pci::test::pointDataset(classifiedMetadata));
    const pci::PointCloudLayerId unclassified =
        document.addLayer(datasetWith({}, 1, false));
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

TEST_CASE("document inserts late previews at their user-facing order",
          "[unit][scene][multi-file][progressive]")
{
    pci::SceneDocument document;
    const pci::PointCloudLayerId second =
        document.insertLayer(datasetWith({}, 2, false, true), 0);
    const pci::PointCloudLayerId third =
        document.insertLayer(datasetWith({}, 3, false, true), 1);
    const pci::PointCloudLayerId first =
        document.insertLayer(datasetWith({}, 1, false, true), 0);

    const auto layers = document.layers();
    REQUIRE(layers.size() == 3);
    CHECK(layers[0].id == first);
    CHECK(layers[1].id == second);
    CHECK(layers[2].id == third);
    CHECK(pci::SceneDocumentTestAccess::hasConsistentLayerIndex(document));
}

TEST_CASE("document scene bounds visit each layer without nested ID lookups",
          "[unit][scene][bounds][complexity]")
{
    constexpr std::size_t layerCount = 64;
    pci::SceneDocument document;
    pci::PointCloudLayerId lastId;
    for (std::size_t index = 0; index < layerCount; ++index) {
        const double coordinate = static_cast<double>(index);
        lastId = document.addLayer(datasetWith(
            {.minimum = {coordinate, coordinate, coordinate},
             .maximum = {coordinate + 1.0, coordinate + 2.0, coordinate + 3.0}},
            1,
            false));
    }

    pci::SceneDocumentTestAccess::resetLayerLookupInspections(document);
    const std::optional<pci::Bounds3d> bounds = document.sceneBounds();

    REQUIRE(bounds);
    CHECK(bounds->minimum == std::array{0.0, 0.0, 0.0});
    CHECK(bounds->maximum == std::array{64.0, 65.0, 66.0});
    CHECK(pci::SceneDocumentTestAccess::layerLookupInspections(document) == 0);
    CHECK(pci::SceneDocumentTestAccess::hasConsistentLayerIndex(document));

    pci::SceneDocumentTestAccess::resetLayerLookupInspections(document);
    REQUIRE(document.layer(lastId));
    CHECK_FALSE(document.layer(pci::SceneLayerId{10'000}));
    CHECK(pci::SceneDocumentTestAccess::layerLookupInspections(document) == 2);

    const pci::SceneDocumentSnapshotPtr snapshot = document.snapshot();
    REQUIRE(snapshot);
    CHECK(snapshot->layerIndices.size() == snapshot->layers.size());
    for (std::size_t index = 0; index < snapshot->layers.size(); ++index) {
        CHECK(snapshot->layerIndices.at(snapshot->layers[index].id) == index);
    }
}

} // namespace
