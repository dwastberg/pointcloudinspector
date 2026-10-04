#include "support/TestPointDatasets.h"
#include <pci/document/SceneDocument.h>
#include <pci/document/SceneDocumentSnapshot.h>
#include <pci/raster/RasterTileSource.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

// A source that answers metadata questions without touching GDAL, so scene
// behaviour is tested independently of any driver.
class StubRasterSource final : public pci::RasterTileSource {
public:
    explicit StubRasterSource(pci::RasterLayerMetadata metadata)
        : metadata_(std::move(metadata))
    {
    }

    [[nodiscard]] const pci::RasterLayerMetadata &
    metadata() const noexcept override
    {
        return metadata_;
    }

    [[nodiscard]] pci::RasterTileData readTile(const pci::RasterTileRequest &,
                                               std::stop_token) const override
    {
        throw pci::RasterReadError("the stub source holds no pixels");
    }

private:
    pci::RasterLayerMetadata metadata_;
};

[[nodiscard]] pci::RasterLayerMetadata
rasterMetadata(const double originX = 1000.0, const double originY = 2000.0)
{
    pci::RasterLayerMetadata metadata;
    metadata.sourceDriver = "GTiff";
    metadata.width = 64;
    metadata.height = 32;
    metadata.geoTransform = {originX, 1.0, 0.0, originY, 0.0, -1.0};
    metadata.spatialReferenceWkt = "STUBCRS";
    metadata.bounds = *pci::rasterPixelEdgeBounds(
        metadata.geoTransform, metadata.width, metadata.height);
    pci::RasterLevel level;
    level.width = metadata.width;
    level.height = metadata.height;
    level.channelCount = 3;
    metadata.levels.push_back(level);
    return metadata;
}

[[nodiscard]] pci::RasterLayerDataPtr
rasterData(pci::RasterLayerMetadata metadata = rasterMetadata())
{
    return std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = std::make_shared<StubRasterSource>(std::move(metadata)),
    });
}

[[nodiscard]] pci::RasterDatasetDescriptor
rasterDescriptor(pci::RasterLayerMetadata metadata = rasterMetadata())
{
    return rasterData(std::move(metadata))->descriptor();
}

TEST_CASE("scene document keeps rasters in their own revision domain",
          "[unit][scene][raster]")
{
    pci::SceneDocument document;
    const auto id = document.addRasterLayer(rasterDescriptor(), false);

    CHECK_FALSE(document.hasPointCloudLayers());
    CHECK(document.hasAnyLayer());
    CHECK(document.layerCount() == 0);
    CHECK(document.vectorLayerCount() == 0);
    CHECK(document.rasterLayerCount() == 1);
    CHECK(document.layerKind(id) == pci::SceneLayerKind::Raster);
    CHECK(document.revision() == 1);
    CHECK(document.pointRevision() == 0);
    CHECK(document.vectorRevision() == 0);
    CHECK(document.rasterRevision() == 1);

    // Hidden layers contribute to document bounds but not to visible bounds.
    CHECK_FALSE(document.visibleSceneBounds());
    REQUIRE(document.sceneBounds());
    CHECK(document.sceneBounds()->minimum[0] == 1000.0);

    CHECK(document.setLayerVisible(id, true));
    REQUIRE(document.visibleSceneBounds());
    CHECK(document.visibleSceneBounds()->maximum[0] == 1064.0);
    CHECK(document.pointRevision() == 0);
    CHECK(document.vectorRevision() == 0);
    CHECK(document.rasterRevision() == 2);
}

TEST_CASE("raster layer bounds inflate only the flat Z dimension",
          "[unit][scene][raster]")
{
    pci::SceneDocument document;
    const auto id = document.addRasterLayer(rasterDescriptor(), true);

    const auto bounds = document.layerBounds(id);
    REQUIRE(bounds.has_value());
    // XY comes straight from the transformed pixel edges.
    CHECK(bounds->minimum[0] == 1000.0);
    CHECK(bounds->maximum[0] == 1064.0);
    CHECK(bounds->minimum[1] == 1968.0);
    CHECK(bounds->maximum[1] == 2000.0);
    // Only Z is inflated, and it straddles the configured elevation so scene
    // fitting has something to frame while the quad still draws at that Z.
    CHECK(bounds->minimum[2] == -0.5);
    CHECK(bounds->maximum[2] == 0.5);

    pci::RasterLayerStyle raised;
    raised.zOffset = 25.0;
    CHECK(document.setRasterLayerStyle(id, raised));
    const auto moved = document.layerBounds(id);
    REQUIRE(moved.has_value());
    CHECK(moved->minimum[2] == 24.5);
    CHECK(moved->maximum[2] == 25.5);
}

