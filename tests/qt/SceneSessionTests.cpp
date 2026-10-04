#include "support/InMemoryRasterColorizeRunStore.h"
#include "support/TestPointColorMaps.h"
#include <pci/desktop/session/PointSceneCoordinator.h>
#include <pci/desktop/session/SceneSession.h>

#include <pci/desktop/operations/PointCloudLoadController.h>

#include <pci/operations/PointDatasetInstallation.h>

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

namespace pci {

class SceneSessionTestAccess final {
public:
    static void failNextPointCommit(SceneSession &session)
    {
        session.beforePointCommit_ = [&session] {
            session.beforePointCommit_ = {};
            throw std::bad_alloc();
        };
    }

    static void publishBlock(SceneSession &session,
                             PointCloudLayerId id,
                             PointBlockPtr block)
    {
        SceneSession::ActiveLoad load{};
        load.layerId = id;
        load.scene = pointRuntime(session, id);
        session.commitPointPublication(
            load, load.scene->preparePublication(std::move(block)));
    }

    [[nodiscard]] static SceneDocumentPtr
    document(const SceneSession &session) noexcept
    {
        return session.document_;
    }

    [[nodiscard]] static SceneLayerId
    addRasterLayer(SceneSession &session,
                   const RasterLayerDataPtr &data,
                   const bool initiallyVisible = true)
    {
        const BindingGeneration binding = session.allocateBindingGeneration();
        session.attachRasterRuntime(data, binding);
        try {
            return session.document_->addRasterLayer(
                data->descriptor(), initiallyVisible, binding);
        } catch (...) {
            static_cast<void>(session.runtime_.detach(binding));
            throw;
        }
    }

    [[nodiscard]] static RasterTileSourcePtr
    rasterSource(const SceneSession &session, const SceneLayerId id)
    {
        const auto layer = session.document_->rasterLayer(id);
        return layer ? session.runtime_.raster(layer->descriptor.sourceId,
                                               layer->bindingGeneration)
                     : RasterTileSourcePtr{};
    }

    [[nodiscard]] static std::uint64_t
    runtimeDecodedByteBudget(const SceneSession &session) noexcept
    {
        return session.runtime_.decodedByteBudget();
    }

    [[nodiscard]] static PointDatasetRuntimePtr
    pointRuntime(const SceneSession &session, const PointCloudLayerId id)
    {
        const auto layer = session.document_->layer(id);
        return layer ? session.runtime_.point(layer->descriptor.sourceId,
                                              layer->bindingGeneration)
                     : PointDatasetRuntimePtr{};
    }

    [[nodiscard]] static bool detachPointRuntime(SceneSession &session,
                                                 const PointCloudLayerId id)
    {
        const auto layer = session.document_->layer(id);
        return layer && session.runtime_.detach(layer->bindingGeneration);
    }

    [[nodiscard]] static std::optional<bool>
    pointRuntimeActive(const SceneSession &session, const PointCloudLayerId id)
    {
        const auto layer = session.document_->layer(id);
        return layer ? session.runtime_.snapshot()->pointActive(
                           layer->descriptor.sourceId, layer->bindingGeneration)
                     : std::nullopt;
    }
};

} // namespace pci

namespace {

[[nodiscard]] pci::SceneDocumentPtr
testDocument(const pci::SceneSession &session) noexcept
{
    return pci::SceneSessionTestAccess::document(session);
}

[[nodiscard]] pci::SceneLayerId
addTestRasterLayer(pci::SceneSession &session,
                   const pci::RasterLayerDataPtr &data,
                   const bool initiallyVisible = true)
{
    return pci::SceneSessionTestAccess::addRasterLayer(
        session, data, initiallyVisible);
}

template <typename Predicate> bool waitFor(Predicate &&predicate)
{
    return QTest::qWaitFor(std::forward<Predicate>(predicate), 2000);
}

std::shared_ptr<pci::PointDatasetPreparation>
makePreparation(const std::filesystem::path &sourcePath,
                const std::uint64_t pointCount = 1)
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePath = sourcePath;
    metadata.sourcePointCount = pointCount;
    metadata.sourceBounds = {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};
    metadata.hasColor = true;
    metadata.hasClassification = true;
    auto scene =
        std::make_shared<pci::PointDatasetPreparation>(std::move(metadata));
    if (pointCount == 0) {
        return scene;
    }
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = {0.0, 0.0, 0.0};
    block->scale = 1.0;
    block->bounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    block->points.resize(static_cast<std::size_t>(pointCount));
    block->attributes.resize(static_cast<std::size_t>(pointCount));
    scene->addBlock(std::move(block));
    return scene;
}

struct ManualClock {
    std::chrono::steady_clock::time_point value{};

    [[nodiscard]] std::chrono::steady_clock::time_point now() const
    {
        return value;
    }

    void advance(const std::chrono::milliseconds duration)
    {
        value += duration;
    }
};

class SessionLoader final : public pci::PointCloudLoader {
public:
    mutable std::atomic_bool slowStarted = false;
    mutable std::atomic_bool releaseSlow = false;
    mutable std::atomic_bool previewPublished = false;
    mutable std::atomic_bool releaseFailure = false;
    mutable std::atomic_int retryFailuresRemaining = 0;

