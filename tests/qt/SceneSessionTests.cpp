#include "app/SceneSession.h"
#include "support/TestPointColorMaps.h"

#include "import/PointCloudLoadController.h"

#include <catch2/catch_test_macros.hpp>

#include <QSignalSpy>
#include <QTest>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

template <typename Predicate> bool waitFor(Predicate &&predicate)
{
    return QTest::qWaitFor(std::forward<Predicate>(predicate), 2000);
}

pci::PointCloudScenePtr makeScene(const std::filesystem::path &sourcePath)
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePath = sourcePath;
    metadata.sourcePointCount = 1;
    metadata.sourceBounds = {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};
    metadata.hasColor = true;
    metadata.hasClassification = true;
    auto scene = std::make_shared<pci::PointCloudScene>(std::move(metadata));
    auto block = std::make_shared<pci::PointBlock>();
    block->points.resize(1);
    block->attributes.resize(1);
    scene->addBlock(std::move(block));
    return scene;
}

class SessionLoader final : public pci::PointCloudLoader {
public:
    mutable std::atomic_bool slowStarted = false;
    mutable std::atomic_bool releaseSlow = false;
    mutable std::atomic_bool previewPublished = false;
    mutable std::atomic_bool releaseFailure = false;

    pci::PointCloudScenePtr
    load(const pci::PointCloudLoadOptions &options,
         const pci::PointCloudLoadResources &,
         const pci::PointCloudImportPreflight &,
         const pci::PointCloudLoadContext &context) const override
    {
        const std::filesystem::path filename = options.sourcePath.filename();
        if (filename == "slow-first.las") {
            slowStarted = true;
            waitUntil(releaseSlow, context.stopToken);
        }

        pci::PointCloudScenePtr scene = makeScene(options.sourcePath);
        if (context.sceneReady) {
            context.sceneReady(scene);
        }

        if (filename == "cancel-after-preview.las") {
            previewPublished = true;
            waitUntil(releaseSlow, context.stopToken);
        } else if (filename == "fail-after-preview.las") {
            previewPublished = true;
            waitUntil(releaseFailure, context.stopToken);
            throw pci::PointCloudImportError("failure after preview");
        }
        return scene;
    }

private:
    static void waitUntil(const std::atomic_bool &released,
                          const std::stop_token stopToken)
    {
        while (!released.load() && !stopToken.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (stopToken.stop_requested()) {
            throw pci::PointCloudImportCancelled();
        }
    }
};

class UnavailableVectorLoader final : public pci::VectorLoader {
public:
    pci::VectorImportPreflight
    inspect(const pci::VectorImportRequest &request) const override
    {
        pci::VectorImportPreflight result;
        for (const pci::VectorSublayerKey &key : request.sublayers) {
            result.sublayers.push_back({.key = key});
        }
        return result;
    }

    std::array<double, 2>
    probeOrigin(const pci::VectorImportRequest &,
                std::span<const pci::VectorSublayerKey>) const override
    {
        return {};
    }

    pci::VectorLayerDataPtr loadSublayer(const pci::VectorImportRequest &,
                                         pci::VectorSublayerKey) const override
    {
        throw std::runtime_error("unused vector loader");
    }
};

class PartialVectorLoader final : public pci::VectorLoader {
public:
    pci::VectorImportPreflight
    inspect(const pci::VectorImportRequest &request) const override
    {
        pci::VectorImportPreflight result;
        for (const pci::VectorSublayerKey &key : request.sublayers) {
            result.sublayers.push_back({.key = key});
        }
        return result;
    }

    std::array<double, 2>
    probeOrigin(const pci::VectorImportRequest &,
                std::span<const pci::VectorSublayerKey>) const override
    {
        return {};
    }

    pci::VectorLayerDataPtr
    loadSublayer(const pci::VectorImportRequest &,
                 const pci::VectorSublayerKey key) const override
    {
        if (key.name == "bad") {
            throw pci::VectorImportError("bad sublayer");
        }
        auto data = std::make_shared<pci::VectorLayerData>();
        data->sublayerName = key.name;
        data->bounds = {
            .minimum = {0.0, 0.0, 0.0},
            .maximum = {1.0, 1.0, 0.0},
        };
        return data;
    }
};

class UnavailableRasterLoader final : public pci::RasterLoader {
public:
    pci::RasterImportPreflight
    inspect(const pci::RasterImportRequest &) const override
    {
        throw pci::RasterImportError("unused raster loader");
    }
};

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