TEST_CASE("raster style changes invalidate pixels only when they must",
          "[unit][scene][raster]")
{
    pci::SceneDocument document;
    const auto id = document.addRasterLayer(rasterDescriptor(), true);
    REQUIRE(document.rasterLayer(id).has_value());
    const std::uint64_t initial = document.rasterLayer(id)->renderGeneration;

    SECTION("opacity and elevation are shader uniforms")
    {
        pci::RasterLayerStyle style = document.rasterLayer(id)->style;
        style.opacity = 0.4F;
        style.zOffset = 12.0;
        CHECK(document.setRasterLayerStyle(id, style));

        // Bumping the generation here would discard every reusable tile on
        // each slider movement.
        CHECK(document.rasterLayer(id)->renderGeneration == initial);
        CHECK(document.rasterRevision() == 2);
    }

    SECTION("display range and ramp alter decoded pixels")
    {
        pci::RasterLayerStyle style = document.rasterLayer(id)->style;
        style.displayRange =
            pci::RasterDisplayRange{.minimum = 5.0, .maximum = 50.0};
        CHECK(document.setRasterLayerStyle(id, style));
        CHECK(document.rasterLayer(id)->renderGeneration == initial + 1);

        style.colorRampKey = "magma";
        CHECK(document.setRasterLayerStyle(id, style));
        CHECK(document.rasterLayer(id)->renderGeneration == initial + 2);
    }

    SECTION("an unchanged style is not a change")
    {
        const std::uint64_t revision = document.rasterRevision();
        CHECK(
            document.setRasterLayerStyle(id, document.rasterLayer(id)->style));
        CHECK(document.rasterRevision() == revision);
        CHECK(document.rasterLayer(id)->renderGeneration == initial);
    }

    SECTION("styling an absent layer reports failure")
    {
        CHECK_FALSE(document.setRasterLayerStyle(pci::SceneLayerId{9999}, {}));
    }
}

TEST_CASE("raster layers reject invalid descriptors", "[unit][scene][raster]")
{
    pci::SceneDocument document;
    CHECK_THROWS_AS(document.addRasterLayer({}), std::invalid_argument);

    pci::RasterLayerMetadata singular = rasterMetadata();
    singular.geoTransform = {0.0, 1.0, 2.0, 0.0, 2.0, 4.0};
    CHECK_THROWS_AS(document.addRasterLayer(rasterDescriptor(singular)),
                    std::invalid_argument);

    // Two layers may not share a source: the tile cache is keyed by source
    // identity, so they would share cache entries.
    const pci::RasterDatasetDescriptor shared = rasterDescriptor();
    static_cast<void>(document.addRasterLayer(shared));
    CHECK_THROWS_AS(document.addRasterLayer(shared), std::invalid_argument);
}

TEST_CASE("scene document retains raster metadata without retaining its source",
          "[unit][scene][raster][ownership]")
{
    auto source = std::make_shared<StubRasterSource>(rasterMetadata());
    std::weak_ptr<const pci::RasterTileSource> sourceLifetime = source;
    pci::RasterLayerDataPtr data =
        std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
            .sourceId = pci::nextRasterSourceId(),
            .source = source,
        });
    source.reset();

    pci::SceneDocument document;
    const pci::SceneLayerId id = document.addRasterLayer(data->descriptor());
    const pci::RasterSourceId sourceId = data->sourceId;
    data.reset();

    CHECK(sourceLifetime.expired());
    const auto layer = document.rasterLayer(id);
    REQUIRE(layer);
    CHECK(layer->descriptor.sourceId == sourceId);
    CHECK(layer->descriptor.metadata.width == 64);
}