    pci::PreparedPointDatasetPtr
    load(const pci::PointCloudLoadOptions &options,
         const pci::PointCloudLoadResources &,
         const pci::PointCloudImportPreflight &,
         const pci::PointCloudLoadContext &context) const override
    {
        const std::filesystem::path filename = options.sourcePath.filename();
        if (filename == "retryable.las" && retryFailuresRemaining.load() > 0) {
            --retryFailuresRemaining;
            throw pci::PointCloudImportError("retryable fixture failure");
        }
        if (filename == "slow-first.las") {
            slowStarted = true;
            waitUntil(releaseSlow, context.stopToken);
        }

        auto scene = makePreparation(options.sourcePath,
                                     filename == "empty.las" ? 0 : 1);
        scene->publish(context);

        if (filename == "cancel-after-preview.las") {
            previewPublished = true;
            waitUntil(releaseSlow, context.stopToken);
        } else if (filename == "fail-after-preview.las") {
            previewPublished = true;
            waitUntil(releaseFailure, context.stopToken);
            throw pci::PointCloudImportError("failure after preview");
        }

        return scene->finish();
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

class BlockingElevationRasterSource final : public pci::RasterTileSource {
public:
    BlockingElevationRasterSource()
    {
        metadata_.sourcePath = "preserved-dem.tif";
        metadata_.width = 16;
        metadata_.height = 16;
        metadata_.geoTransform = {0.0, 1.0, 0.0, 16.0, 0.0, -1.0};
        metadata_.bounds = *pci::rasterPixelEdgeBounds(
            metadata_.geoTransform, metadata_.width, metadata_.height);
        metadata_.elevation = {
            .available = true,
            .band = 1,
            .scale = 1.0,
            .offset = 0.0,
            .unit = "m",
        };
        metadata_.levels.push_back({.width = metadata_.width,
                                    .height = metadata_.height,
                                    .channelCount = 1});
    }

    [[nodiscard]] const pci::RasterLayerMetadata &
    metadata() const noexcept override
    {
        return metadata_;
    }

    [[nodiscard]] pci::RasterTileData readTile(const pci::RasterTileRequest &,
                                               std::stop_token) const override
    {
        throw pci::RasterReadError("unused tile read");
    }

    [[nodiscard]] std::uint64_t
    exactElevationScanReservationBytes() const noexcept override
    {
        return 1024;
    }

    [[nodiscard]] pci::RasterElevationRange exactElevationRange(
        const std::stop_token stop,
        pci::RasterElevationProgressCallback = {}) const override
    {
        scanning = true;
        while (!release.load()) {
            if (stop.stop_requested()) {
                throw pci::RasterReadCancelled{};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return {.minimum = 12.0, .maximum = 48.0};
    }

    mutable std::atomic_bool scanning = false;
    std::atomic_bool release = false;

private:
    pci::RasterLayerMetadata metadata_;
};

class BlockingElevationRasterLoader final : public pci::RasterLoader {
public:
    explicit BlockingElevationRasterLoader(
        std::shared_ptr<BlockingElevationRasterSource> source)
        : source_(std::move(source))
        , sourceId_(pci::nextRasterSourceId())
    {
    }

    [[nodiscard]] pci::RasterImportPreflight
    inspect(const pci::RasterImportRequest &) const override
    {
        return {.data =
                    std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
                        .sourceId = sourceId_, .source = source_})};
    }

    [[nodiscard]] pci::RasterSourceId sourceId() const noexcept
    {
        return sourceId_;
    }

private:
    std::shared_ptr<BlockingElevationRasterSource> source_;
    pci::RasterSourceId sourceId_;
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
        metadata.geoTransform =
            request.sourcePath.filename() == "colors.tif"
                ? std::array<double, 6>{0.0, 1.0, 0.0, 0.0, 0.0, 1.0}
                : std::array<double, 6>{0.0, 1.0, 0.0, 32.0, 0.0, -1.0};
        metadata.bounds = *pci::rasterPixelEdgeBounds(
            metadata.geoTransform, metadata.width, metadata.height);
        metadata.elevation = {
            .available = true,
            .band = 1,
            .scale = 1.0,
            .offset = 0.0,
            .unit = "m",
        };
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
        *services.scheduler,
        pci::test::inMemoryRasterColorizeRunStoreFactory());
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
        return !testDocument(session)->layers().empty();
    }));
    REQUIRE(waitFor([&session] {
        const auto states = session.pointLoadJobStates();
        return !states.empty() &&
               std::ranges::all_of(states, [](const auto &state) {
                   return state.phase == pci::PointCloudLoadJobPhase::Ready;
               });
    }));
    for (const pci::PointCloudLayer &layer : testDocument(session)->layers()) {
        session.onRenderLoadProgress({
            .layerId = layer.id,
            .bindingGeneration = layer.bindingGeneration,
            .stage = pci::RenderLoadStage::DisplayReady,
        });
    }
    REQUIRE(waitFor([&session] {
        return !session.loading();
    }));
}

void waitForPointImports(pci::SceneSession &session)
{
    REQUIRE(waitFor([&session] {
        const auto states = session.pointLoadJobStates();
        return !states.empty() &&
               std::ranges::all_of(states, [](const auto &state) {
                   return state.phase == pci::PointCloudLoadJobPhase::Ready;
               });
    }));
}

pci::PointCloudLayerId layerForSource(const pci::SceneSession &session,
                                      const std::filesystem::path &source)
{
    const std::vector<pci::PointCloudLayer> layers =
        testDocument(session)->layers();
    const auto found = std::ranges::find(
        layers, source, [](const pci::PointCloudLayer &layer) {
            return layer.descriptor.metadata.sourcePath;
        });
    REQUIRE(found != layers.end());
    return found->id;
}

} // namespace

TEST_CASE("scene session preserves overlay identity and state on replacement",
          "[scene-session][characterization][replace][layers]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader), 100, 1024 * 1024);
    session.loadPointCloud("first.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);

    auto vectorData = std::make_shared<pci::VectorLayerData>();
    vectorData->sublayerName = "survey";
    vectorData->bounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 0.0},
    };
    const pci::SceneLayerId vectorId =
        testDocument(session)->addVectorLayer(vectorData);

    session.loadPointCloud("second.las", pci::PointCloudLoadMode::Add);
    finishVisibleLoads(session);

    SessionRasterLoader rasterLoader;
    const auto rasterData =
        rasterLoader.inspect({.sourcePath = "ortho.tif"}).data;
    const pci::SceneLayerId rasterId = addTestRasterLayer(session, rasterData);

    pci::VectorLayerStyle vectorStyle =
        testDocument(session)->vectorLayer(vectorId)->style;
    vectorStyle.opacity = 0.35F;
    vectorStyle.alwaysOnTop = true;
    session.setVectorLayerStyle(vectorId, vectorStyle);
    session.setLayerVisible(vectorId, false);

    pci::RasterLayerStyle rasterStyle =
        testDocument(session)->rasterLayer(rasterId)->style;
    rasterStyle.opacity = 0.65F;
    rasterStyle.zOffset = 12.0;
    session.setRasterLayerStyle(rasterId, rasterStyle);
    REQUIRE(testDocument(session)->setRasterElevationState(
        rasterId,
        pci::RasterElevationStatus::Ready,
        pci::RasterElevationRange{.minimum = 5.0, .maximum = 25.0}));

    const auto originalVector = testDocument(session)->vectorLayer(vectorId);
    const auto originalRaster = testDocument(session)->rasterLayer(rasterId);
    REQUIRE(originalVector);
    REQUIRE(originalRaster);
    const auto originalRasterSource =
        pci::SceneSessionTestAccess::rasterSource(session, rasterId);
    REQUIRE(originalRasterSource);
    const std::vector overlayOrder{vectorId, rasterId};

    session.loadPointCloud("replacement.las", pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&session] {
        return testDocument(session)->layerCount() == 1 &&
               testDocument(session)
                       ->layers()
                       .front()
                       .descriptor.metadata.sourcePath ==
                   std::filesystem::path("replacement.las");
    }));

    const auto replacedVector = testDocument(session)->vectorLayer(vectorId);
    const auto replacedRaster = testDocument(session)->rasterLayer(rasterId);
    REQUIRE(replacedVector);
    REQUIRE(replacedRaster);
    CHECK(replacedVector->data == originalVector->data);
    CHECK(replacedVector->style == originalVector->style);
    CHECK(replacedVector->visible == originalVector->visible);
    CHECK(replacedVector->bindingGeneration ==
          originalVector->bindingGeneration);
    CHECK(replacedRaster->descriptor.sourceId ==
          originalRaster->descriptor.sourceId);
    CHECK(replacedRaster->descriptor.metadata.sourcePath ==
          originalRaster->descriptor.metadata.sourcePath);
    CHECK(pci::SceneSessionTestAccess::rasterSource(session, rasterId) ==
          originalRasterSource);
    CHECK(replacedRaster->style == originalRaster->style);
    CHECK(replacedRaster->visible == originalRaster->visible);
    CHECK(replacedRaster->bindingGeneration ==
          originalRaster->bindingGeneration);
    CHECK(replacedRaster->elevationStatus == originalRaster->elevationStatus);
    CHECK(replacedRaster->exactElevationRange ==
          originalRaster->exactElevationRange);
    CHECK(replacedRaster->elevationGeneration ==
          originalRaster->elevationGeneration);
    CHECK(replacedRaster->renderGeneration == originalRaster->renderGeneration);

    std::vector<pci::SceneLayerId> retainedOverlayOrder;
    for (const pci::SceneLayer &layer : testDocument(session)->sceneLayers()) {
        if (layer.id == vectorId || layer.id == rasterId) {
            retainedOverlayOrder.push_back(layer.id);
        }
    }
    CHECK(retainedOverlayOrder == overlayOrder);
    CHECK(testDocument(session)->vectorLayerCount() == 1);
    CHECK(testDocument(session)->rasterLayerCount() == 1);

    finishVisibleLoads(session);
}