    [[nodiscard]] pci::RasterTileData
    readTile(const pci::RasterTileRequest &request,
             const std::stop_token stop) const override
    {
        if (stop.stop_requested()) {
            throw pci::RasterReadCancelled();
        }
        ++reads;
        pci::RasterTileData tile;
        tile.key = request.key;
        tile.renderGeneration = request.renderGeneration;
        tile.validWidth = static_cast<std::uint16_t>(metadata_.width);
        tile.validHeight = static_cast<std::uint16_t>(metadata_.height);
        tile.rgba.resize(pci::rasterStoredTileBytes);
        for (std::size_t offset = 0; offset < tile.rgba.size(); offset += 4) {
            tile.rgba[offset + 0] = std::byte{0x11};
            tile.rgba[offset + 1] = std::byte{0x22};
            tile.rgba[offset + 2] = std::byte{0x33};
            tile.rgba[offset + 3] = std::byte{0xff};
        }
        return tile;
    }

    mutable std::atomic_uint64_t reads = 0;

private:
    pci::RasterLayerMetadata metadata_;
};

class SessionRasterLoader final : public pci::RasterLoader {
public:
    pci::RasterImportPreflight
    inspect(const pci::RasterImportRequest &request) const override
    {
        lastTargetWkt = request.targetSpatialReferenceWkt;
        if (request.sourcePath.filename() == "broken.tif") {
            throw pci::RasterImportError("stub raster failure");
        }
        pci::RasterLayerMetadata metadata;
        metadata.sourcePath = request.sourcePath;
        metadata.sourceDriver = "GTiff";
        metadata.width = 64;
        metadata.height = 32;
        metadata.geoTransform = {0.0, 1.0, 0.0, 32.0, 0.0, -1.0};
        metadata.bounds = *pci::rasterPixelEdgeBounds(
            metadata.geoTransform, metadata.width, metadata.height);
        pci::RasterLevel level;
        level.width = metadata.width;
        level.height = metadata.height;
        level.channelCount = 3;
        metadata.levels.push_back(level);
        return {
            .data = std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
                .sourceId = pci::nextRasterSourceId(),
                .source =
                    std::make_shared<StubRasterSource>(std::move(metadata)),
            })};
    }

    mutable std::string lastTargetWkt;
};

class UnavailableStatistics final : public pci::PointCloudStatisticsProvider {
public:
    pci::PointCloudStatistics calculate(const pci::PointCloudMetadata &,
                                        std::stop_token,
                                        Progress) const override
    {
        throw std::runtime_error("unused statistics provider");
    }
};

pci::ImportServices
makeServices(const std::shared_ptr<const pci::PointCloudLoader> &loader)
{
    pci::ImportServices services;
    services.scheduler = std::make_unique<pci::TaskScheduler>();
    services.pointCloud = std::make_unique<pci::PointCloudLoadController>(
        loader, *services.scheduler);
    services.vector = std::make_unique<pci::VectorLoadController>(
        std::make_shared<UnavailableVectorLoader>(), *services.scheduler);
    services.raster = std::make_unique<pci::RasterLoadController>(
        std::make_shared<UnavailableRasterLoader>(), *services.scheduler);
    services.rasterElevation =
        std::make_unique<pci::RasterElevationController>(*services.scheduler);
    services.colorize = std::make_unique<pci::PointCloudColorizeController>(
        *services.scheduler);
    services.statistics = std::make_shared<UnavailableStatistics>();
    return services;
}

pci::ImportServices
makeServices(const std::shared_ptr<const pci::PointCloudLoader> &pointLoader,
             const std::shared_ptr<const pci::VectorLoader> &vectorLoader)
{
    pci::ImportServices services = makeServices(pointLoader);
    services.vector = std::make_unique<pci::VectorLoadController>(
        vectorLoader, *services.scheduler);
    return services;
}

pci::ImportServices
makeServices(const std::shared_ptr<const pci::PointCloudLoader> &pointLoader,
             const std::shared_ptr<const pci::RasterLoader> &rasterLoader)
{
    pci::ImportServices services = makeServices(pointLoader);
    services.raster = std::make_unique<pci::RasterLoadController>(
        rasterLoader, *services.scheduler);
    return services;
}