TEST_CASE("snapshot projections agree with the document for rasters",
          "[unit][scene][raster][variant]")
{
    pci::SceneDocument document;
    const pci::RasterLayerDataPtr firstData = rasterData();
    const auto raster = document.addRasterLayer(firstData->descriptor(), true);
    const auto second = document.addRasterLayer(
        rasterDescriptor(rasterMetadata(5000.0, 9000.0)), false);

    const pci::SceneDocumentSnapshotPtr snapshot = document.snapshot();
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->rasterRevision == document.rasterRevision());
    CHECK(snapshot->rasterLayerCount() == 2);
    // vectorLayerCount() must count its own alternative, not "everything that
    // is not a point cloud".
    CHECK(snapshot->vectorLayerCount() == 0);
    CHECK(snapshot->layerCount() == 0);
    CHECK(snapshot->rasterLayers().size() == 2);
    CHECK(snapshot->pointLayerIndices.empty());
    CHECK(snapshot->vectorLayerIndices.empty());
    CHECK(snapshot->rasterLayerIndices == std::vector<std::size_t>{0, 1});

    REQUIRE(snapshot->rasterLayer(raster).has_value());
    CHECK(snapshot->rasterLayer(raster)->descriptor.sourceId ==
          firstData->sourceId);
    CHECK(snapshot->rasterLayer(raster)->descriptor.metadata.sourceDriver ==
          firstData->metadata().sourceDriver);
    CHECK(snapshot->rasterLayer(raster)->visible);
    CHECK_FALSE(snapshot->rasterLayer(second)->visible);
    CHECK_FALSE(snapshot->vectorLayer(raster).has_value());
    CHECK_FALSE(snapshot->layer(raster).has_value());

    // layerBounds() must not treat a raster as a vector; doing so is a bad
    // variant access rather than a missing feature.
    const auto documentBounds = document.layerBounds(raster);
    const auto snapshotBounds = snapshot->layerBounds(raster);
    REQUIRE(documentBounds.has_value());
    REQUIRE(snapshotBounds.has_value());
    CHECK(documentBounds->minimum == snapshotBounds->minimum);
    CHECK(documentBounds->maximum == snapshotBounds->maximum);
}

TEST_CASE("overlay copying preserves ids for vectors and rasters alike",
          "[unit][scene][raster]")
{
    pci::SceneDocument source;
    auto vector = std::make_shared<pci::VectorLayerData>();
    vector->bounds = {.minimum = {0.0, 0.0, 0.0}, .maximum = {1.0, 1.0, 0.0}};
    const auto vectorId = source.addVectorLayer(vector, true);
    const auto rasterId = source.addRasterLayer(rasterDescriptor(), true);

    pci::SceneDocument destination;
    CHECK(destination.copyOverlayLayersFrom(source));
    CHECK(destination.vectorLayerCount() == 1);
    CHECK(destination.rasterLayerCount() == 1);

    // Renderer caches are keyed by SceneLayerId, so a copy that reassigned ids
    // would evict and re-upload every overlay on each point-cloud replacement.
    REQUIRE(destination.rasterLayer(rasterId).has_value());
    REQUIRE(destination.vectorLayer(vectorId).has_value());
    CHECK(destination.layerOrder() == source.layerOrder());
    CHECK(destination.rasterRevision() == 1);
    CHECK(destination.vectorRevision() == 1);

    // Copying twice is idempotent rather than duplicating.
    CHECK_FALSE(destination.copyOverlayLayersFrom(source));
    CHECK(destination.rasterLayerCount() == 1);
}

TEST_CASE("raster layers participate in isolation and removal",
          "[unit][scene][raster]")
{
    pci::SceneDocument document;
    const auto first = document.addRasterLayer(rasterDescriptor(), true);
    const auto second = document.addRasterLayer(
        rasterDescriptor(rasterMetadata(7000.0, 8000.0)), true);

    CHECK(document.isolateLayer(second));
    CHECK_FALSE(document.rasterLayer(first)->visible);
    CHECK(document.rasterLayer(second)->visible);

    CHECK(document.removeLayer(first));
    CHECK(document.rasterLayerCount() == 1);
    CHECK_FALSE(document.rasterLayer(first).has_value());
    CHECK(document.layerKind(first) == pci::SceneLayerKind::None);
    // Visibility and removal belong to the payload's own revision domain.
    // Treating every non-point layer as a vector leaves raster-only consumers
    // looking at a stale revision even though the document revision changed.
    CHECK(document.pointRevision() == 0);
    CHECK(document.vectorRevision() == 0);
    CHECK(document.rasterRevision() == 4);
}