TEST_CASE("scene session preserves an active raster operation across point "
          "replacement",
          "[scene-session][generation][replace][raster][surface]")
{
    auto loader = std::make_shared<SessionLoader>();
    auto rasterSource = std::make_shared<BlockingElevationRasterSource>();
    auto rasterLoader =
        std::make_shared<BlockingElevationRasterLoader>(rasterSource);
    pci::SceneSession session(
        makeServices(loader, rasterLoader), 100, 1024 * 1024);
    session.loadPointCloud("first.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);

    static_cast<void>(
        session.startRasterImport({.sourcePath = "preserved-dem.tif"}));
    REQUIRE(waitFor([&session] {
        return testDocument(session)->rasterLayerCount() == 1;
    }));
    const pci::SceneLayerId rasterId =
        testDocument(session)->rasterLayers().front().id;
    const auto originalRaster = testDocument(session)->rasterLayer(rasterId);
    REQUIRE(originalRaster);

    pci::RasterLayerStyle surfaceStyle = originalRaster->style;
    surfaceStyle.renderMode = pci::RasterRenderMode::Surface;
    session.setRasterLayerStyle(rasterId, surfaceStyle);
    REQUIRE(waitFor([&rasterSource] {
        return rasterSource->scanning.load();
    }));

    session.loadPointCloud("replacement.las", pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&session] {
        const auto snapshot = session.documentSnapshot();
        return snapshot->pointLayers().size() == 1 &&
               snapshot->pointLayers().front().descriptor.metadata.sourcePath ==
                   std::filesystem::path("replacement.las");
    }));

    const auto retainedRaster = testDocument(session)->rasterLayer(rasterId);
    REQUIRE(retainedRaster);
    CHECK(retainedRaster->descriptor.sourceId == rasterLoader->sourceId());
    CHECK(retainedRaster->bindingGeneration ==
          originalRaster->bindingGeneration);
    CHECK(retainedRaster->elevationStatus ==
          pci::RasterElevationStatus::Scanning);

    rasterSource->release = true;
    REQUIRE(waitFor([&session, rasterId] {
        const auto layer = testDocument(session)->rasterLayer(rasterId);
        return layer &&
               layer->elevationStatus == pci::RasterElevationStatus::Ready;
    }));

    const auto completedRaster = testDocument(session)->rasterLayer(rasterId);
    REQUIRE(completedRaster);
    CHECK(completedRaster->bindingGeneration ==
          originalRaster->bindingGeneration);
    CHECK(completedRaster->exactElevationRange ==
          pci::RasterElevationRange{.minimum = 12.0, .maximum = 48.0});

    finishVisibleLoads(session);
}

TEST_CASE("scene session distinguishes first-frame and display milestones",
          "[scene-session][characterization][loading][timings]")
{
    ManualClock clock;
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader),
                              100,
                              1024 * 1024,
                              std::nullopt,
                              {},
                              {},
                              nullptr,
                              [&clock] {
                                  return clock.now();
                              });
    QSignalSpy loadingChanged(&session, &pci::SceneSession::loadingChanged);

    session.loadPointCloud("milestones.las", pci::PointCloudLoadMode::Replace);
    waitForPointImports(session);
    const auto layerId = testDocument(session)->layers().front().id;
    CHECK(session.loading());

    clock.advance(std::chrono::milliseconds(25));
    session.onRenderLoadProgress({
        .layerId = layerId,
        .bindingGeneration =
            testDocument(session)->layer(layerId)->bindingGeneration,
        .stage = pci::RenderLoadStage::FirstFrameReady,
    });
    CHECK(session.loading());
    CHECK_FALSE(session.timings().timeToFirstPointsMilliseconds);

    clock.advance(std::chrono::milliseconds(15));
    session.onRenderLoadProgress({
        .layerId = layerId,
        .bindingGeneration =
            testDocument(session)->layer(layerId)->bindingGeneration,
        .stage = pci::RenderLoadStage::DisplayReady,
    });
    REQUIRE_FALSE(session.loading());
    const pci::SceneSessionTimings timings = session.timings();
    REQUIRE(timings.timeToFirstPointsMilliseconds);
    REQUIRE(timings.timeToAllFirstPointsMilliseconds);
    REQUIRE(timings.displayReadyMilliseconds);
    CHECK(*timings.timeToFirstPointsMilliseconds == 25.0);
    CHECK(*timings.timeToAllFirstPointsMilliseconds == 25.0);
    CHECK(*timings.displayReadyMilliseconds == 40.0);

    const qsizetype signalCount = loadingChanged.count();
    session.onRenderLoadProgress({
        .layerId = layerId,
        .bindingGeneration =
            testDocument(session)->layer(layerId)->bindingGeneration,
        .stage = pci::RenderLoadStage::FirstFrameReady,
    });
    session.onRenderLoadProgress({
        .layerId = layerId,
        .bindingGeneration =
            testDocument(session)->layer(layerId)->bindingGeneration,
        .stage = pci::RenderLoadStage::DisplayReady,
    });
    CHECK(loadingChanged.count() == signalCount);
    CHECK(session.timings().timeToFirstPointsMilliseconds ==
          timings.timeToFirstPointsMilliseconds);
    CHECK(session.timings().displayReadyMilliseconds ==
          timings.displayReadyMilliseconds);
}