void finishVisibleLoads(pci::SceneSession &session)
{
    REQUIRE(waitFor([&session] {
        return !session.document()->layers().empty();
    }));
    REQUIRE(waitFor([&session] {
        const auto states = session.pointLoadJobStates();
        return !states.empty() &&
               std::ranges::all_of(states, [](const auto &state) {
                   return state.phase == pci::PointCloudLoadJobPhase::Ready;
               });
    }));
    for (const pci::PointCloudLayer &layer : session.document()->layers()) {
        session.onRenderLoadProgress({
            .layerId = layer.id,
            .stage = pci::RenderLoadStage::DisplayReady,
        });
    }
    REQUIRE(waitFor([&session] {
        return !session.loading();
    }));
}

} // namespace

TEST_CASE("scene session retires replace and add loads at display readiness",
          "[scene-session][loading]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader),
                              100,
                              1024 * 1024,
                              std::nullopt,
                              {},
                              pci::test::createTestPointColorMapCatalog());

    session.loadPointCloud("first.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);
    CHECK(session.document()->layerCount() == 1);

    session.loadPointCloud("second.las", pci::PointCloudLoadMode::Add);
    REQUIRE(waitFor([&session] {
        return session.document()->layerCount() == 2;
    }));
    finishVisibleLoads(session);
    CHECK(session.document()->layerCount() == 2);
}

TEST_CASE("scene session orders out-of-order batch previews",
          "[scene-session][batch]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader), 100, 1024 * 1024);

    session.loadPointClouds({"slow-first.las", "fast-second.las"},
                            pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&loader] {
        return loader->slowStarted.load();
    }));
    REQUIRE(waitFor([&session] {
        return session.document()->layerCount() == 1;
    }));
    CHECK(session.document()->layers().front().scene->metadata().sourcePath ==
          std::filesystem::path("fast-second.las"));

    loader->releaseSlow = true;
    REQUIRE(waitFor([&session] {
        return session.document()->layerCount() == 2;
    }));
    const auto layers = session.document()->layers();
    CHECK(layers[0].scene->metadata().sourcePath ==
          std::filesystem::path("slow-first.las"));
    CHECK(layers[1].scene->metadata().sourcePath ==
          std::filesystem::path("fast-second.las"));
    finishVisibleLoads(session);
}

TEST_CASE("scene session rolls back cancellation and failure after preview",
          "[scene-session][rollback]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader), 100, 1024 * 1024);
    session.loadPointCloud("original.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);

    session.loadPointCloud("cancel-after-preview.las",
                           pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&loader] {
        return loader->previewPublished.load();
    }));
    REQUIRE(waitFor([&session] {
        return session.document()
                   ->layers()
                   .front()
                   .scene->metadata()
                   .sourcePath ==
               std::filesystem::path("cancel-after-preview.las");
    }));
    session.cancelAll();
    REQUIRE(waitFor([&session] {
        return !session.loading();
    }));
    CHECK(session.document()->layers().front().scene->metadata().sourcePath ==
          std::filesystem::path("original.las"));

    loader->previewPublished = false;
    session.loadPointCloud("fail-after-preview.las",
                           pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&loader] {
        return loader->previewPublished.load();
    }));
    loader->releaseFailure = true;
    REQUIRE(waitFor([&session] {
        return !session.loading();
    }));
    CHECK(session.document()->layers().front().scene->metadata().sourcePath ==
          std::filesystem::path("original.las"));
}

TEST_CASE("scene session owns document commands", "[scene-session][commands]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader),
                              100,
                              1024 * 1024,
                              std::nullopt,
                              {},
                              pci::test::createTestPointColorMapCatalog());
    session.loadPointCloud("first.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);
    session.loadPointCloud("second.las", pci::PointCloudLoadMode::Add);
    REQUIRE(waitFor([&session] {
        return session.document()->layerCount() == 2;
    }));
    finishVisibleLoads(session);

    const auto layers = session.document()->layers();
    session.setLayerVisible(layers.front().id, false);
    CHECK_FALSE(session.document()->layer(layers.front().id)->visible);
    session.isolateLayer(layers.front().id);
    CHECK(session.document()->layer(layers.front().id)->visible);
    CHECK_FALSE(session.document()->layer(layers.back().id)->visible);
    session.showAllLayers();
    CHECK(session.document()->layer(layers.back().id)->visible);

    const pci::PointColorMode mode{
        .source = pci::PointColorSource::X,
        .colorMap = pci::PointColorMap::Turbo,
    };
    session.setAllLayerColors(mode);
    CHECK(session.document()->layer(layers.front().id)->colorMode == mode);
    CHECK(session.document()->layer(layers.back().id)->colorMode == mode);

    pci::PointClassificationFilter filter =
        pci::PointClassificationFilter::noneVisible();
    filter.setVisible(2, true);
    session.setLayerClassificationFilter(layers.front().id, filter, false);
    CHECK(session.document()->layer(layers.front().id)->classificationFilter ==
          filter);
    session.removeLayer(layers.back().id);
    CHECK(session.document()->layerCount() == 1);
}

