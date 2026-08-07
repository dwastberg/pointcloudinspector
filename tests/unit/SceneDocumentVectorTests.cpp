#include "scene/SceneDocument.h"
#include "scene/SceneDocumentSnapshot.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>

namespace {

pci::VectorLayerDataPtr vectorData(const double minimum, const double maximum)
{
    auto data = std::make_shared<pci::VectorLayerData>();
    data->bounds = {.minimum = {minimum, minimum, 0.0},
                    .maximum = {maximum, maximum, 0.0}};
    return data;
}

TEST_CASE("scene document keeps vector layers in a separate revision domain",
          "[unit][scene][vector]")
{
    pci::SceneDocument document;
    const auto id = document.addVectorLayer(vectorData(1000.0, 1001.0), false);

    CHECK_FALSE(document.hasPointCloudLayers());
    CHECK(document.hasAnyLayer());
    CHECK(document.layerCount() == 0);
    CHECK(document.vectorLayerCount() == 1);
    CHECK(document.layerKind(id) == pci::SceneLayerKind::Vector);
    CHECK(document.revision() == 1);
    CHECK(document.pointRevision() == 0);
    CHECK(document.vectorRevision() == 1);
    CHECK_FALSE(document.visibleSceneBounds());
    REQUIRE(document.sceneBounds());
    CHECK(document.sceneBounds()->minimum[0] == 1000.0);

    CHECK(document.setLayerVisible(id, true));
    REQUIRE(document.visibleSceneBounds());
    CHECK(document.visibleSceneBounds()->maximum[0] == 1001.0);
    CHECK(document.revision() == 2);
    CHECK(document.pointRevision() == 0);
    CHECK(document.vectorRevision() == 2);
}

TEST_CASE("scene document shares layer ids and copies vector records",
          "[unit][scene][vector]")
{
    pci::SceneDocument source;
    const auto id = source.addVectorLayer(vectorData(-2.0, 2.0));
    pci::VectorLayerStyle style;
    style.opacity = 0.45F;
    style.zOffset = 12.0;
    style.alwaysOnTop = true;
    CHECK(source.setVectorLayerStyle(id, style));

    pci::SceneDocument destination;
    CHECK(destination.copyOverlayLayersFrom(source));
    REQUIRE(destination.vectorLayer(id));
    CHECK(destination.vectorLayer(id)->data == source.vectorLayer(id)->data);
    CHECK(destination.vectorLayer(id)->style == style);
    CHECK(destination.setAllLayersVisible(false));
    CHECK_FALSE(destination.vectorLayer(id)->visible);
    CHECK(destination.isolateLayer(id));
    CHECK(destination.vectorLayer(id)->visible);
    CHECK(destination.removeLayer(id));
    CHECK(destination.layerKind(id) == pci::SceneLayerKind::None);
}

TEST_CASE("scene document preserves cross-kind insertion order",
          "[unit][scene][vector][order]")
{
    pci::SceneDocument document;
    const auto firstPoint = document.addLayer(
        std::make_shared<pci::PointCloudScene>(pci::PointCloudMetadata{}));
    const auto vector = document.addVectorLayer(vectorData(-2.0, 2.0));
    const auto secondPoint = document.addLayer(
        std::make_shared<pci::PointCloudScene>(pci::PointCloudMetadata{}));

    CHECK(document.layerOrder() ==
          std::vector<pci::SceneLayerId>{firstPoint, vector, secondPoint});

    REQUIRE(document.removeLayer(vector));
    CHECK(document.layerOrder() ==
          std::vector<pci::SceneLayerId>{firstPoint, secondPoint});
}

TEST_CASE("scene document stores one ordered variant layer collection",
          "[unit][scene][vector][variant]")
{
    pci::SceneDocument document;
    const auto pointScene =
        std::make_shared<pci::PointCloudScene>(pci::PointCloudMetadata{});
    const pci::SceneLayerId pointId = document.addLayer(pointScene);
    const pci::SceneLayerId vectorId =
        document.addVectorLayer(vectorData(-4.0, 4.0), false);

    const auto &stored = document.sceneLayers();
    REQUIRE(stored.size() == 2);
    CHECK(stored[0].id == pointId);
    CHECK(stored[0].visible);
    REQUIRE(
        std::holds_alternative<pci::PointCloudLayerState>(stored[0].payload));
    CHECK(std::get<pci::PointCloudLayerState>(stored[0].payload).scene ==
          pointScene);
    CHECK(stored[1].id == vectorId);
    CHECK_FALSE(stored[1].visible);
    REQUIRE(std::holds_alternative<pci::VectorLayerState>(stored[1].payload));
    const pci::Bounds3d &storedBounds =
        std::get<pci::VectorLayerState>(stored[1].payload).data->bounds;
    CHECK(storedBounds.minimum == std::array{-4.0, -4.0, 0.0});
    CHECK(storedBounds.maximum == std::array{4.0, 4.0, 0.0});

    REQUIRE(document.setLayerVisible(vectorId, true));
    CHECK(document.sceneLayers()[1].visible);
    CHECK(document.layers().front().id == pointId);
    CHECK(document.vectorLayers().front().id == vectorId);
}

TEST_CASE("document snapshots are coherent, immutable, and cached",
          "[unit][scene][snapshot][revision]")
{
    pci::SceneDocument document(8192);
    const auto first = document.snapshot();
    REQUIRE(first);
    CHECK(first == document.snapshot());
    CHECK(first->revision == 0);
    CHECK(first->pointRevision == 0);
    CHECK(first->vectorRevision == 0);
    CHECK(first->decodedByteBudget == 8192);

    const auto pointScene =
        std::make_shared<pci::PointCloudScene>(pci::PointCloudMetadata{});
    const pci::SceneLayerId pointId = document.addLayer(pointScene);
    const auto withPoint = document.snapshot();
    CHECK(withPoint != first);
    CHECK(withPoint->revision == 1);
    CHECK(withPoint->pointRevision == 1);
    CHECK(withPoint->vectorRevision == 0);
    REQUIRE(withPoint->layers.size() == 1);
    CHECK(withPoint->layers.front().id == pointId);

    const pci::SceneLayerId vectorId =
        document.addVectorLayer(vectorData(-2.0, 2.0), false);
    const auto withVector = document.snapshot();
    CHECK(withVector != withPoint);
    CHECK(withVector->revision == 2);
    CHECK(withVector->pointRevision == 1);
    CHECK(withVector->vectorRevision == 1);
    REQUIRE(withVector->layers.size() == 2);
    CHECK(withVector->layers.back().id == vectorId);
    CHECK(withPoint->layers.size() == 1);
    CHECK(withVector->bounds.has_value());
    REQUIRE(withVector->visibleBounds.has_value());

    REQUIRE(document.setLayerVisible(vectorId, true));
    const auto visibleVector = document.snapshot();
    CHECK(visibleVector->revision == 3);
    CHECK(visibleVector->pointRevision == 1);
    CHECK(visibleVector->vectorRevision == 2);
    CHECK(visibleVector->visibleBounds.has_value());
}

} // namespace
TEST_CASE("scene layer queries name every payload alternative explicitly",
          "[unit][scene][variant]")
{
    // Counts, kinds, and bounds are each derived by naming the alternative
    // they mean. Deriving one as "everything that is not a point cloud" holds
    // only while exactly two alternatives exist, and fails silently rather
    // than loudly on the day a third is added.
    pci::SceneDocument document;
    const auto first = document.addVectorLayer(vectorData(0.0, 10.0), true);
    const auto second = document.addVectorLayer(vectorData(20.0, 30.0), true);

    const pci::SceneDocumentSnapshotPtr snapshot = document.snapshot();
    REQUIRE(snapshot != nullptr);
    CHECK(document.vectorLayerCount() == snapshot->vectorLayerCount());
    CHECK(document.layerCount() == snapshot->layerCount());
    CHECK(snapshot->vectorLayerCount() == 2);
    CHECK(snapshot->layerCount() == 0);
    CHECK(snapshot->layers.size() == 2);

    // The document and its snapshot must agree on the bounds of each kind, so
    // a projection added on one side cannot drift from the other.
    for (const pci::SceneLayerId id : {first, second}) {
        const auto documentBounds = document.layerBounds(id);
        const auto snapshotBounds = snapshot->layerBounds(id);
        REQUIRE(documentBounds.has_value());
        REQUIRE(snapshotBounds.has_value());
        CHECK(documentBounds->minimum == snapshotBounds->minimum);
        CHECK(documentBounds->maximum == snapshotBounds->maximum);
    }

    // An unknown identifier is absent, not a differently shaped layer.
    const pci::SceneLayerId unknown{9999};
    CHECK(document.layerKind(unknown) == pci::SceneLayerKind::None);
    CHECK_FALSE(document.layerBounds(unknown).has_value());
    CHECK_FALSE(snapshot->layerBounds(unknown).has_value());
}