TEST_CASE(
    "display readiness implies first-frame readiness and waits for import",
    "[scene-session][characterization][loading][timings]")
{
    ManualClock clock;
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader),
                              100,
                              1024 * 1024,
                              std::nullopt,
                              {},
                              {},
                              nullptr,
                              [&clock] {
                                  return clock.now();
                              });

    session.loadPointCloud("cancel-after-preview.las",
                           pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&loader] {
        return loader->previewPublished.load();
    }));
    const auto layerId = testDocument(session)->layers().front().id;
    clock.advance(std::chrono::milliseconds(17));
    session.onRenderLoadProgress({
        .layerId = layerId,
        .bindingGeneration =
            testDocument(session)->layer(layerId)->bindingGeneration,
        .stage = pci::RenderLoadStage::DisplayReady,
    });
    CHECK(session.loading());

    loader->releaseSlow = true;
    REQUIRE(waitFor([&session] {
        return !session.loading();
    }));
    const pci::SceneSessionTimings timings = session.timings();
    REQUIRE(timings.timeToFirstPointsMilliseconds);
    REQUIRE(timings.displayReadyMilliseconds);
    CHECK(*timings.timeToFirstPointsMilliseconds == 17.0);
    CHECK(*timings.displayReadyMilliseconds == 17.0);
}

TEST_CASE("scene session reports deterministic out-of-order batch milestones",
          "[scene-session][characterization][batch][timings]")
{
    ManualClock clock;
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader),
                              100,
                              1024 * 1024,
                              std::nullopt,
                              {},
                              {},
                              nullptr,
                              [&clock] {
                                  return clock.now();
                              });

    session.loadPointClouds({"slow-first.las", "fast-second.las"},
                            pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&loader] {
        return loader->slowStarted.load();
    }));
    REQUIRE(waitFor([&session] {
        return testDocument(session)->layerCount() == 1;
    }));
    const pci::PointCloudLayerId fastId =
        layerForSource(session, "fast-second.las");
    clock.advance(std::chrono::milliseconds(10));
    session.onRenderLoadProgress({
        .layerId = fastId,
        .bindingGeneration =
            testDocument(session)->layer(fastId)->bindingGeneration,
        .stage = pci::RenderLoadStage::DisplayReady,
    });
    CHECK(session.loading());

    loader->releaseSlow = true;
    REQUIRE(waitFor([&session] {
        return testDocument(session)->layerCount() == 2;
    }));
    const pci::PointCloudLayerId slowId =
        layerForSource(session, "slow-first.las");
    clock.advance(std::chrono::milliseconds(15));
    session.onRenderLoadProgress({
        .layerId = slowId,
        .bindingGeneration =
            testDocument(session)->layer(slowId)->bindingGeneration,
        .stage = pci::RenderLoadStage::FirstFrameReady,
    });
    CHECK(session.loading());
    clock.advance(std::chrono::milliseconds(5));
    session.onRenderLoadProgress({
        .layerId = slowId,
        .bindingGeneration =
            testDocument(session)->layer(slowId)->bindingGeneration,
        .stage = pci::RenderLoadStage::DisplayReady,
    });
    REQUIRE_FALSE(session.loading());

    const pci::SceneSessionTimings timings = session.timings();
    REQUIRE(timings.timeToFirstPointsMilliseconds);
    REQUIRE(timings.timeToAllFirstPointsMilliseconds);
    REQUIRE(timings.displayReadyMilliseconds);
    CHECK(*timings.timeToFirstPointsMilliseconds == 10.0);
    CHECK(*timings.timeToAllFirstPointsMilliseconds == 25.0);
    CHECK(*timings.displayReadyMilliseconds == 30.0);
    const auto layers = testDocument(session)->layers();
    CHECK(layers[0].id == slowId);
    CHECK(layers[1].id == fastId);
}

TEST_CASE("empty point sources retire without a render milestone",
          "[scene-session][characterization][loading][empty]")
{
    ManualClock clock;
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader),
                              100,
                              1024 * 1024,
                              std::nullopt,
                              {},
                              {},
                              nullptr,
                              [&clock] {
                                  return clock.now();
                              });
    session.loadPointCloud("empty.las", pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&session] {
        return !session.loading();
    }));
    CHECK(testDocument(session)->layerCount() == 1);
    CHECK(testDocument(session)->layers().front().availability.pointCount == 0);
    CHECK_FALSE(session.timings().timeToFirstPointsMilliseconds);
    CHECK_FALSE(session.timings().timeToAllFirstPointsMilliseconds);
    REQUIRE(session.timings().displayReadyMilliseconds);
    CHECK(*session.timings().displayReadyMilliseconds == 0.0);
}

TEST_CASE("point retry creates a fresh row and job identity",
          "[scene-session][characterization][tasks][recovery]")
{
    auto loader = std::make_shared<SessionLoader>();
    loader->retryFailuresRemaining = 1;
    pci::SceneSession session(makeServices(loader), 100, 1024 * 1024);
    session.loadPointCloud("retryable.las", pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&session] {
        const auto states = session.pointLoadJobStates();
        return states.size() == 1 &&
               states.front().phase == pci::PointCloudLoadJobPhase::Failed;
    }));
    const pci::LoadJobId failedId = session.pointLoadJobStates().front().jobId;

    session.retryJob(failedId);
    REQUIRE(waitFor([&session, failedId] {
        const auto states = session.pointLoadJobStates();
        return states.size() == 1 && states.front().jobId != failedId &&
               states.front().phase == pci::PointCloudLoadJobPhase::Ready;
    }));
    CHECK(testDocument(session)->layerCount() == 1);
    finishVisibleLoads(session);
}

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
    CHECK(testDocument(session)->layerCount() == 1);

    session.loadPointCloud("second.las", pci::PointCloudLoadMode::Add);
    REQUIRE(waitFor([&session] {
        return testDocument(session)->layerCount() == 2;
    }));
    finishVisibleLoads(session);
    CHECK(testDocument(session)->layerCount() == 2);
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
        return testDocument(session)->layerCount() == 1;
    }));
    CHECK(testDocument(session)
              ->layers()
              .front()
              .descriptor.metadata.sourcePath ==
          std::filesystem::path("fast-second.las"));

    loader->releaseSlow = true;
    REQUIRE(waitFor([&session] {
        return testDocument(session)->layerCount() == 2;
    }));
    const auto layers = testDocument(session)->layers();
    CHECK(layers[0].descriptor.metadata.sourcePath ==
          std::filesystem::path("slow-first.las"));
    CHECK(layers[1].descriptor.metadata.sourcePath ==
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

    auto vectorData = std::make_shared<pci::VectorLayerData>();
    vectorData->sublayerName = "rollback-survey";
    vectorData->bounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 0.0},
    };
    const pci::SceneLayerId vectorId =
        testDocument(session)->addVectorLayer(vectorData);
    SessionRasterLoader rasterLoader;
    const auto rasterData =
        rasterLoader.inspect({.sourcePath = "rollback-ortho.tif"}).data;
    const pci::SceneLayerId rasterId = addTestRasterLayer(session, rasterData);

    session.loadPointCloud("cancel-after-preview.las",
                           pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&loader] {
        return loader->previewPublished.load();
    }));
    REQUIRE(waitFor([&session] {
        return testDocument(session)
                   ->layers()
                   .front()
                   .descriptor.metadata.sourcePath ==
               std::filesystem::path("cancel-after-preview.las");
    }));
    session.cancelAll();
    REQUIRE(waitFor([&session] {
        return !session.loading();
    }));
    CHECK(testDocument(session)
              ->layers()
              .front()
              .descriptor.metadata.sourcePath ==
          std::filesystem::path("original.las"));
    REQUIRE(testDocument(session)->vectorLayer(vectorId));
    REQUIRE(testDocument(session)->rasterLayer(rasterId));
    CHECK(testDocument(session)->vectorLayer(vectorId)->data == vectorData);
    CHECK(pci::SceneSessionTestAccess::rasterSource(session, rasterId) ==
          rasterData->source);

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
    CHECK(testDocument(session)
              ->layers()
              .front()
              .descriptor.metadata.sourcePath ==
          std::filesystem::path("original.las"));
    REQUIRE(testDocument(session)->vectorLayer(vectorId));
    REQUIRE(testDocument(session)->rasterLayer(rasterId));
    CHECK(testDocument(session)->vectorLayer(vectorId)->data == vectorData);
    CHECK(pci::SceneSessionTestAccess::rasterSource(session, rasterId) ==
          rasterData->source);
}