TEST_CASE("raster point-color provenance survives source-layer removal",
          "[unit][scene][raster][colorize]")
{
    const pci::PointDatasetView pointDataset = pci::test::pointDataset(
        pci::PointCloudMetadata{.sourcePointCount = 1},
        pci::PointDatasetAvailability{
            .bounds = {},
            .pointCount = 1,
            .colorizeAvailability = pci::PointColorizeAvailability::Ready,
        });

    pci::SceneDocument document;
    const pci::PointCloudLayerId pointId = document.addLayer(pointDataset);
    const pci::RasterLayerDataPtr data = rasterData();
    const pci::SceneLayerId rasterId =
        document.addRasterLayer(data->descriptor());
    CHECK_FALSE(
        document.setLayerColorMode(pointId,
                                   {.source = pci::PointColorSource::Rgb,
                                    .colorMap = pci::PointColorMap::Rgb}));

    auto decode = std::make_shared<pci::RasterDecodeParameters>();
    CHECK(document.setLayerRasterColors(
        pointId,
        {.rasterLayerId = rasterId,
         .rasterSourceId = data->sourceId,
         .rasterSourcePath = "source.tif",
         .decode = decode,
         .rasterRenderGeneration = 3,
         .coloredPoints = 1,
         .uncoloredPoints = 2,
         .crsRelation = pci::SpatialReferenceRelation::Different}));
    REQUIRE(document.layer(pointId)->rasterColors.has_value());
    CHECK(document.layer(pointId)->colorGeneration == 1);
    CHECK(document.setLayerColorMode(pointId,
                                     {.source = pci::PointColorSource::Rgb,
                                      .colorMap = pci::PointColorMap::Rgb}));

    CHECK(document.removeLayer(rasterId));
    const auto point = document.layer(pointId);
    REQUIRE(point.has_value());
    REQUIRE(point->rasterColors.has_value());
    CHECK_FALSE(point->rasterColors->rasterLayerId.has_value());
    CHECK(point->rasterColors->rasterSourceId == data->sourceId);
    CHECK(point->rasterColors->rasterSourcePath == "source.tif");
    // Unlinking provenance does not alter baked bytes or their generation.
    CHECK(point->colorGeneration == 1);

    const auto snapshot = document.snapshot()->layer(pointId);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->rasterColors.has_value());
    CHECK_FALSE(snapshot->rasterColors->rasterLayerId.has_value());
    CHECK(snapshot->colorGeneration == 1);

    CHECK(document.clearLayerRasterColors(pointId));
    CHECK(document.layer(pointId)->colorGeneration == 2);
    CHECK_FALSE(document.layer(pointId)->rasterColors.has_value());
}

TEST_CASE("reference CRS prefers point clouds over rasters",
          "[unit][scene][raster]")
{
    pci::SceneDocument empty;
    CHECK(empty.referenceSpatialReferenceWkt().empty());

    pci::SceneDocument document;
    pci::RasterLayerMetadata unreferenced = rasterMetadata();
    unreferenced.spatialReferenceWkt.clear();
    static_cast<void>(document.addRasterLayer(rasterDescriptor(unreferenced)));
    // The first raster carries no CRS, so the scan continues rather than
    // settling for an empty answer.
    CHECK(document.referenceSpatialReferenceWkt().empty());

    static_cast<void>(document.addRasterLayer(rasterDescriptor()));
    CHECK(document.referenceSpatialReferenceWkt() == "STUBCRS");
}

TEST_CASE("exact DEM range updates bounds without invalidating decoded color",
          "[unit][scene][raster][surface]")
{
    pci::RasterLayerMetadata metadata = rasterMetadata();
    metadata.elevation.available = true;
    metadata.elevation.anchor = 100.0;
    pci::SceneDocument document;
    const pci::SceneLayerId id =
        document.addRasterLayer(rasterDescriptor(std::move(metadata)), true);

    REQUIRE(document.rasterLayer(id).has_value());
    CHECK(document.rasterLayer(id)->elevationStatus ==
          pci::RasterElevationStatus::Unknown);
    const std::uint64_t renderGeneration =
        document.rasterLayer(id)->renderGeneration;

    pci::RasterLayerStyle style = document.rasterLayer(id)->style;
    style.renderMode = pci::RasterRenderMode::Surface;
    style.verticalExaggeration = 3.0;
    style.zOffset = -5.0;
    REQUIRE(document.setRasterLayerStyle(id, style));
    CHECK(document.rasterLayer(id)->renderGeneration == renderGeneration);
    REQUIRE(document.setRasterElevationState(
        id,
        pci::RasterElevationStatus::Ready,
        pci::RasterElevationRange{.minimum = 10.0, .maximum = 20.0}));

    const pci::RasterLayer layer = *document.rasterLayer(id);
    CHECK(layer.renderGeneration == renderGeneration);
    CHECK(layer.elevationGeneration == 1);
    REQUIRE(document.visibleSceneBounds().has_value());
    CHECK(document.visibleSceneBounds()->minimum[2] == 25.0);
    CHECK(document.visibleSceneBounds()->maximum[2] == 55.0);
    REQUIRE(document.snapshot()->rasterLayer(id).has_value());
    CHECK(document.snapshot()->rasterLayer(id)->exactElevationRange ==
          pci::RasterElevationRange{.minimum = 10.0, .maximum = 20.0});
}

} // namespace