TEST_CASE("new scene clears layers and cannot restore a cancelled replacement",
          "[scene-session][commands][loading]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader),
                              100,
                              1024 * 1024,
                              std::nullopt,
                              {},
                              pci::test::createTestPointColorMapCatalog());
    session.loadPointCloud("original.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);

    auto vector = std::make_shared<pci::VectorLayerData>();
    vector->sublayerName = "survey";
    vector->bounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 0.0},
    };
    static_cast<void>(session.document()->addVectorLayer(std::move(vector)));
    SessionRasterLoader rasterLoader;
    const pci::RasterImportPreflight raster =
        rasterLoader.inspect({.sourcePath = "ortho.tif"});
    static_cast<void>(session.document()->addRasterLayer(raster.data));
    CHECK(session.document()->sceneLayers().size() == 3);

    const auto originalDocument = session.document();
    const auto colorMaps = originalDocument->colorMaps();
    QSignalSpy documentChanged(&session, &pci::SceneSession::documentChanged);

    session.loadPointCloud("cancel-after-preview.las",
                           pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&loader] {
        return loader->previewPublished.load();
    }));
    REQUIRE(session.document() != originalDocument);

    session.newScene();

    CHECK_FALSE(session.loading());
    CHECK(session.document() != originalDocument);
    CHECK_FALSE(session.document()->hasAnyLayer());
    CHECK(session.document()->colorMaps() == colorMaps);
    CHECK(session.document()->decodedByteBudget() == 1024 * 1024);
    REQUIRE_FALSE(documentChanged.empty());
    CHECK_FALSE(documentChanged.back().at(1).toBool());
    CHECK(documentChanged.back().at(2).toBool());

    REQUIRE(waitFor([&session] {
        const auto states = session.pointLoadJobStates();
        return states.size() == 2 && states.back().phase ==
                                         pci::PointCloudLoadJobPhase::Cancelled;
    }));
    CHECK_FALSE(session.document()->hasAnyLayer());
}

TEST_CASE("scene session updates future-load and decoded-memory budgets",
          "[scene-session][settings]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(
        makeServices(loader), 100, std::uint64_t{1024} * 1024);

    session.setMaximumLoadPoints(250);
    CHECK(session.maximumLoadPoints() == 250);
    CHECK(session.setDecodedByteBudget(std::uint64_t{2} * 1024 * 1024,
                                       std::nullopt));
    CHECK(session.decodedByteBudget() == std::uint64_t{2} * 1024 * 1024);
    CHECK_FALSE(session.automaticMemoryBudgetEnabled());
    CHECK(session.document()->decodedByteBudget() ==
          std::uint64_t{2} * 1024 * 1024);
}

TEST_CASE("scene session publishes unified rows for partial vector success",
          "[scene-session][tasks][vector]")
{
    auto pointLoader = std::make_shared<SessionLoader>();
    auto vectorLoader = std::make_shared<PartialVectorLoader>();
    pci::SceneSession session(
        makeServices(pointLoader, vectorLoader), 100, 1024 * 1024);
    pci::LoadJobRows latestRows;
    QObject::connect(&session,
                     &pci::SceneSession::taskRowsChanged,
                     [&latestRows](pci::LoadJobRows rows) {
                         latestRows = std::move(rows);
                     });

    pci::VectorImportRequest request;
    request.sourcePath = "survey.gpkg";
    request.limits.maximumApplicationWorkingBytes = 1024;
    request.sublayers = {{.index = 0, .name = "good"},
                         {.index = 1, .name = "bad"}};
    static_cast<void>(session.loadVectorLayers(std::move(request)));

    REQUIRE(waitFor([&session] {
        return session.document()->vectorLayerCount() == 1;
    }));
    REQUIRE(waitFor([&latestRows] {
        return latestRows.size() == 1 && latestRows.front().terminal &&
               latestRows.front().capabilities.canRetry;
    }));
    CHECK(latestRows.front().key.kind == pci::LoadJobKind::Vector);
    CHECK(latestRows.front().capabilities.canRetry);
    CHECK(latestRows.front().detail.contains(
        QStringLiteral("Retry failed sublayers")));
}