TEST_CASE("scene session generations follow installation and reset boundaries",
          "[scene-session][generation][rollback]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader), 100, 1024 * 1024);

    const pci::SessionGeneration initialSession = session.sessionGeneration();
    const pci::DocumentGeneration initialDocument =
        testDocument(session)->snapshot()->generation;
    CHECK(initialSession == pci::SessionGeneration{1});
    CHECK(initialDocument == pci::DocumentGeneration{1});

    session.loadPointCloud("original.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);
    const pci::PointCloudLayer originalLayer =
        testDocument(session)->layers().front();
    session.setLayerVisible(originalLayer.id, false);
    session.setLayerVisible(originalLayer.id, true);
    CHECK(testDocument(session)->snapshot()->generation == initialDocument);
    CHECK(session.sessionGeneration() == initialSession);

    loader->previewPublished = false;
    session.loadPointCloud("cancel-after-preview.las",
                           pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&loader] {
        return loader->previewPublished.load();
    }));
    const pci::DocumentGeneration replacementDocument =
        testDocument(session)->snapshot()->generation;
    CHECK(replacementDocument == pci::nextGeneration(initialDocument));
    CHECK(testDocument(session)->layers().front().bindingGeneration !=
          originalLayer.bindingGeneration);
    CHECK(session.sessionGeneration() == initialSession);

    session.cancelAll();
    REQUIRE(waitFor([&session] {
        return !session.loading();
    }));
    const pci::DocumentGeneration restoredDocument =
        testDocument(session)->snapshot()->generation;
    CHECK(restoredDocument == pci::nextGeneration(replacementDocument));
    CHECK(testDocument(session)->layers().front().bindingGeneration !=
          originalLayer.bindingGeneration);
    CHECK(session.sessionGeneration() == initialSession);

    session.newScene();
    CHECK(session.sessionGeneration() == pci::nextGeneration(initialSession));
    CHECK(testDocument(session)->snapshot()->generation ==
          pci::nextGeneration(restoredDocument));
}

TEST_CASE("scene session rejects readiness from retired point bindings",
          "[scene-session][generation][loading][replace]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader), 100, 1024 * 1024);

    session.loadPointCloud("original.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);
    const pci::PointCloudLayer original =
        testDocument(session)->layers().front();

    session.loadPointCloud("replacement.las", pci::PointCloudLoadMode::Replace);
    waitForPointImports(session);
    const pci::PointCloudLayer replacement =
        testDocument(session)->layers().front();
    REQUIRE(replacement.id == original.id);
    REQUIRE(replacement.bindingGeneration != original.bindingGeneration);

    session.onRenderLoadProgress({
        .layerId = original.id,
        .bindingGeneration = original.bindingGeneration,
        .stage = pci::RenderLoadStage::DisplayReady,
    });
    CHECK(session.loading());
    CHECK_FALSE(session.timings().displayReadyMilliseconds);

    session.onRenderLoadProgress({
        .layerId = replacement.id,
        .bindingGeneration = replacement.bindingGeneration,
        .stage = pci::RenderLoadStage::DisplayReady,
    });
    REQUIRE_FALSE(session.loading());

    session.newScene();
    session.loadPointCloud("after-reset.las", pci::PointCloudLoadMode::Replace);
    waitForPointImports(session);
    const pci::PointCloudLayer afterReset =
        testDocument(session)->layers().front();
    REQUIRE(afterReset.id == replacement.id);
    REQUIRE(afterReset.bindingGeneration != replacement.bindingGeneration);

    session.onRenderLoadProgress({
        .layerId = replacement.id,
        .bindingGeneration = replacement.bindingGeneration,
        .stage = pci::RenderLoadStage::DisplayReady,
    });
    CHECK(session.loading());
    session.onRenderLoadProgress({
        .layerId = afterReset.id,
        .bindingGeneration = afterReset.bindingGeneration,
        .stage = pci::RenderLoadStage::DisplayReady,
    });
    CHECK_FALSE(session.loading());
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
        return testDocument(session)->layerCount() == 2;
    }));
    finishVisibleLoads(session);

    const auto layers = testDocument(session)->layers();
    session.setLayerVisible(layers.front().id, false);
    CHECK_FALSE(testDocument(session)->layer(layers.front().id)->visible);
    CHECK(pci::SceneSessionTestAccess::pointRuntimeActive(
              session, layers.front().id) == false);
    session.isolateLayer(layers.front().id);
    CHECK(testDocument(session)->layer(layers.front().id)->visible);
    CHECK_FALSE(testDocument(session)->layer(layers.back().id)->visible);
    CHECK(pci::SceneSessionTestAccess::pointRuntimeActive(
              session, layers.front().id) == true);
    CHECK(pci::SceneSessionTestAccess::pointRuntimeActive(
              session, layers.back().id) == false);
    session.showAllLayers();
    CHECK(testDocument(session)->layer(layers.back().id)->visible);
    CHECK(pci::SceneSessionTestAccess::pointRuntimeActive(
              session, layers.back().id) == true);

    const pci::PointColorMode mode{
        .source = pci::PointColorSource::X,
        .colorMap = pci::PointColorMap::Turbo,
    };
    session.setAllLayerColors(mode);
    CHECK(testDocument(session)->layer(layers.front().id)->colorMode == mode);
    CHECK(testDocument(session)->layer(layers.back().id)->colorMode == mode);

    pci::PointClassificationFilter filter =
        pci::PointClassificationFilter::noneVisible();
    filter.setVisible(2, true);
    session.setLayerClassificationFilter(layers.front().id, filter, false);
    CHECK(
        testDocument(session)->layer(layers.front().id)->classificationFilter ==
        filter);
    session.removeLayer(layers.back().id);
    CHECK(testDocument(session)->layerCount() == 1);
}

TEST_CASE("point visibility stays unchanged when its runtime is unavailable",
          "[scene-session][commands][regression][runtime]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader), 100, 1024 * 1024);
    session.loadPointCloud("visibility.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);
    const pci::PointCloudLayerId id =
        testDocument(session)->layers().front().id;
    REQUIRE(testDocument(session)->layer(id)->visible);
    REQUIRE(pci::SceneSessionTestAccess::detachPointRuntime(session, id));

    CHECK_THROWS_AS(session.setLayerVisible(id, false), std::logic_error);

    CHECK(testDocument(session)->layer(id)->visible);
}

TEST_CASE("document-only edits do not republish task rows",
          "[scene-session][tasks][document-update]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader), 100, 1024 * 1024);
    session.loadPointCloud("editable.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);

    const pci::PointCloudLayerId layerId =
        testDocument(session)->layers().front().id;
    QSignalSpy taskRowsChanged(&session, &pci::SceneSession::taskRowsChanged);
    QSignalSpy documentChanged(&session, &pci::SceneSession::documentChanged);

    session.setLayerVisible(layerId, false);

    CHECK(documentChanged.count() == 1);
    CHECK(taskRowsChanged.empty());
    const auto update =
        documentChanged.back().at(0).value<pci::DocumentUpdate>();
    CHECK(update.sessionGeneration == session.sessionGeneration());
    REQUIRE(update.runtime);
    CHECK(update.runtime->pointBindingCount() == 1);
    const auto point = update.snapshot->pointLayers().front();
    CHECK(update.runtime->point(point.descriptor.sourceId,
                                point.bindingGeneration));
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
    static_cast<void>(testDocument(session)->addVectorLayer(std::move(vector)));
    SessionRasterLoader rasterLoader;
    const pci::RasterImportPreflight raster =
        rasterLoader.inspect({.sourcePath = "ortho.tif"});
    static_cast<void>(addTestRasterLayer(session, raster.data));
    CHECK(testDocument(session)->sceneLayers().size() == 3);

    const auto originalDocument = testDocument(session);
    const auto colorMaps = originalDocument->colorMaps();
    QSignalSpy documentChanged(&session, &pci::SceneSession::documentChanged);

    session.loadPointCloud("cancel-after-preview.las",
                           pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&loader] {
        return loader->previewPublished.load();
    }));
    REQUIRE(testDocument(session) != originalDocument);

    session.newScene();

    CHECK_FALSE(session.loading());
    CHECK(testDocument(session) != originalDocument);
    CHECK_FALSE(testDocument(session)->hasAnyLayer());
    CHECK(testDocument(session)->colorMaps() == colorMaps);
    CHECK(pci::SceneSessionTestAccess::runtimeDecodedByteBudget(session) ==
          1024 * 1024);
    REQUIRE_FALSE(documentChanged.empty());
    const auto newSceneUpdate =
        documentChanged.back().at(0).value<pci::DocumentUpdate>();
    CHECK(newSceneUpdate.viewAdjustment == pci::ViewAdjustment::Preserve);
    CHECK(newSceneUpdate.rendererPolicy ==
          pci::RendererDocumentPolicy::ResetPointView);

    REQUIRE(waitFor([&session] {
        const auto states = session.pointLoadJobStates();
        return states.size() == 2 &&
               states.back().phase == pci::PointCloudLoadJobPhase::Cancelled;
    }));
    CHECK_FALSE(testDocument(session)->hasAnyLayer());
}

TEST_CASE("scene session updates future-load and decoded-memory budgets",
          "[scene-session][settings]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(
        makeServices(loader), 100, std::uint64_t{1024} * 1024);

    session.setMaximumLoadPoints(250);
    CHECK(session.maximumLoadPoints() == 250);
    const pci::SceneDocumentSnapshotPtr beforeBudgetChange =
        session.documentSnapshot();
    QSignalSpy documentChanged(&session, &pci::SceneSession::documentChanged);
    CHECK(session.setDecodedByteBudget(std::uint64_t{2} * 1024 * 1024,
                                       std::nullopt));
    CHECK(session.decodedByteBudget() == std::uint64_t{2} * 1024 * 1024);
    CHECK_FALSE(session.automaticMemoryBudgetEnabled());
    CHECK(pci::SceneSessionTestAccess::runtimeDecodedByteBudget(session) ==
          std::uint64_t{2} * 1024 * 1024);
    REQUIRE(documentChanged.count() == 1);
    const auto update =
        documentChanged.back().at(0).value<pci::DocumentUpdate>();
    CHECK(update.snapshot == beforeBudgetChange);
    CHECK(update.runtimeBudget.decodedPointBytes ==
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
        return testDocument(session)->vectorLayerCount() == 1;
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
        return testDocument(session)->rasterLayerCount() == 1;
    }));
    CHECK(testDocument(session)->layerCount() == 0);
    CHECK(testDocument(session)->vectorLayerCount() == 0);

    // The first layer in an empty document frames the view.
    REQUIRE_FALSE(documentChanged.empty());
    const auto firstRasterUpdate =
        documentChanged.back().at(0).value<pci::DocumentUpdate>();
    REQUIRE(firstRasterUpdate.runtime);
    CHECK(firstRasterUpdate.runtime->rasterBindingCount() == 1);
    const auto firstRaster = firstRasterUpdate.snapshot->rasterLayers().front();
    CHECK(firstRasterUpdate.runtime->raster(firstRaster.descriptor.sourceId,
                                            firstRaster.bindingGeneration));
    CHECK(firstRasterUpdate.viewAdjustment ==
          pci::ViewAdjustment::FrameVisibleLayers);
    CHECK(firstRasterUpdate.rendererPolicy ==
          pci::RendererDocumentPolicy::Reconcile);

    // A second raster is additive and must not move the camera.
    static_cast<void>(session.startRasterImport({.sourcePath = "second.tif"}));
    REQUIRE(waitFor([&session] {
        return testDocument(session)->rasterLayerCount() == 2;
    }));
    const auto secondRasterUpdate =
        documentChanged.back().at(0).value<pci::DocumentUpdate>();
    CHECK(secondRasterUpdate.viewAdjustment == pci::ViewAdjustment::Preserve);
    CHECK(secondRasterUpdate.rendererPolicy ==
          pci::RendererDocumentPolicy::Reconcile);
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
        return testDocument(session)->rasterLayerCount() == 1;
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
    CHECK(testDocument(session)->rasterLayerCount() == 0);
    CHECK_FALSE(session.hasActiveRasterLoads());
}