TEST_CASE("scene session adds inspected rasters to the document",
          "[scene-session][raster]")
{
    auto pointLoader = std::make_shared<SessionLoader>();
    auto rasterLoader = std::make_shared<SessionRasterLoader>();
    pci::SceneSession session(makeServices(pointLoader, rasterLoader),
                              100,
                              1024 * 1024,
                              std::nullopt,
                              {},
                              pci::test::createTestPointColorMapCatalog());

    QSignalSpy documentChanged(&session, &pci::SceneSession::documentChanged);
    static_cast<void>(session.startRasterImport({.sourcePath = "ortho.tif"}));

    REQUIRE(waitFor([&session] {
        return session.document()->rasterLayerCount() == 1;
    }));
    CHECK(session.document()->layerCount() == 0);
    CHECK(session.document()->vectorLayerCount() == 0);

    // The first layer in an empty document frames the view.
    REQUIRE_FALSE(documentChanged.empty());
    CHECK(documentChanged.back().at(1).toBool());

    // A second raster is additive and must not move the camera.
    static_cast<void>(session.startRasterImport({.sourcePath = "second.tif"}));
    REQUIRE(waitFor([&session] {
        return session.document()->rasterLayerCount() == 2;
    }));
    CHECK_FALSE(documentChanged.back().at(1).toBool());
}

TEST_CASE("scene session keeps raster task rows alongside point and vector",
          "[scene-session][raster][tasks]")
{
    auto pointLoader = std::make_shared<SessionLoader>();
    auto rasterLoader = std::make_shared<SessionRasterLoader>();
    pci::SceneSession session(makeServices(pointLoader, rasterLoader),
                              100,
                              1024 * 1024,
                              std::nullopt,
                              {},
                              pci::test::createTestPointColorMapCatalog());

    QSignalSpy rows(&session, &pci::SceneSession::taskRowsChanged);
    const pci::LoadJobId job =
        session.startRasterImport({.sourcePath = "ortho.tif"});
    REQUIRE(waitFor([&session] {
        return session.document()->rasterLayerCount() == 1;
    }));

    // Task rows are published through the session signal, so they are read
    // back from the last emission rather than from a controller directly.
    REQUIRE_FALSE(rows.empty());
    const auto published = rows.back().at(0).value<pci::LoadJobRows>();
    const auto raster = std::ranges::find(
        published, pci::LoadJobKind::Raster, [](const pci::LoadJobRow &row) {
            return row.key.kind;
        });
    REQUIRE(raster != published.end());
    CHECK(raster->key.id == job);
    CHECK(raster->terminal);

    // A keyed dismiss must reach the raster controller rather than falling
    // through to the vector one, where it would be silently ignored.
    session.dismissJob(
        pci::LoadJobKey{.kind = pci::LoadJobKind::Raster, .id = job});
    REQUIRE(waitFor([&rows] {
        const auto latest = rows.back().at(0).value<pci::LoadJobRows>();
        return std::ranges::none_of(latest, [](const pci::LoadJobRow &row) {
            return row.key.kind == pci::LoadJobKind::Raster;
        });
    }));
}

TEST_CASE("scene session reports a failed raster without adding a layer",
          "[scene-session][raster]")
{
    auto pointLoader = std::make_shared<SessionLoader>();
    auto rasterLoader = std::make_shared<SessionRasterLoader>();
    pci::SceneSession session(makeServices(pointLoader, rasterLoader),
                              100,
                              1024 * 1024,
                              std::nullopt,
                              {},
                              pci::test::createTestPointColorMapCatalog());

    QSignalSpy status(&session, &pci::SceneSession::statusChanged);
    static_cast<void>(session.startRasterImport({.sourcePath = "broken.tif"}));
    REQUIRE(waitFor([&status] {
        return !status.empty();
    }));

    CHECK(status.back().at(0).toString().contains(
        QStringLiteral("stub raster failure")));
    CHECK(session.document()->rasterLayerCount() == 0);
    CHECK_FALSE(session.hasActiveRasterLoads());
}