TEST_CASE("scene session commits and reverts raster point colors",
          "[scene-session][raster][colorize]")
{
    auto pointLoader = std::make_shared<SessionLoader>();
    auto rasterLoader = std::make_shared<SessionRasterLoader>();
    pci::SceneSession session(makeServices(pointLoader, rasterLoader),
                              100,
                              32ULL * 1024 * 1024,
                              std::nullopt,
                              {},
                              pci::test::createTestPointColorMapCatalog());
    session.loadPointCloud("target.laz", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);
    const auto point = testDocument(session)->layers().front();
    const pci::PointCloudLayerId pointId = point.id;
    const auto scene =
        pci::SceneSessionTestAccess::pointRuntime(session, pointId);
    REQUIRE(scene);
    const std::uint32_t sourceColor =
        scene->blockEntries().front().block->points.front().rgba;

    const pci::LoadJobId rasterImport =
        session.startRasterImport({.sourcePath = "colors.tif"});
    REQUIRE(waitFor([&session] {
        return testDocument(session)->rasterLayerCount() == 1;
    }));
    const auto raster = testDocument(session)->rasterLayers().front();
    const pci::SceneLayerId rasterId = raster.id;
    const auto rasterSource = std::dynamic_pointer_cast<const StubRasterSource>(
        pci::SceneSessionTestAccess::rasterSource(session, rasterId));
    REQUIRE(rasterSource);

    for (const pci::PointCloudLoadJobState &state :
         session.pointLoadJobStates()) {
        session.dismissJob(
            {.kind = pci::LoadJobKind::PointCloud, .id = state.jobId});
    }
    session.dismissJob({.kind = pci::LoadJobKind::Raster, .id = rasterImport});

    QSignalSpy status(&session, &pci::SceneSession::statusChanged);
    pci::LoadJobRows latestRows;
    QObject::connect(&session,
                     &pci::SceneSession::taskRowsChanged,
                     [&latestRows](pci::LoadJobRows rows) {
                         latestRows = std::move(rows);
                     });

    const pci::LoadJobId job =
        session.colorizePointCloudFromRaster(pointId, rasterId);
    REQUIRE(waitFor([&] {
        const auto layer = testDocument(session)->layer(pointId);
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
    const auto colored = testDocument(session)->layer(pointId);
    REQUIRE(colored.has_value());
    REQUIRE(colored->rasterColors.has_value());
    CHECK(colored->rasterColors->rasterLayerId == rasterId);
    CHECK(colored->rasterColors->rasterSourceId == raster.descriptor.sourceId);
    CHECK(colored->colorGeneration == 1);
    CHECK(colored->colorMode.source == pci::PointColorSource::Rgb);
    CHECK(scene->blockEntries().front().block->points.front().rgba ==
          0xff332211U);
    CHECK(rasterSource->reads.load() == 1);

    const auto rows = session.colorizeMetrics();
    CHECK(rows.activeColorTableBytes == 0);
    CHECK(session.revertPointCloudColors(pointId));
    CHECK_FALSE(
        testDocument(session)->layer(pointId)->rasterColors.has_value());
    CHECK(testDocument(session)->layer(pointId)->colorGeneration == 2);
    CHECK(scene->blockEntries().front().block->points.front().rgba ==
          sourceColor);
    CHECK(status.back().at(0).toString() ==
          QStringLiteral("Restored source colors for target.laz."));
    CHECK_FALSE(session.revertPointCloudColors(pointId));

    session.dismissJob({.kind = pci::LoadJobKind::Colorize, .id = job});
}

TEST_CASE("failed point admission preserves the installed document and runtime",
          "[scene-session][point-import][transaction]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader), 100, 1024 * 1024);
    session.loadPointCloud("original.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);
    const auto original = session.documentSnapshot();
    const auto id = layerForSource(session, "original.las");
    const auto runtime = pci::SceneSessionTestAccess::pointRuntime(session, id);
    const auto revision = runtime->revision();
    const auto blocks = runtime->blocks();
    QSignalSpy failures(&session, &pci::SceneSession::failureOccurred);

    SECTION("add")
    {
        pci::SceneSessionTestAccess::failNextPointCommit(session);
        session.loadPointCloud("incoming.las", pci::PointCloudLoadMode::Add);
    }
    SECTION("replacement")
    {
        pci::SceneSessionTestAccess::failNextPointCommit(session);
        session.loadPointCloud("incoming.las",
                               pci::PointCloudLoadMode::Replace);
    }
    REQUIRE(waitFor([&] {
        return failures.count() == 1;
    }));
    CHECK(session.documentSnapshot() == original);
    CHECK(pci::SceneSessionTestAccess::pointRuntime(session, id) == runtime);
    CHECK(runtime->revision() == revision);
    CHECK(runtime->blocks() == blocks);
    CHECK(session.pointLoadJobStates().back().phase ==
          pci::PointCloudLoadJobPhase::Failed);
}

TEST_CASE("point block and document publication commit together despite "
          "observer failure",
          "[scene-session][point-import][transaction]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader), 100, 1024 * 1024);
    session.loadPointCloud("original.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);
    const auto original = session.documentSnapshot();
    const auto id = layerForSource(session, "original.las");
    const auto runtime = pci::SceneSessionTestAccess::pointRuntime(session, id);
    const auto revision = runtime->revision();
    auto block = std::make_shared<pci::PointBlock>();
    block->points.resize(2);

    SECTION("failure before commit preserves both states")
    {
        pci::SceneSessionTestAccess::failNextPointCommit(session);
        CHECK_THROWS_AS(
            pci::SceneSessionTestAccess::publishBlock(session, id, block),
            std::bad_alloc);
        CHECK(runtime->totalPointCount() == 1);
        CHECK(runtime->revision() == revision);
        CHECK(session.documentSnapshot() == original);
    }
    SECTION("observer failure follows an intact commit")
    {
        bool observed = false;
        QObject::connect(
            &session,
            &pci::SceneSession::documentChanged,
            &session,
            [&](const pci::DocumentUpdate &update) {
                observed = true;
                CHECK(runtime->totalPointCount() == 3);
                CHECK(update.snapshot == session.documentSnapshot());
                throw std::runtime_error("observer failure after commit");
            });
        CHECK_NOTHROW(
            pci::SceneSessionTestAccess::publishBlock(session, id, block));
        CHECK(observed);
        CHECK(runtime->blocks().back() == block);
        CHECK(session.documentSnapshot() != original);
    }
}

TEST_CASE("color publication failures preserve document and runtime together",
          "[scene-session][colorize][transaction]")
{
    pci::SceneSession session(
        makeServices(std::make_shared<SessionLoader>(),
                     std::make_shared<SessionRasterLoader>()),
        100,
        32ULL * 1024 * 1024,
        std::nullopt,
        {},
        pci::test::createTestPointColorMapCatalog());
    session.loadPointCloud("target.laz", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);
    const auto pointId = testDocument(session)->layers().front().id;
    static_cast<void>(session.startRasterImport({.sourcePath = "colors.tif"}));
    REQUIRE(waitFor([&] {
        return testDocument(session)->rasterLayerCount() == 1;
    }));
    const auto rasterId = testDocument(session)->rasterLayers().front().id;
    const auto runtime =
        pci::SceneSessionTestAccess::pointRuntime(session, pointId);

    SECTION("apply admission failure")
    {
        const auto before = session.documentSnapshot();
        const auto blocks = runtime->blocks();
        pci::SceneSessionTestAccess::failNextPointCommit(session);
        static_cast<void>(
            session.colorizePointCloudFromRaster(pointId, rasterId));
        REQUIRE(waitFor([&] {
            return !session.hasActiveColorizeJobs();
        }));
        CHECK(session.documentSnapshot() == before);
        CHECK(runtime->blocks() == blocks);
        CHECK_FALSE(runtime->hasRasterPointColors());
    }
    SECTION("revert admission failure")
    {
        static_cast<void>(
            session.colorizePointCloudFromRaster(pointId, rasterId));
        REQUIRE(waitFor([&] {
            return !session.hasActiveColorizeJobs();
        }));
        REQUIRE(runtime->hasRasterPointColors());
        const auto before = session.documentSnapshot();
        const auto blocks = runtime->blocks();
        pci::SceneSessionTestAccess::failNextPointCommit(session);
        CHECK_THROWS_AS(session.revertPointCloudColors(pointId),
                        std::bad_alloc);
        CHECK(session.documentSnapshot() == before);
        CHECK(runtime->blocks() == blocks);
        CHECK(runtime->hasRasterPointColors());
    }
    SECTION("observer failure cannot undo a committed bake or revert")
    {
        std::size_t observed = 0;
        QObject::connect(&session,
                         &pci::SceneSession::documentChanged,
                         &session,
                         [&](const pci::DocumentUpdate &update) {
                             ++observed;
                             CHECK(update.snapshot ==
                                   session.documentSnapshot());
                             CHECK(testDocument(session)
                                       ->layer(pointId)
                                       ->rasterColors.has_value() ==
                                   runtime->hasRasterPointColors());
                             throw std::runtime_error("color observer failure");
                         });
        static_cast<void>(
            session.colorizePointCloudFromRaster(pointId, rasterId));
        REQUIRE(waitFor([&] {
            return !session.hasActiveColorizeJobs();
        }));
        REQUIRE(runtime->hasRasterPointColors());
        CHECK(session.revertPointCloudColors(pointId));
        CHECK_FALSE(runtime->hasRasterPointColors());
        CHECK(observed == 2);
    }
}

TEST_CASE(
    "failed removal and reset preserve active imports and the installed scene",
    "[scene-session][transaction][cancellation]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader), 100, 1024 * 1024);
    session.loadPointCloud("cancel-after-preview.las",
                           pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&] {
        return !testDocument(session)->layers().empty();
    }));
    const auto before = session.documentSnapshot();
    const auto generation = session.sessionGeneration();
    const auto id = testDocument(session)->layers().front().id;
    const auto runtime = pci::SceneSessionTestAccess::pointRuntime(session, id);
    pci::SceneSessionTestAccess::failNextPointCommit(session);
    SECTION("remove")
    {
        CHECK_THROWS_AS(session.removeLayer(id), std::bad_alloc);
    }
    SECTION("New Scene")
    {
        CHECK_THROWS_AS(session.newScene(), std::bad_alloc);
    }
    CHECK(session.documentSnapshot() == before);
    CHECK(session.sessionGeneration() == generation);
    CHECK(pci::SceneSessionTestAccess::pointRuntime(session, id) == runtime);
    CHECK(session.loading());
    loader->releaseSlow = true;
    finishVisibleLoads(session);
    CHECK_FALSE(session.loading());
}

TEST_CASE(
    "New Scene retires an admitted replacement without restoring old layers",
    "[scene-session][transaction][cancellation]")
{
    auto loader = std::make_shared<SessionLoader>();
    pci::SceneSession session(makeServices(loader), 100, 1024 * 1024);
    session.loadPointCloud("original.las", pci::PointCloudLoadMode::Replace);
    finishVisibleLoads(session);
    session.loadPointCloud("cancel-after-preview.las",
                           pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&] {
        return loader->previewPublished &&
               testDocument(session)
                       ->layers()
                       .front()
                       .descriptor.metadata.sourcePath.filename() ==
                   "cancel-after-preview.las";
    }));
    const auto generation = session.sessionGeneration();
    session.newScene();
    REQUIRE(waitFor([&] {
        const auto jobs = session.pointLoadJobStates();
        return !jobs.empty() &&
               jobs.back().phase == pci::PointCloudLoadJobPhase::Cancelled;
    }));
    CHECK(session.sessionGeneration() != generation);
    CHECK_FALSE(testDocument(session)->hasAnyLayer());
    CHECK_FALSE(session.loading());
}

TEST_CASE(
    "overlay admission failure does not publish success or change the document",
    "[scene-session][transaction][regression]")
{
    auto points = std::make_shared<SessionLoader>();
    auto services = makeServices(points);
    services.raster = std::make_unique<pci::RasterLoadController>(
        std::make_shared<SessionRasterLoader>(), *services.scheduler);
    services.vector = std::make_unique<pci::VectorLoadController>(
        std::make_shared<PartialVectorLoader>(), *services.scheduler);
    pci::SceneSession session(std::move(services), 100, 1024 * 1024);
    const auto original = session.documentSnapshot();
    pci::LoadJobRows rows;
    QObject::connect(&session,
                     &pci::SceneSession::taskRowsChanged,
                     &session,
                     [&](pci::LoadJobRows update) {
                         rows = std::move(update);
                     });
    pci::SceneSessionTestAccess::failNextPointCommit(session);
    SECTION("raster")
    {
        static_cast<void>(
            session.startRasterImport({.sourcePath = "ortho.tif"}));
    }
    SECTION("vector")
    {
        pci::VectorImportRequest request;
        request.sourcePath = "survey.gpkg";
        request.sublayers = {{.index = 0, .name = "good"}};
        static_cast<void>(session.loadVectorLayers(std::move(request)));
    }
    REQUIRE(waitFor([&] {
        return rows.size() == 1 && rows.front().terminal;
    }));
    CHECK(rows.front().capabilities.canRetry);
    CHECK(session.documentSnapshot() == original);
    CHECK_FALSE(testDocument(session)->hasAnyLayer());
}