TEST_CASE("scene session commits and reverts raster point colors",
          "[scene-session][raster][colorize]")
{
    auto pointLoader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(pointLoader),
                              100,
                              32ULL * 1024 * 1024,
                              std::nullopt,
                              {},
                              pci::test::createTestPointColorMapCatalog());
    QSignalSpy status(&session, &pci::SceneSession::statusChanged);
    pci::LoadJobRows latestRows;
    QObject::connect(&session,
                     &pci::SceneSession::taskRowsChanged,
                     [&latestRows](pci::LoadJobRows rows) {
                         latestRows = std::move(rows);
                     });

    pci::PointCloudMetadata pointMetadata;
    pointMetadata.sourcePath = "target.laz";
    pointMetadata.sourcePointCount = 1;
    pointMetadata.sourceBounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {0.0, 0.0, 0.0},
    };
    auto scene =
        std::make_shared<pci::PointCloudScene>(std::move(pointMetadata));
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = {0.0, 0.0, 0.0};
    block->scale = 1.0;
    block->bounds = {.minimum = {0.0, 0.0, 0.0}, .maximum = {0.0, 0.0, 0.0}};
    block->points.push_back({.rgba = 0xffabcdefU});
    scene->addBlock(block);
    scene->markLoadingComplete();
    const pci::PointCloudLayerId pointId = session.document()->addLayer(scene);

    pci::RasterLayerMetadata rasterMetadata;
    rasterMetadata.sourcePath = "colors.tif";
    rasterMetadata.width = 4;
    rasterMetadata.height = 4;
    rasterMetadata.geoTransform = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    rasterMetadata.bounds =
        *pci::rasterPixelEdgeBounds(rasterMetadata.geoTransform,
                                    rasterMetadata.width,
                                    rasterMetadata.height);
    rasterMetadata.levels.push_back({.width = rasterMetadata.width,
                                     .height = rasterMetadata.height,
                                     .channelCount = 3});
    auto rasterSource =
        std::make_shared<StubRasterSource>(std::move(rasterMetadata));
    auto rasterData =
        std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
            .sourceId = pci::nextRasterSourceId(),
            .source = rasterSource,
        });
    const pci::SceneLayerId rasterId =
        session.document()->addRasterLayer(rasterData);

    const pci::LoadJobId job =
        session.colorizePointCloudFromRaster(pointId, rasterId);
    REQUIRE(waitFor([&] {
        const auto layer = session.document()->layer(pointId);
        return layer && layer->rasterColors.has_value();
    }));
    REQUIRE(waitFor([&latestRows] {
        return latestRows.size() == 1 && latestRows.front().terminal;
    }));
    CHECK(latestRows.front().title == QStringLiteral("Colorize target.laz"));
    CHECK(
        latestRows.front().detail.contains(QStringLiteral("From colors.tif")));
    CHECK(latestRows.front().detail.contains(
        QStringLiteral("kept source color")));
    REQUIRE_FALSE(status.empty());
    CHECK(status.front().at(0).toString() ==
          QStringLiteral("Colorizing target.laz from colors.tif…"));
    CHECK(status.back().at(0).toString().contains(
        QStringLiteral("Colorized target.laz from colors.tif")));
    const auto colored = session.document()->layer(pointId);
    REQUIRE(colored.has_value());
    REQUIRE(colored->rasterColors.has_value());
    CHECK(colored->rasterColors->rasterLayerId == rasterId);
    CHECK(colored->rasterColors->rasterSourceId == rasterData->sourceId);
    CHECK(colored->colorGeneration == 1);
    CHECK(colored->colorMode.source == pci::PointColorSource::Rgb);
    CHECK(scene->blockEntries().front().block->points.front().rgba ==
          0xff332211U);
    CHECK(rasterSource->reads.load() == 1);

    const auto rows = session.colorizeMetrics();
    CHECK(rows.activeColorTableBytes == 0);
    CHECK(session.revertPointCloudColors(pointId));
    CHECK_FALSE(session.document()->layer(pointId)->rasterColors.has_value());
    CHECK(session.document()->layer(pointId)->colorGeneration == 2);
    CHECK(scene->blockEntries().front().block->points.front().rgba ==
          0xffabcdefU);
    CHECK(status.back().at(0).toString() ==
          QStringLiteral("Restored source colors for target.laz."));
    CHECK_FALSE(session.revertPointCloudColors(pointId));

    session.dismissJob({.kind = pci::LoadJobKind::Colorize, .id = job});
}
