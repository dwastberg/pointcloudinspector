#include "app/ColorizeFromRasterDialog.h"
#include "app/LayerInspectorDock.h"
#include "app/MainWindow.h"
#include "app/SceneLayersDock.h"
#include "app/SettingsDialog.h"
#include "app/TaskDock.h"

#include "import/PointCloudLoadController.h"
#include "platform/QtPath.h"
#include "pointcloud/PointColorPolicy.h"
#include "renderer/RenderViewport.h"
#include "scene/PointCloudDataSource.h"
#include "support/TestPointColorMaps.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#endif
#include <QFile>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

template <typename Predicate> bool waitFor(Predicate &&predicate)
{
    return QTest::qWaitFor(std::forward<Predicate>(predicate), 2000);
}

pci::SceneDocumentSnapshotPtr
makeSnapshot(const std::vector<pci::PointCloudLayer> &points,
             const std::vector<pci::VectorLayer> &vectors,
             const std::vector<pci::SceneLayerId> &order,
             const std::vector<pci::RasterLayer> &rasters = {})
{
    auto snapshot = std::make_shared<pci::SceneDocumentSnapshot>();
    for (const pci::SceneLayerId id : order) {
        const auto point =
            std::ranges::find(points, id, &pci::PointCloudLayer::id);
        if (point != points.end()) {
            snapshot->layers.push_back({
                .id = id,
                .visible = point->visible,
                .payload =
                    pci::PointCloudLayerState{
                        .scene = point->scene,
                        .colorMode = point->colorMode,
                        .classificationFilter = point->classificationFilter,
                        .rasterColors = point->rasterColors,
                        .colorGeneration = point->colorGeneration,
                    },
            });
            continue;
        }
        const auto vector =
            std::ranges::find(vectors, id, &pci::VectorLayer::id);
        if (vector != vectors.end()) {
            snapshot->layers.push_back({
                .id = id,
                .visible = vector->visible,
                .payload =
                    pci::VectorLayerState{
                        .data = vector->data,
                        .style = vector->style,
                    },
            });
            continue;
        }
        const auto raster =
            std::ranges::find(rasters, id, &pci::RasterLayer::id);
        REQUIRE(raster != rasters.end());
        snapshot->layers.push_back({
            .id = id,
            .visible = raster->visible,
            .payload =
                pci::RasterLayerState{
                    .data = raster->data,
                    .style = raster->style,
                    .renderGeneration = raster->renderGeneration,
                },
        });
    }
    return snapshot;
}

class ImmediateLoader final : public pci::PointCloudLoader {
public:
    pci::PointCloudScenePtr
    load(const pci::PointCloudLoadOptions &options,
         const pci::PointCloudLoadResources &,
         const pci::PointCloudImportPreflight &,
         const pci::PointCloudLoadContext &context) const override
    {
        if (context.progress) {
            context.progress({
                .stage = pci::PointCloudImportStage::Reading,
                .processed = 0,
                .total = 3,
            });
        }
        pci::PointCloudMetadata metadata;
        metadata.sourcePath = options.sourcePath;
        metadata.sourcePointCount = 3;
        if (options.sourcePath.filename() == "range-first.las") {
            metadata.sourceBounds = {
                {10.0, 20.0, 30.0},
                {20.0, 40.0, 60.0},
            };
        } else if (options.sourcePath.filename() == "range-second.las") {
            metadata.sourceBounds = {
                {-5.0, 25.0, 15.0},
                {100.0, 35.0, 90.0},
            };
        }
        const bool scalarOnly =
            options.sourcePath.filename() == "scalar-only.las";
        metadata.hasColor = !scalarOnly;
        metadata.hasIntensity = !scalarOnly;
        metadata.hasClassification = !scalarOnly;
        auto scene = std::make_shared<pci::PointCloudScene>(metadata);
        if (context.sceneReady) {
            context.sceneReady(scene);
        }
        auto block = std::make_shared<pci::PointBlock>();
        block->points.resize(3);
        block->attributes.resize(3);
        if (metadata.hasClassification) {
            const std::array<std::uint8_t, 3> classifications =
                options.sourcePath.filename() == "classified-second.las"
                    ? std::array<std::uint8_t, 3>{5, 6, 17}
                    : std::array<std::uint8_t, 3>{2, 6, 9};
            for (std::size_t index = 0; index < classifications.size();
                 ++index) {
                block->points[index].attributes = classifications[index];
                block->attributes[index].classification =
                    classifications[index];
            }
        }
        scene->addBlock(std::move(block));
        if (context.progress) {
            context.progress({
                .stage = pci::PointCloudImportStage::Reading,
                .processed = 3,
                .total = 3,
            });
        }
        return scene;
    }
};

class CachedHierarchySource final : public pci::PointCloudDataSource {
public:
    [[nodiscard]] pci::PointCloudNode rootNode() const override
    {
        return node(pci::rootPointCloudNode);
    }

    [[nodiscard]] pci::PointCloudNode
    node(const pci::PointCloudNodeId id) const override
    {
        return {
            .id = id,
            .bounds = {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}},
            .estimatedPointCount = 10,
            .leaf = true,
        };
    }

    [[nodiscard]] pci::PointCloudNodePayloadPtr
    loadNode(const pci::PointCloudNodeId, const std::stop_token) const override
    {
        throw std::logic_error("cached hierarchy fixture has only its root");
    }

    [[nodiscard]] pci::PointCloudStorageMetrics storageMetrics() const override
    {
        return {.localPersistent = true, .committed = true, .reused = true};
    }
};

class CachedPagedLoader final : public pci::PointCloudLoader {
public:
    [[nodiscard]] pci::PointCloudImportPreflight
    inspect(const pci::PointCloudLoadOptions &options,
            std::uint64_t,
            std::stop_token) const override
    {
        pci::PointCloudMetadata metadata;
        metadata.sourcePath = options.sourcePath;
        metadata.sourceBounds = {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};
        metadata.sourcePointCount = 10;
        return {
            .metadata = std::move(metadata),
            .estimatedActiveBytes = 1,
            .hierarchical = true,
            .localPaging = true,
        };
    }

    pci::PointCloudScenePtr
    load(const pci::PointCloudLoadOptions &options,
         const pci::PointCloudLoadResources &resources,
         const pci::PointCloudImportPreflight &,
         const pci::PointCloudLoadContext &context) const override
    {
        pci::PointCloudMetadata metadata;
        metadata.sourcePath = options.sourcePath;
        metadata.sourceBounds = {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};
        metadata.sourcePointCount = 10;
        auto block = std::make_shared<pci::PointBlock>();
        block->points.resize(1);
        block->attributes.resize(1);
        auto root = std::make_shared<pci::PointCloudNodePayload>();
        root->nodeId = pci::rootPointCloudNode;
        root->sourcePointCount = 10;
        root->blocks.push_back(std::move(block));
        auto scene = std::make_shared<pci::PointCloudScene>(
            metadata,
            std::make_shared<CachedHierarchySource>(),
            std::move(root),
            resources.decodedByteBudget);
        if (context.sceneReady) {
            context.sceneReady(scene);
        }
        return scene;
    }
};

class SlowLoader final : public pci::PointCloudLoader {
public:
    mutable std::atomic<bool> started = false;
    mutable std::atomic<bool> stopped = false;

    pci::PointCloudScenePtr
    load(const pci::PointCloudLoadOptions &,
         const pci::PointCloudLoadResources &,
         const pci::PointCloudImportPreflight &,
         const pci::PointCloudLoadContext &context) const override
    {
        started = true;
        if (context.progress) {
            context.progress({
                .stage = pci::PointCloudImportStage::Reading,
                .processed = 5,
                .total = 10,
            });
        }
        while (!context.stopToken.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stopped = true;
        throw pci::PointCloudImportCancelled();
    }
};

class FirstImmediateThenGatedLoader final : public pci::PointCloudLoader {
public:
    mutable std::atomic<int> gatedLoads = 0;
    mutable std::atomic<bool> allowGatedCompletion = false;

    pci::PointCloudScenePtr
    load(const pci::PointCloudLoadOptions &options,
         const pci::PointCloudLoadResources &,
         const pci::PointCloudImportPreflight &,
         const pci::PointCloudLoadContext &context) const override
    {
        const bool gated = options.sourcePath.filename() == "pending.las";
        pci::PointCloudMetadata metadata;
        metadata.sourcePath = options.sourcePath;
        metadata.sourcePointCount = 3;
        metadata.hasColor = true;
        auto scene = std::make_shared<pci::PointCloudScene>(metadata);
        if (context.sceneReady) {
            context.sceneReady(scene);
        }
        if (gated) {
            ++gatedLoads;
            while (!allowGatedCompletion.load() &&
                   !context.stopToken.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (context.stopToken.stop_requested()) {
                throw pci::PointCloudImportCancelled();
            }
        }
        auto block = std::make_shared<pci::PointBlock>();
        block->points.resize(3);
        block->attributes.resize(3);
        scene->addBlock(std::move(block));
        return scene;
    }
};

class FailingLoader final : public pci::PointCloudLoader {
public:
    pci::PointCloudScenePtr
    load(const pci::PointCloudLoadOptions &,
         const pci::PointCloudLoadResources &,
         const pci::PointCloudImportPreflight &,
         const pci::PointCloudLoadContext &) const override
    {
        throw pci::PointCloudImportError("fixture failure");
    }
};

class FirstImmediateThenPreviewFailureLoader final
    : public pci::PointCloudLoader {
public:
    mutable std::atomic_bool previewPublished = false;
    mutable std::atomic_bool releaseFailure = false;

    pci::PointCloudScenePtr
    load(const pci::PointCloudLoadOptions &options,
         const pci::PointCloudLoadResources &,
         const pci::PointCloudImportPreflight &,
         const pci::PointCloudLoadContext &context) const override
    {
        pci::PointCloudMetadata metadata;
        metadata.sourcePath = options.sourcePath;
        metadata.sourcePointCount = 1;
        auto scene = std::make_shared<pci::PointCloudScene>(metadata);
        if (context.sceneReady) {
            context.sceneReady(scene);
        }
        if (options.sourcePath.filename() == "broken-after-preview.las") {
            previewPublished = true;
            while (!releaseFailure.load() &&
                   !context.stopToken.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (context.stopToken.stop_requested()) {
                throw pci::PointCloudImportCancelled();
            }
            throw pci::PointCloudImportError("failure after preview");
        }
        auto block = std::make_shared<pci::PointBlock>();
        block->points.resize(1);
        block->attributes.resize(1);
        scene->addBlock(std::move(block));
        return scene;
    }
};

class DisjointVectorLoader final : public pci::VectorLoader {
public:
    [[nodiscard]] pci::VectorImportPreflight
    inspect(const pci::VectorImportRequest &request) const override
    {
        pci::VectorImportPreflight result;
        result.sourcePath = request.sourcePath;
        if (request.sublayers.empty()) {
            result.sublayers.push_back(
                {.key = {.index = 0, .name = "default"}});
            return result;
        }
        for (const pci::VectorSublayerKey &key : request.sublayers) {
            result.sublayers.push_back({.key = key});
        }
        return result;
    }
    [[nodiscard]] std::array<double, 2>
    probeOrigin(const pci::VectorImportRequest &,
                std::span<const pci::VectorSublayerKey>) const override
    {
        return {0.0, 0.0};
    }
    [[nodiscard]] pci::VectorLayerDataPtr
    loadSublayer(const pci::VectorImportRequest &,
                 pci::VectorSublayerKey) const override
    {
        auto data = std::make_shared<pci::VectorLayerData>();
        data->sublayerName = "Remote survey";
        data->extentDisjointXY = true;
        data->bounds = {.minimum = {1000.0, 1000.0, 0.0},
                        .maximum = {1001.0, 1001.0, 0.0}};
        return data;
    }
};

class UnavailableStatistics final : public pci::PointCloudStatisticsProvider {
public:
    [[nodiscard]] pci::PointCloudStatistics
    calculate(const pci::PointCloudMetadata &,
              std::stop_token,
              Progress) const override
    {
        throw std::runtime_error("statistics unavailable in UI fixture");
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

    [[nodiscard]] pci::RasterTileData readTile(const pci::RasterTileRequest &,
                                               std::stop_token) const override
    {
        throw pci::RasterReadError("the UI fixture holds no pixels");
    }

private:
    pci::RasterLayerMetadata metadata_;
};

[[nodiscard]] pci::RasterLayerDataPtr
makeUiRasterData(const std::filesystem::path &sourcePath)
{
    pci::RasterLayerMetadata metadata;
    metadata.sourcePath = sourcePath;
    metadata.sourceDriver = "GTiff";
    metadata.width = 64;
    metadata.height = 32;
    metadata.geoTransform = {0.0, 1.0, 0.0, 32.0, 0.0, -1.0};
    metadata.spatialReferenceWkt = "STUBCRS";
    metadata.bounds = *pci::rasterPixelEdgeBounds(
        metadata.geoTransform, metadata.width, metadata.height);
    metadata.bands = {
        {.band = 1, .name = "Red"},
        {.band = 2, .name = "Green"},
        {.band = 3, .name = "Blue"},
    };
    pci::RasterLevel level;
    level.width = metadata.width;
    level.height = metadata.height;
    level.channelCount = 3;
    metadata.levels.push_back(level);
    return std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = std::make_shared<StubRasterSource>(std::move(metadata)),
    });
}

[[nodiscard]] pci::PointCloudScenePtr
makeUiColorizableScene(const std::filesystem::path &sourcePath)
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePath = sourcePath;
    metadata.sourcePointCount = 1;
    metadata.sourceBounds = {{8.0, 8.0, 0.0}, {8.0, 8.0, 0.0}};
    metadata.spatialReferenceWkt = "STUBCRS";
    metadata.hasColor = true;
    auto scene = std::make_shared<pci::PointCloudScene>(std::move(metadata));
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = {8.0, 8.0, 0.0};
    block->bounds = {{8.0, 8.0, 0.0}, {8.0, 8.0, 0.0}};
    block->points.push_back({
        .x = 0,
        .y = 0,
        .z = 0,
        .attributes = 0,
        .rgba = 0x112233ffU,
        .packedProperties = 0,
    });
    block->attributes.emplace_back();
    scene->addBlock(std::move(block));
    scene->markLoadingComplete();
    return scene;
}

// Produces a small, correctly georeferenced raster so UI rows and inspector
// fields can be exercised without a GDAL dependency in the widget tests.
class StubRasterLoader final : public pci::RasterLoader {
public:
    [[nodiscard]] pci::RasterImportPreflight
    inspect(const pci::RasterImportRequest &request) const override
    {
        ++inspectCalls;
        lastSourcePath = request.sourcePath;
        pci::RasterLayerMetadata metadata;
        metadata.sourcePath = request.sourcePath;
        metadata.sourceDriver = "GTiff";
        metadata.width = 64;
        metadata.height = 32;
        metadata.geoTransform = {0.0, 1.0, 0.0, 32.0, 0.0, -1.0};
        metadata.spatialReferenceWkt = "STUBCRS";
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

    mutable std::atomic_int inspectCalls{0};
    mutable std::filesystem::path lastSourcePath;
};

pci::ImportServices
makeTestImportServices(std::shared_ptr<const pci::PointCloudLoader> pointLoader,
                       std::shared_ptr<const pci::VectorLoader> vectorLoader =
                           std::make_shared<DisjointVectorLoader>(),
                       std::unique_ptr<pci::TaskScheduler> scheduler = {},
                       std::shared_ptr<const pci::RasterLoader> rasterLoader =
                           std::make_shared<StubRasterLoader>())
{
    if (!scheduler) {
        scheduler = std::make_unique<pci::TaskScheduler>();
    }
    pci::ImportServices services;
    services.scheduler = std::move(scheduler);
    services.pointCloud = std::make_unique<pci::PointCloudLoadController>(
        std::move(pointLoader), *services.scheduler);
    services.vector = std::make_unique<pci::VectorLoadController>(
        std::move(vectorLoader), *services.scheduler);
    services.raster = std::make_unique<pci::RasterLoadController>(
        std::move(rasterLoader), *services.scheduler);
    services.rasterElevation =
        std::make_unique<pci::RasterElevationController>(*services.scheduler);
    services.colorize = std::make_unique<pci::PointCloudColorizeController>(
        *services.scheduler);
    services.statistics = std::make_shared<UnavailableStatistics>();
    return services;
}

class OutOfOrderPreviewLoader final : public pci::PointCloudLoader {
public:
    mutable std::atomic_bool slowStarted = false;
    mutable std::atomic_bool releaseSlow = false;

    pci::PointCloudScenePtr
    load(const pci::PointCloudLoadOptions &options,
         const pci::PointCloudLoadResources &,
         const pci::PointCloudImportPreflight &,
         const pci::PointCloudLoadContext &context) const override
    {
        if (options.sourcePath.filename() == "slow-first.las") {
            slowStarted = true;
            while (!releaseSlow.load() && !context.stopToken.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (context.stopToken.stop_requested()) {
                throw pci::PointCloudImportCancelled();
            }
        }
        pci::PointCloudMetadata metadata;
        metadata.sourcePath = options.sourcePath;
        metadata.sourcePointCount = 1;
        auto scene = std::make_shared<pci::PointCloudScene>(metadata);
        auto block = std::make_shared<pci::PointBlock>();
        block->points.resize(1);
        block->attributes.resize(1);
        scene->addBlock(std::move(block));
        if (context.sceneReady) {
            context.sceneReady(scene);
        }
        return scene;
    }
};

class FakeViewport final : public pci::RenderViewport {
public:
    QWidget *widget() noexcept override
    {
        return &widget_;
    }

    QString backendName() const override
    {
        return QStringLiteral("Fake");
    }

    std::uint64_t totalPointCount() const noexcept override
    {
        return document_ ? document_->visibleExpectedPointCount : 0;
    }

    void setDocument(pci::SceneDocumentSnapshotPtr document,
                     const bool frameVisibleLayers) override
    {
        document_ = std::move(document);
        ++documentSetCount_;
        lastDocumentWasFramed_ = frameVisibleLayers;
    }

    void updateDocument(pci::SceneDocumentSnapshotPtr document) override
    {
        document_ = std::move(document);
        ++documentUpdateCount_;
        requestRender();
    }

    void requestRender() override
    {
        ++renderRequestCount_;
    }

    void frameVisibleLayers() override
    {
        ++frameVisibleLayersCount_;
    }

    void frameVisibleLayersTopDown() override
    {
        ++frameVisibleLayersTopDownCount_;
    }

    bool isOrthographic() const noexcept override
    {
        return orthographic_;
    }

    void setOrthographic(const bool enabled) override
    {
        orthographic_ = enabled;
    }

    void frameLayer(pci::PointCloudLayerId layerId) override
    {
        lastFramedLayerId_ = layerId;
    }

    bool eyeDomeLightingEnabled() const noexcept override
    {
        return viewportSettings_.depthEnhancement.enabled;
    }

    void setEyeDomeLightingEnabled(const bool enabled) override
    {
        viewportSettings_.depthEnhancement.enabled = enabled;
        ++eyeDomeLightingSetCount_;
    }

    pci::ViewportSettings viewportSettings() const noexcept override
    {
        return viewportSettings_;
    }

    void setViewportSettings(const pci::ViewportSettings &settings) override
    {
        if (viewportSettings_ == settings) {
            return;
        }
        viewportSettings_ = settings;
        ++viewportSettingsSetCount_;
    }

    std::uint64_t gpuByteBudget() const noexcept override
    {
        return gpuByteBudget_;
    }

    void setGpuByteBudget(const std::uint64_t byteBudget) override
    {
        if (gpuByteBudget_ == byteBudget) {
            return;
        }
        gpuByteBudget_ = byteBudget;
        ++gpuByteBudgetSetCount_;
    }

    void setRasterByteBudgets(const std::uint64_t cpuByteBudget,
                              const std::uint64_t gpuByteBudget) override
    {
        rasterCpuByteBudget_ = cpuByteBudget;
        rasterGpuByteBudget_ = gpuByteBudget;
    }

    std::uint64_t rasterCpuByteBudget_ = 0;
    std::uint64_t rasterGpuByteBudget_ = 0;

    int pointSizePixels() const noexcept override
    {
        return pointSizePixels_;
    }

    void setPointSizePixels(const int pointSize) override
    {
        pointSizePixels_ = pointSize;
        ++pointSizeSetCount_;
    }

    pci::ViewportTool activeTool() const noexcept override
    {
        return activeTool_;
    }

    void setActiveTool(const pci::ViewportTool tool) override
    {
        activeTool_ = tool;
    }

    void setMetricsCallback(MetricsCallback callback) override
    {
        metricsCallback_ = std::move(callback);
    }

    void setFailureCallback(FailureCallback callback) override
    {
        failureCallback_ = std::move(callback);
    }

    void setLoadProgressCallback(LoadProgressCallback callback) override
    {
        loadProgressCallback_ = std::move(callback);
    }

    [[nodiscard]] pci::VectorOverlayCapability
    vectorOverlayCapability() const noexcept override
    {
        return vectorCapability_;
    }
    void setVectorOverlayCapabilityCallback(
        VectorOverlayCapabilityCallback callback) override
    {
        vectorCapabilityCallback_ = std::move(callback);
    }
    void
    setVectorOverlayCapability(const pci::VectorOverlayCapability capability)
    {
        vectorCapability_ = capability;
        if (vectorCapabilityCallback_) {
            vectorCapabilityCallback_(capability, {});
        }
    }

    void emitLoadProgress(const pci::RenderLoadProgress &progress)
    {
        if (loadProgressCallback_) {
            loadProgressCallback_(progress);
        }
    }

    void emitDisplayReady(const pci::PointCloudLayerId layerId,
                          const std::uint64_t points = 3)
    {
        emitLoadProgress({
            .layerId = layerId,
            .stage = pci::RenderLoadStage::FirstFrameReady,
            .completed = points,
            .total = points,
        });
        emitLoadProgress({
            .layerId = layerId,
            .stage = pci::RenderLoadStage::DisplayReady,
            .completed = points,
            .total = points,
        });
    }

    void emitMetrics(const pci::RenderMetrics &metrics)
    {
        if (metricsCallback_) {
            metricsCallback_(metrics);
        }
    }

    [[nodiscard]] pci::SceneDocumentSnapshotPtr document() const
    {
        return document_;
    }

    [[nodiscard]] int documentSetCount() const noexcept
    {
        return documentSetCount_;
    }

    [[nodiscard]] int renderRequestCount() const noexcept
    {
        return renderRequestCount_;
    }

    [[nodiscard]] int eyeDomeLightingSetCount() const noexcept
    {
        return eyeDomeLightingSetCount_;
    }

    [[nodiscard]] int pointSizeSetCount() const noexcept
    {
        return pointSizeSetCount_;
    }

    [[nodiscard]] int viewportSettingsSetCount() const noexcept
    {
        return viewportSettingsSetCount_;
    }

    [[nodiscard]] bool lastDocumentWasFramed() const noexcept
    {
        return lastDocumentWasFramed_;
    }

    [[nodiscard]] std::optional<pci::PointCloudLayerId>
    lastFramedLayerId() const noexcept
    {
        return lastFramedLayerId_;
    }

    [[nodiscard]] int frameVisibleLayersCount() const noexcept
    {
        return frameVisibleLayersCount_;
    }

    [[nodiscard]] int frameVisibleLayersTopDownCount() const noexcept
    {
        return frameVisibleLayersTopDownCount_;
    }

private:
    QWidget widget_;
    pci::SceneDocumentSnapshotPtr document_;
    int documentSetCount_ = 0;
    int documentUpdateCount_ = 0;
    int renderRequestCount_ = 0;
    int eyeDomeLightingSetCount_ = 0;
    int pointSizeSetCount_ = 0;
    int viewportSettingsSetCount_ = 0;
    int gpuByteBudgetSetCount_ = 0;
    int frameVisibleLayersCount_ = 0;
    int frameVisibleLayersTopDownCount_ = 0;
    bool orthographic_ = false;
    bool lastDocumentWasFramed_ = false;
    pci::ViewportSettings viewportSettings_;
    std::uint64_t gpuByteBudget_ = std::uint64_t{512} * 1024 * 1024;
    int pointSizePixels_ = pci::defaultPointSizePixels;
    pci::ViewportTool activeTool_ = pci::ViewportTool::Navigate;
    std::optional<pci::PointCloudLayerId> lastFramedLayerId_;
    MetricsCallback metricsCallback_;
    FailureCallback failureCallback_;
    LoadProgressCallback loadProgressCallback_;
    VectorOverlayCapabilityCallback vectorCapabilityCallback_;
    pci::VectorOverlayCapability vectorCapability_ =
        pci::VectorOverlayCapability::Unknown;
};

TEST_CASE("fit scene action frames the visible scene",
          "[ui][mainwindow][viewport]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    CHECK(window.windowTitle() == QStringLiteral("Point Cloud Inspector"));

    auto *fit = window.findChild<QAction *>(QStringLiteral("fitSceneAction"));
    REQUIRE(fit != nullptr);
    window.loadPointCloud("fit.las");
    REQUIRE(QTest::qWaitFor(
        [&] {
            return viewportPointer->document() &&
                   viewportPointer->document()->layerCount() == 1;
        },
        2000));
    fit->trigger();
    CHECK(viewportPointer->frameVisibleLayersCount() == 1);
}

TEST_CASE("top-down scene action frames the visible scene from above",
          "[ui][mainwindow][viewport]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);

    auto *topDown =
        window.findChild<QAction *>(QStringLiteral("topDownSceneAction"));
    REQUIRE(topDown != nullptr);
    window.loadPointCloud("top-down.las");
    REQUIRE(QTest::qWaitFor(
        [&] {
            return viewportPointer->document() &&
                   viewportPointer->document()->layerCount() == 1;
        },
        2000));
    topDown->trigger();
    CHECK(viewportPointer->frameVisibleLayersTopDownCount() == 1);
}

TEST_CASE("orthographic camera action stays synchronized with the viewport",
          "[ui][mainwindow][viewport]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);

    auto *orthographic =
        window.findChild<QAction *>(QStringLiteral("orthographicCameraAction"));
    REQUIRE(orthographic != nullptr);
    CHECK_FALSE(orthographic->isChecked());
    orthographic->setChecked(true);
    CHECK(viewportPointer->isOrthographic());
    orthographic->setChecked(false);
    CHECK_FALSE(viewportPointer->isOrthographic());
}

TEST_CASE("main window shares its open action between menu and toolbar",
          "[ui][mainwindow]")
{
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(
        std::make_unique<FakeViewport>(), std::move(services), 100);

    QAction *openAction =
        window.findChild<QAction *>(QStringLiteral("openFilesAction"));
    QMenu *fileMenu = window.findChild<QMenu *>(QStringLiteral("fileMenu"));
    QToolBar *toolBar =
        window.findChild<QToolBar *>(QStringLiteral("pointCloudToolBar"));

    REQUIRE(openAction != nullptr);
    REQUIRE(fileMenu != nullptr);
    REQUIRE(toolBar != nullptr);
    CHECK(fileMenu->actions().contains(openAction));
    CHECK(toolBar->actions().contains(openAction));
    CHECK(openAction->shortcut() == QKeySequence::Open);
#ifndef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    CHECK(window.findChild<QWidget *>(
              QStringLiteral("pointCloudResidencyDiagnostics")) == nullptr);
    CHECK(window.findChild<QLabel *>(QStringLiteral("layerIndexValue")) ==
          nullptr);
    CHECK(window.statusBar()->currentMessage() == QStringLiteral("Ready"));
#endif
}

TEST_CASE("main toolbar follows data, camera, display, settings order",
          "[ui][mainwindow][toolbar]")
{
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(
        std::make_unique<FakeViewport>(), std::move(services), 100);

    auto *toolBar =
        window.findChild<QToolBar *>(QStringLiteral("pointCloudToolBar"));
    auto *fileMenu = window.findChild<QMenu *>(QStringLiteral("fileMenu"));
    auto *open = window.findChild<QAction *>(QStringLiteral("openFilesAction"));
    auto *add =
        window.findChild<QAction *>(QStringLiteral("addPointCloudAction"));
    auto *vector =
        window.findChild<QAction *>(QStringLiteral("importVectorLayerAction"));
    auto *raster =
        window.findChild<QAction *>(QStringLiteral("importRasterLayerAction"));
    auto *fit = window.findChild<QAction *>(QStringLiteral("fitSceneAction"));
    auto *topDown =
        window.findChild<QAction *>(QStringLiteral("topDownSceneAction"));
    auto *orthographic =
        window.findChild<QAction *>(QStringLiteral("orthographicCameraAction"));
    auto *settings =
        window.findChild<QAction *>(QStringLiteral("settingsAction"));
    auto *pointSize =
        window.findChild<QSpinBox *>(QStringLiteral("pointSizeSpinBox"));
    auto *depth = window.findChild<QCheckBox *>(
        QStringLiteral("eyeDomeLightingCheckBox"));
    auto *spacer =
        window.findChild<QWidget *>(QStringLiteral("commandToolbarSpacer"));
    REQUIRE(toolBar != nullptr);
    REQUIRE(fileMenu != nullptr);
    REQUIRE(open != nullptr);
    CHECK(add == nullptr);
    CHECK(vector == nullptr);
    CHECK(raster == nullptr);
    REQUIRE(fit != nullptr);
    REQUIRE(topDown != nullptr);
    REQUIRE(orthographic != nullptr);
    REQUIRE(settings != nullptr);
    REQUIRE(pointSize != nullptr);
    REQUIRE(depth != nullptr);
    REQUIRE(spacer != nullptr);

    const QList<QAction *> actions = toolBar->actions();
    const auto actionForWidget = [toolBar, &actions](QWidget *widget) {
        const auto found =
            std::ranges::find_if(actions, [toolBar, widget](QAction *action) {
                return toolBar->widgetForAction(action) == widget;
            });
        return found == actions.end() ? nullptr : *found;
    };
    const auto ordered = [&actions](QAction *left, QAction *right) {
        return actions.indexOf(left) < actions.indexOf(right);
    };

    QAction *pointSizeAction = actionForWidget(pointSize);
    QAction *depthAction = actionForWidget(depth);
    QAction *spacerAction = actionForWidget(spacer);
    REQUIRE(pointSizeAction != nullptr);
    REQUIRE(depthAction != nullptr);
    REQUIRE(spacerAction != nullptr);
    CHECK(ordered(open, fit));
    CHECK(ordered(fit, topDown));
    CHECK(ordered(topDown, orthographic));
    CHECK(ordered(orthographic, pointSizeAction));
    CHECK(ordered(pointSizeAction, depthAction));
    CHECK(ordered(depthAction, spacerAction));
    CHECK(ordered(spacerAction, settings));
    CHECK(spacer->sizePolicy().horizontalPolicy() == QSizePolicy::Expanding);
    CHECK(open->iconText() == QStringLiteral("Open…"));
    CHECK(fit->iconText() == QStringLiteral("Fit"));
    CHECK(topDown->iconText() == QStringLiteral("Top Down"));
    CHECK(orthographic->iconText() == QStringLiteral("Orthographic"));
    CHECK_FALSE(open->icon().isNull());
    CHECK_FALSE(fit->icon().isNull());
    CHECK_FALSE(topDown->icon().isNull());
    CHECK_FALSE(orthographic->icon().isNull());
    CHECK_FALSE(settings->icon().isNull());
    CHECK(toolBar->iconSize() == QSize(20, 20));
}

TEST_CASE("unified open remains available while vector capability is unknown",
          "[ui][mainwindow][vector]")
{
    pci::MainWindow window(
        std::make_unique<FakeViewport>(),
        makeTestImportServices(std::make_shared<ImmediateLoader>()),
        100);
    QAction *action =
        window.findChild<QAction *>(QStringLiteral("openFilesAction"));
    QToolBar *toolBar =
        window.findChild<QToolBar *>(QStringLiteral("pointCloudToolBar"));
    REQUIRE(action);
    REQUIRE(toolBar);
    CHECK(action->isEnabled());
    CHECK(toolBar->actions().contains(action));
    CHECK(action->iconText() == QStringLiteral("Open…"));
    CHECK(action->shortcut() == QKeySequence::Open);
}

TEST_CASE("unified open waits for vector capability before dispatching",
          "[ui][mainwindow][source-open][vector]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);

    window.openSources({"survey.gpkg"});
    QTest::qWait(20);
    CHECK(viewportPointer->document() == nullptr);

    viewportPointer->setVectorOverlayCapability(
        pci::VectorOverlayCapability::Supported);
    REQUIRE(waitFor([&] {
        return viewportPointer->document() != nullptr &&
               viewportPointer->document()->vectorLayerCount() == 1;
    }));
}

TEST_CASE("unified open loads a mixed point vector and raster selection",
          "[ui][mainwindow][source-open]")
{
    auto rasterLoader = std::make_shared<StubRasterLoader>();
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services =
        makeTestImportServices(std::make_shared<ImmediateLoader>(),
                               std::make_shared<DisjointVectorLoader>(),
                               {},
                               rasterLoader);
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    viewportPointer->setVectorOverlayCapability(
        pci::VectorOverlayCapability::Supported);

    window.openSources({"cloud.laz", "ortho.tif", "survey.gpkg"});

    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 1 &&
               viewportPointer->document()->vectorLayerCount() == 1 &&
               viewportPointer->document()->rasterLayerCount() == 1;
    }));
    CHECK(rasterLoader->inspectCalls.load() == 1);
    CHECK(rasterLoader->lastSourcePath == std::filesystem::path("ortho.tif"));
}

TEST_CASE("unified open routes GeoPackage exclusively to vector loading",
          "[ui][mainwindow][source-open][gpkg]")
{
    auto rasterLoader = std::make_shared<StubRasterLoader>();
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services =
        makeTestImportServices(std::make_shared<ImmediateLoader>(),
                               std::make_shared<DisjointVectorLoader>(),
                               {},
                               rasterLoader);
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    viewportPointer->setVectorOverlayCapability(
        pci::VectorOverlayCapability::Supported);

    window.openSources({"catalog.gti.gpkg"});

    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->vectorLayerCount() == 1;
    }));
    CHECK(viewportPointer->document()->rasterLayerCount() == 0);
    CHECK(rasterLoader->inspectCalls.load() == 0);
}

TEST_CASE("main window accepts and opens mixed local file drops",
          "[ui][mainwindow][source-open][drag-drop]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString cloudPath = directory.filePath(QStringLiteral("mätning.laz"));
    const QString rasterPath = directory.filePath(QStringLiteral("ortho.tif"));
    const QString vectorPath = directory.filePath(QStringLiteral("roads.gpkg"));
    for (const QString &path : {cloudPath, rasterPath, vectorPath}) {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
    }

    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    viewportPointer->setVectorOverlayCapability(
        pci::VectorOverlayCapability::Supported);

    QMimeData mimeData;
    mimeData.setUrls({QUrl::fromLocalFile(cloudPath),
                      QUrl::fromLocalFile(rasterPath),
                      QUrl::fromLocalFile(vectorPath)});
    QDragEnterEvent enter(QPoint(10, 10),
                          Qt::CopyAction,
                          &mimeData,
                          Qt::LeftButton,
                          Qt::NoModifier);
    QApplication::sendEvent(&window, &enter);
    CHECK(enter.isAccepted());

    QDropEvent drop(QPointF(10.0, 10.0),
                    Qt::CopyAction,
                    &mimeData,
                    Qt::LeftButton,
                    Qt::NoModifier);
    QApplication::sendEvent(&window, &drop);
    CHECK(drop.isAccepted());
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 1 &&
               viewportPointer->document()->vectorLayerCount() == 1 &&
               viewportPointer->document()->rasterLayerCount() == 1;
    }));
    CHECK(viewportPointer->document()
              ->pointLayers()
              .front()
              .scene->metadata()
              .sourcePath.filename() == std::filesystem::path(u8"mätning.laz"));
}

TEST_CASE("main window rejects non-local and directory-only drops",
          "[ui][mainwindow][source-open][drag-drop]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(
        std::make_unique<FakeViewport>(), std::move(services), 100);

    QMimeData mimeData;
    mimeData.setUrls({QUrl(QStringLiteral("https://example.com/cloud.laz")),
                      QUrl::fromLocalFile(directory.path())});
    QDragEnterEvent enter(QPoint(10, 10),
                          Qt::CopyAction,
                          &mimeData,
                          Qt::LeftButton,
                          Qt::NoModifier);
    QApplication::sendEvent(&window, &enter);
    CHECK_FALSE(enter.isAccepted());
}

TEST_CASE("main window rejects incomplete or mismatched import services",
          "[ui][mainwindow][import-services]")
{
    pci::ImportServices incomplete;
    CHECK_THROWS_AS(pci::MainWindow(std::make_unique<FakeViewport>(),
                                    std::move(incomplete),
                                    100),
                    std::invalid_argument);

    auto scheduler = std::make_unique<pci::TaskScheduler>(1, 1024 * 1024);
    auto otherScheduler = std::make_unique<pci::TaskScheduler>(1, 1024 * 1024);
    pci::ImportServices mismatched;
    mismatched.scheduler = std::move(scheduler);
    mismatched.pointCloud = std::make_unique<pci::PointCloudLoadController>(
        std::make_shared<ImmediateLoader>(), *mismatched.scheduler);
    mismatched.vector = std::make_unique<pci::VectorLoadController>(
        std::make_shared<DisjointVectorLoader>(), *otherScheduler);
    mismatched.statistics = std::make_shared<UnavailableStatistics>();
    CHECK_FALSE(mismatched.valid());
    CHECK_THROWS_AS(pci::MainWindow(std::make_unique<FakeViewport>(),
                                    std::move(mismatched),
                                    100),
                    std::invalid_argument);
}

TEST_CASE("import services destroy controllers before waiting for workers",
          "[ui][import-services][shutdown]")
{
    auto loader = std::make_shared<SlowLoader>();
    pci::ImportServices services = makeTestImportServices(loader);
    static_cast<void>(services.pointCloud->load({.sourcePath = "slow.las"}));
    REQUIRE(waitFor([&] {
        return loader->started.load();
    }));

    services.shutdown();

    CHECK(loader->stopped.load());
    CHECK_FALSE(services.scheduler);
    CHECK_FALSE(services.pointCloud);
    CHECK_FALSE(services.vector);
    CHECK_FALSE(services.colorize);
    CHECK_FALSE(services.statistics);
}

TEST_CASE("main window retains an XY-disjoint vector import hidden",
          "[ui][mainwindow][vector]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    REQUIRE(services.valid());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();
    viewportPointer->setVectorOverlayCapability(
        pci::VectorOverlayCapability::Supported);

    pci::VectorImportRequest request;
    request.limits.maximumApplicationWorkingBytes = 1024;
    request.sublayers = {{.index = 0, .name = "remote"}};
    static_cast<void>(window.loadVectorLayers(std::move(request)));
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->vectorLayers().size() == 1;
    }));
    const auto layers = viewportPointer->document()->vectorLayers();
    REQUIRE(layers.size() == 1);
    CHECK_FALSE(layers.front().visible);
    CHECK(layers.front().data->extentDisjointXY);
    const int framedAfterFirstLayer =
        viewportPointer->frameVisibleLayersCount();
    CHECK(framedAfterFirstLayer == 1);
    auto *fit = window.findChild<QAction *>(QStringLiteral("fitSceneAction"));
    REQUIRE(fit != nullptr);
    fit->trigger();
    CHECK(viewportPointer->frameVisibleLayersCount() ==
          framedAfterFirstLayer + 1);
}

TEST_CASE(
    "first point load replaces a vector-only document and preserves vectors",
    "[ui][mainwindow][vector][replace]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    viewportPointer->setVectorOverlayCapability(
        pci::VectorOverlayCapability::Supported);

    pci::VectorImportRequest request;
    request.limits.maximumApplicationWorkingBytes = 1024;
    request.sublayers = {{.index = 0, .name = "survey"}};
    static_cast<void>(window.loadVectorLayers(std::move(request)));
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->vectorLayerCount() == 1;
    }));

    const pci::SceneDocumentSnapshotPtr vectorOnlyDocument =
        viewportPointer->document();
    const pci::VectorLayer original =
        vectorOnlyDocument->vectorLayers().front();
    window.loadPointCloud("first.las", pci::PointCloudLoadMode::Add);
    REQUIRE(waitFor([&] {
        return viewportPointer->document() != vectorOnlyDocument &&
               viewportPointer->document()->layerCount() == 1;
    }));

    const pci::SceneDocumentSnapshotPtr replacement =
        viewportPointer->document();
    CHECK(replacement->hasAnyLayer());
    CHECK(replacement->hasPointCloudLayers());
    REQUIRE(replacement->vectorLayerCount() == 1);
    const pci::VectorLayer preserved = replacement->vectorLayers().front();
    CHECK(preserved.id == original.id);
    CHECK(preserved.data == original.data);
    CHECK(preserved.style == original.style);
    CHECK(preserved.visible == original.visible);
}

TEST_CASE("main window exposes exclusive viewport tools",
          "[ui][mainwindow][tools]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);

    QAction *navigate =
        window.findChild<QAction *>(QStringLiteral("navigateToolAction"));
    QAction *measure =
        window.findChild<QAction *>(QStringLiteral("measureToolAction"));
    QToolBar *rail =
        window.findChild<QToolBar *>(QStringLiteral("viewportToolRail"));
    REQUIRE(navigate != nullptr);
    REQUIRE(measure != nullptr);
    REQUIRE(rail != nullptr);
    CHECK_FALSE(measure->isEnabled());
    CHECK(rail->actions().contains(navigate));
    CHECK(rail->actions().contains(measure));
    CHECK_FALSE(navigate->icon().isNull());
    CHECK_FALSE(measure->icon().isNull());
    CHECK(rail->iconSize() == QSize(20, 20));

    window.loadPointCloud("measure.las");
    REQUIRE(waitFor([&] {
        return measure->isEnabled();
    }));
    measure->setChecked(true);
    CHECK(viewportPointer->activeTool() == pci::ViewportTool::Measure);
    CHECK_FALSE(navigate->isChecked());
    navigate->setChecked(true);
    CHECK(viewportPointer->activeTool() == pci::ViewportTool::Navigate);
}

TEST_CASE("selected layers expose a point-cloud statistics dialog",
          "[ui][mainwindow][statistics]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    QAction *statisticsAction = window.findChild<QAction *>(
        QStringLiteral("pointCloudStatisticsAction"));
    QAction *colorizeAction =
        window.findChild<QAction *>(QStringLiteral("colorizeFromRasterAction"));
    QAction *revertColorsAction =
        window.findChild<QAction *>(QStringLiteral("revertRasterColorsAction"));
    QMenu *layerMenu = window.findChild<QMenu *>(QStringLiteral("layerMenu"));
    REQUIRE(statisticsAction != nullptr);
    REQUIRE(colorizeAction != nullptr);
    REQUIRE(revertColorsAction != nullptr);
    REQUIRE(layerMenu != nullptr);
    CHECK_FALSE(statisticsAction->isEnabled());
    CHECK(layerMenu->actions().contains(statisticsAction));
    CHECK(layerMenu->actions().contains(colorizeAction));
    CHECK(layerMenu->actions().contains(revertColorsAction));

    window.loadPointCloud("range-first.las");
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 1 &&
               statisticsAction->isEnabled();
    }));
    statisticsAction->trigger();

    auto *dialog = window.findChild<QDialog *>(
        QStringLiteral("pointCloudStatisticsDialog"));
    REQUIRE(dialog != nullptr);
    CHECK(dialog->isVisible());
    auto *pointCount = dialog->findChild<QLabel *>(
        QStringLiteral("statisticsPointCountValue"));
    auto *extent =
        dialog->findChild<QLabel *>(QStringLiteral("statisticsExtentValue"));
    REQUIRE(pointCount != nullptr);
    REQUIRE(extent != nullptr);
    CHECK(pointCount->text() == QStringLiteral("3"));
    CHECK(extent->text().contains(QStringLiteral("10.0000")));
}

TEST_CASE("main window exposes a synchronized eye-dome lighting checkbox",
          "[ui][mainwindow][edl]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    QAction *action =
        window.findChild<QAction *>(QStringLiteral("eyeDomeLightingAction"));
    QCheckBox *checkBox = window.findChild<QCheckBox *>(
        QStringLiteral("eyeDomeLightingCheckBox"));
    QToolBar *toolBar =
        window.findChild<QToolBar *>(QStringLiteral("pointCloudToolBar"));
    REQUIRE(action != nullptr);
    REQUIRE(checkBox != nullptr);
    REQUIRE(toolBar != nullptr);
    CHECK(action->isCheckable());
    CHECK(action->isChecked());
    CHECK(checkBox->isChecked());
    REQUIRE(waitFor([&] {
        return checkBox->isVisibleTo(toolBar);
    }));
    CHECK(viewportPointer->eyeDomeLightingEnabled());
    CHECK(viewportPointer->eyeDomeLightingSetCount() == 0);

    checkBox->setChecked(false);
    CHECK_FALSE(action->isChecked());
    CHECK_FALSE(viewportPointer->eyeDomeLightingEnabled());
    CHECK(viewportPointer->eyeDomeLightingSetCount() == 1);

    action->setChecked(true);
    CHECK(checkBox->isChecked());
    CHECK(viewportPointer->eyeDomeLightingEnabled());
    CHECK(viewportPointer->eyeDomeLightingSetCount() == 2);
}

TEST_CASE("settings dialog applies viewport appearance and depth enhancement",
          "[ui][mainwindow][settings]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    QAction *settingsAction =
        window.findChild<QAction *>(QStringLiteral("settingsAction"));
    QAction *depthAction =
        window.findChild<QAction *>(QStringLiteral("eyeDomeLightingAction"));
    QCheckBox *depthCheckBox = window.findChild<QCheckBox *>(
        QStringLiteral("eyeDomeLightingCheckBox"));
    QToolBar *toolBar =
        window.findChild<QToolBar *>(QStringLiteral("pointCloudToolBar"));
    REQUIRE(settingsAction != nullptr);
    REQUIRE(depthAction != nullptr);
    REQUIRE(depthCheckBox != nullptr);
    REQUIRE(toolBar != nullptr);
    CHECK(toolBar->actions().contains(settingsAction));
    auto *settingsButton =
        qobject_cast<QToolButton *>(toolBar->widgetForAction(settingsAction));
    REQUIRE(settingsButton != nullptr);
    CHECK(settingsButton->text() == QStringLiteral("Settings…"));

    settingsButton->click();
    auto *dialog = window.findChild<pci::SettingsDialog *>(
        QStringLiteral("settingsDialog"));
    REQUIRE(dialog != nullptr);
    REQUIRE(dialog->isVisible());
    CHECK_FALSE(dialog->isModal());

    pci::ViewportSettings changed;
    changed.backgroundColor = {.red = 0.2F, .green = 0.3F, .blue = 0.4F};
    changed.depthEnhancement = {
        .enabled = false,
        .radius = 2.5F,
        .strength = 40.0F,
    };
    const pci::PerformanceSettings changedPerformance{
        .automaticCpuCache = false,
        .cpuCacheMebibytes = 256,
        .gpuCacheMebibytes = 64,
        .maximumLoadPoints = 20'000'000,
    };
    dialog->setSettings(changed);
    dialog->setPerformanceSettings(changedPerformance);

    const pci::ViewportSettings applied = viewportPointer->viewportSettings();
    CHECK(applied.backgroundColor.red == Catch::Approx(0.2F).margin(0.0001F));
    CHECK(applied.backgroundColor.green == Catch::Approx(0.3F).margin(0.0001F));
    CHECK(applied.backgroundColor.blue == Catch::Approx(0.4F).margin(0.0001F));
    CHECK(applied.depthEnhancement == changed.depthEnhancement);
    CHECK(viewportPointer->viewportSettingsSetCount() == 1);
    CHECK(viewportPointer->gpuByteBudget() == std::uint64_t{64} * 1024 * 1024);
    CHECK_FALSE(depthAction->isChecked());
    CHECK_FALSE(depthCheckBox->isChecked());
    dialog->reject();

    CHECK(viewportPointer->viewportSettings() == pci::ViewportSettings{});
    CHECK(viewportPointer->gpuByteBudget() == std::uint64_t{512} * 1024 * 1024);
    CHECK(depthAction->isChecked());
    CHECK(depthCheckBox->isChecked());
}

TEST_CASE("main window exposes a bounded point-size selector",
          "[ui][mainwindow][point-size]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    QSpinBox *pointSize =
        window.findChild<QSpinBox *>(QStringLiteral("pointSizeSpinBox"));
    QToolBar *toolBar =
        window.findChild<QToolBar *>(QStringLiteral("pointCloudToolBar"));
    REQUIRE(pointSize != nullptr);
    REQUIRE(toolBar != nullptr);
    CHECK(pointSize->minimum() == pci::minimumPointSizePixels);
    CHECK(pointSize->maximum() == pci::maximumPointSizePixels);
    CHECK(pointSize->value() == pci::defaultPointSizePixels);
    REQUIRE(waitFor([&] {
        return pointSize->isVisibleTo(toolBar);
    }));
    CHECK(viewportPointer->pointSizePixels() == pci::defaultPointSizePixels);
    CHECK(viewportPointer->pointSizeSetCount() == 0);

    pointSize->setValue(pci::maximumPointSizePixels);
    CHECK(viewportPointer->pointSizePixels() == pci::maximumPointSizePixels);
    CHECK(viewportPointer->pointSizeSetCount() == 1);

    pointSize->setValue(pci::minimumPointSizePixels);
    CHECK(viewportPointer->pointSizePixels() == pci::minimumPointSizePixels);
    CHECK(viewportPointer->pointSizeSetCount() == 2);
}

TEST_CASE("main window color selectors apply compatible renderer modes",
          "[ui][mainwindow]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport),
                           std::move(services),
                           100,
                           pci::defaultPointCloudDecodedByteBudget,
                           std::nullopt,
                           {},
                           pci::test::createTestPointColorMapCatalog());
    window.show();

    auto *sources =
        window.findChild<QComboBox *>(QStringLiteral("colorSourceComboBox"));
    auto *maps =
        window.findChild<QComboBox *>(QStringLiteral("colorMapComboBox"));
    REQUIRE(sources != nullptr);
    REQUIRE(maps != nullptr);

    window.loadPointCloud("fixture.las");
    REQUIRE(waitFor([&] {
        return viewportPointer->totalPointCount() == 3;
    }));
    REQUIRE(waitFor([&] {
        return sources->findText(QStringLiteral("Intensity")) >= 0 &&
               sources->findText(QStringLiteral("Classification")) >= 0;
    }));

    const int zIndex = sources->findText(QStringLiteral("Z"));
    REQUIRE(zIndex >= 0);
    sources->setCurrentIndex(zIndex);
    REQUIRE(viewportPointer->document());
    const auto layers = viewportPointer->document()->pointLayers();
    REQUIRE(layers.size() == 1);
    CHECK(layers.front().colorMode ==
          pci::PointColorMode{
              .source = pci::PointColorSource::Z,
              .colorMap = pci::PointColorMap::Viridis,
          });
    REQUIRE(waitFor([&] {
        return maps->findText(QStringLiteral("Turbo")) >= 0;
    }));
    REQUIRE(maps->count() == 2);
    CHECK(maps->itemText(0) == QStringLiteral("Turbo"));
    CHECK(maps->itemText(1) == QStringLiteral("Viridis"));

    const int turboIndex = maps->findText(QStringLiteral("Turbo"));
    REQUIRE(turboIndex >= 0);
    const QString turboToolTip =
        maps->itemData(turboIndex, Qt::ToolTipRole).toString();
    CHECK(turboToolTip.contains(QStringLiteral("high-contrast")));
    maps->setCurrentIndex(turboIndex);
    CHECK(maps->toolTip() == turboToolTip);
    CHECK(
        viewportPointer->document()->pointLayers().front().colorMode.colorMap ==
        pci::PointColorMap::Turbo);

    const int classificationIndex =
        sources->findText(QStringLiteral("Classification"));
    REQUIRE(classificationIndex >= 0);
    sources->setCurrentIndex(classificationIndex);
    CHECK(viewportPointer->document()->pointLayers().front().colorMode.source ==
          pci::PointColorSource::Classification);
    CHECK(
        viewportPointer->document()->pointLayers().front().colorMode.colorMap ==
        pci::PointColorMap::LasClassification);
}

TEST_CASE("main window add mode keeps per-layer colors independent",
          "[ui][mainwindow]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport),
                           std::move(services),
                           100,
                           pci::defaultPointCloudDecodedByteBudget,
                           std::nullopt,
                           {},
                           pci::test::createTestPointColorMapCatalog());
    window.show();

    auto *openAction =
        window.findChild<QAction *>(QStringLiteral("openFilesAction"));
    auto *list =
        window.findChild<QListView *>(QStringLiteral("pointCloudLayerList"));
    auto *sources =
        window.findChild<QComboBox *>(QStringLiteral("colorSourceComboBox"));
    REQUIRE(openAction != nullptr);
    REQUIRE(list != nullptr);
    REQUIRE(sources != nullptr);

    window.loadPointCloud("first.las");
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 1;
    }));
    const auto firstDocument = viewportPointer->document();
    const auto firstLayer = firstDocument->pointLayers().front();
    CHECK_FALSE(openAction->isEnabled());
    viewportPointer->emitDisplayReady(firstLayer.id);
    REQUIRE(waitFor([&] {
        return openAction->isEnabled();
    }));

    window.loadPointCloud("scalar-only.las", pci::PointCloudLoadMode::Add);
    REQUIRE(waitFor([&] {
        return viewportPointer->document()->layerCount() == 2;
    }));
    CHECK(viewportPointer->document() != firstDocument);
    CHECK(viewportPointer->documentSetCount() == 1);
    CHECK(viewportPointer->lastDocumentWasFramed());
    REQUIRE(waitFor([&] {
        return list->model()->rowCount() == 2;
    }));

    const auto secondLayer = viewportPointer->document()->pointLayers().at(1);

    // Select the first layer and color it by intensity — only it has that
    // attribute, so the choice must not leak onto the scalar-only layer.
    list->setCurrentIndex(list->model()->index(0, 0));
    REQUIRE(waitFor([&] {
        return sources->findText(QStringLiteral("Intensity")) >= 0;
    }));
    const int intensityIndex = sources->findText(QStringLiteral("Intensity"));
    REQUIRE(intensityIndex >= 0);
    sources->setCurrentIndex(intensityIndex);
    CHECK(viewportPointer->document()->layer(firstLayer.id)->colorMode.source ==
          pci::PointColorSource::Intensity);

    // The scalar-only layer offers no intensity and keeps its own color mode.
    list->setCurrentIndex(list->model()->index(1, 0));
    CHECK(sources->findText(QStringLiteral("Intensity")) < 0);
    CHECK(
        viewportPointer->document()->layer(secondLayer.id)->colorMode.source !=
        pci::PointColorSource::Intensity);
    CHECK(viewportPointer->document()->layer(firstLayer.id)->colorMode.source ==
          pci::PointColorSource::Intensity);
}

TEST_CASE("layer colors can be applied to every compatible point cloud",
          "[ui][mainwindow][color]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport),
                           std::move(services),
                           100,
                           pci::defaultPointCloudDecodedByteBudget,
                           std::nullopt,
                           {},
                           pci::test::createTestPointColorMapCatalog());
    window.show();

    auto *list =
        window.findChild<QListView *>(QStringLiteral("pointCloudLayerList"));
    auto *sources =
        window.findChild<QComboBox *>(QStringLiteral("colorSourceComboBox"));
    auto *maps =
        window.findChild<QComboBox *>(QStringLiteral("colorMapComboBox"));
    auto *applyToAll = window.findChild<QPushButton *>(
        QStringLiteral("applyColorToAllButton"));
    REQUIRE(list != nullptr);
    REQUIRE(sources != nullptr);
    REQUIRE(maps != nullptr);
    REQUIRE(applyToAll != nullptr);

    window.loadPointCloud("first.las");
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 1;
    }));
    const auto firstLayer = viewportPointer->document()->pointLayers().front();
    viewportPointer->emitDisplayReady(firstLayer.id);

    window.loadPointCloud("scalar-only.las", pci::PointCloudLoadMode::Add);
    REQUIRE(waitFor([&] {
        return viewportPointer->document()->layerCount() == 2 &&
               list->model()->rowCount() == 2;
    }));

    list->setCurrentIndex(list->model()->index(0, 0));
    sources->setCurrentIndex(sources->findText(QStringLiteral("Z")));
    maps->setCurrentIndex(maps->findText(QStringLiteral("Turbo")));
    REQUIRE(applyToAll->isEnabled());
    applyToAll->click();

    for (const pci::PointCloudLayer &layer :
         viewportPointer->document()->pointLayers()) {
        CHECK(layer.colorMode.source == pci::PointColorSource::Z);
        CHECK(layer.colorMode.colorMap == pci::PointColorMap::Turbo);
    }
    CHECK_FALSE(applyToAll->isEnabled());

    sources->setCurrentIndex(sources->findText(QStringLiteral("Intensity")));
    CHECK_FALSE(applyToAll->isEnabled());
    CHECK(applyToAll->toolTip().contains(
        QStringLiteral("not available for every point cloud")));
}

TEST_CASE("classification dialog applies to one layer or all layers",
          "[ui][mainwindow][classification]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    auto *filterButton = window.findChild<QPushButton *>(
        QStringLiteral("classificationFilterButton"));
    auto *layerList =
        window.findChild<QListView *>(QStringLiteral("pointCloudLayerList"));
    REQUIRE(filterButton != nullptr);
    REQUIRE(layerList != nullptr);

    window.loadPointCloud("classified-first.las");
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 1 &&
               filterButton->isVisible();
    }));
    const pci::PointCloudLayerId firstLayerId =
        viewportPointer->document()->pointLayers().front().id;
    viewportPointer->emitDisplayReady(firstLayerId);

    window.loadPointCloud("classified-second.las",
                          pci::PointCloudLoadMode::Add);
    REQUIRE(waitFor([&] {
        return viewportPointer->document()->layerCount() == 2 &&
               layerList->model()->rowCount() == 2;
    }));
    const pci::PointCloudLayerId secondLayerId =
        viewportPointer->document()->pointLayers().at(1).id;
    viewportPointer->emitDisplayReady(secondLayerId);
    layerList->setCurrentIndex(layerList->model()->index(0, 0));

    const int renderRequestsBefore = viewportPointer->renderRequestCount();

    filterButton->click();
    auto *dialog = window.findChild<QDialog *>(
        QStringLiteral("classificationFilterDialog"));
    REQUIRE(dialog != nullptr);
    auto *classifications = dialog->findChild<QListWidget *>(
        QStringLiteral("classificationFilterList"));
    auto *clearAll = dialog->findChild<QPushButton *>(
        QStringLiteral("clearAllClassificationsButton"));
    auto *applySelected = dialog->findChild<QPushButton *>(
        QStringLiteral("applyClassificationToSelectedButton"));
    auto *applyAll = dialog->findChild<QPushButton *>(
        QStringLiteral("applyClassificationToAllButton"));
    REQUIRE(classifications != nullptr);
    REQUIRE(clearAll != nullptr);
    REQUIRE(applySelected != nullptr);
    REQUIRE(applyAll != nullptr);
    std::vector<int> listedClassifications;
    listedClassifications.reserve(
        static_cast<std::size_t>(classifications->count()));
    for (int row = 0; row < classifications->count(); ++row) {
        listedClassifications.push_back(
            classifications->item(row)->data(Qt::UserRole).toInt());
    }
    CHECK(listedClassifications == std::vector{2, 5, 6, 9, 17});
    const auto classificationRow = [classifications](const int classification) {
        for (int row = 0; row < classifications->count(); ++row) {
            if (classifications->item(row)->data(Qt::UserRole).toInt() ==
                classification) {
                return row;
            }
        }
        return -1;
    };

    clearAll->click();
    REQUIRE(classificationRow(2) >= 0);
    classifications->item(classificationRow(2))->setCheckState(Qt::Checked);
    applySelected->click();

    REQUIRE(waitFor([&] {
        const pci::PointClassificationFilter first = viewportPointer->document()
                                                         ->layer(firstLayerId)
                                                         ->classificationFilter;
        return first.isVisible(2) && !first.isVisible(5) &&
               !first.isVisible(6) && !first.isVisible(9) &&
               !first.isVisible(17) &&
               viewportPointer->document()
                   ->layer(secondLayerId)
                   ->classificationFilter.allVisible();
    }));
    const pci::PointClassificationFilter filter =
        viewportPointer->document()->layer(firstLayerId)->classificationFilter;
    CHECK(filter.isVisible(2));
    CHECK_FALSE(filter.isVisible(5));
    CHECK(viewportPointer->renderRequestCount() == renderRequestsBefore + 1);
    CHECK(filterButton->text().contains(QStringLiteral("1 of 5")));

    REQUIRE(waitFor([&] {
        return window.findChild<QDialog *>(
                   QStringLiteral("classificationFilterDialog")) == nullptr;
    }));
    filterButton->click();
    dialog = window.findChild<QDialog *>(
        QStringLiteral("classificationFilterDialog"));
    REQUIRE(dialog != nullptr);
    classifications = dialog->findChild<QListWidget *>(
        QStringLiteral("classificationFilterList"));
    clearAll = dialog->findChild<QPushButton *>(
        QStringLiteral("clearAllClassificationsButton"));
    applyAll = dialog->findChild<QPushButton *>(
        QStringLiteral("applyClassificationToAllButton"));
    REQUIRE(classifications != nullptr);
    REQUIRE(clearAll != nullptr);
    REQUIRE(applyAll != nullptr);

    int classSixRow = -1;
    for (int row = 0; row < classifications->count(); ++row) {
        if (classifications->item(row)->data(Qt::UserRole).toInt() == 6) {
            classSixRow = row;
            break;
        }
    }
    clearAll->click();
    REQUIRE(classSixRow >= 0);
    classifications->item(classSixRow)->setCheckState(Qt::Checked);
    applyAll->click();

    REQUIRE(waitFor([&] {
        const auto first = viewportPointer->document()->layer(firstLayerId);
        const auto second = viewportPointer->document()->layer(secondLayerId);
        return first && second && first->classificationFilter.isVisible(6) &&
               !first->classificationFilter.isVisible(2) &&
               !first->classificationFilter.isVisible(5) &&
               !first->classificationFilter.isVisible(9) &&
               !first->classificationFilter.isVisible(17) &&
               second->classificationFilter == first->classificationFilter;
    }));
    CHECK(viewportPointer->renderRequestCount() == renderRequestsBefore + 2);
}

TEST_CASE("coordinate color ranges default to all document layers",
          "[ui][mainwindow][color]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport),
                           std::move(services),
                           100,
                           pci::defaultPointCloudDecodedByteBudget,
                           std::nullopt,
                           {},
                           pci::test::createTestPointColorMapCatalog());
    window.show();

    auto *list =
        window.findChild<QListView *>(QStringLiteral("pointCloudLayerList"));
    auto *sources =
        window.findChild<QComboBox *>(QStringLiteral("colorSourceComboBox"));
    auto *automatic = window.findChild<QCheckBox *>(
        QStringLiteral("automaticColorRangeCheckBox"));
    auto *minimum = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("colorRangeMinimumSpinBox"));
    auto *maximum = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("colorRangeMaximumSpinBox"));
    REQUIRE(list != nullptr);
    REQUIRE(sources != nullptr);
    REQUIRE(automatic != nullptr);
    REQUIRE(minimum != nullptr);
    REQUIRE(maximum != nullptr);

    window.loadPointCloud("range-first.las");
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 1 &&
               list->model()->rowCount() == 1;
    }));
    const auto firstLayer = viewportPointer->document()->pointLayers().front();
    viewportPointer->emitDisplayReady(firstLayer.id);

    list->setCurrentIndex(list->model()->index(0, 0));
    const int xIndex = sources->findText(QStringLiteral("X"));
    REQUIRE(xIndex >= 0);
    sources->setCurrentIndex(xIndex);
    CHECK(automatic->isChecked());
    CHECK(minimum->value() == Catch::Approx(10.0));
    CHECK(maximum->value() == Catch::Approx(20.0));

    window.loadPointCloud("range-second.las", pci::PointCloudLoadMode::Add);
    REQUIRE(waitFor([&] {
        return viewportPointer->document()->layerCount() == 2 &&
               list->model()->rowCount() == 2;
    }));
    const auto secondLayer = viewportPointer->document()->pointLayers().at(1);
    viewportPointer->emitDisplayReady(secondLayer.id);

    // The selected first layer now uses the complete scene range. Hiding the
    // second layer intentionally leaves that stable normalization unchanged.
    CHECK(minimum->value() == Catch::Approx(-5.0));
    CHECK(maximum->value() == Catch::Approx(100.0));
    list->model()->setData(
        list->model()->index(1, 0), Qt::Unchecked, Qt::CheckStateRole);
    CHECK(minimum->value() == Catch::Approx(-5.0));
    CHECK(maximum->value() == Catch::Approx(100.0));

    automatic->setChecked(false);
    minimum->setValue(0.0);
    maximum->setValue(50.0);
    const auto manualMode =
        viewportPointer->document()->layer(firstLayer.id)->colorMode;
    REQUIRE(manualMode.manualRange.has_value());
    CHECK(manualMode.manualRange->minimum == Catch::Approx(0.0));
    CHECK(manualMode.manualRange->maximum == Catch::Approx(50.0));

    // Manual ranges remain per layer; the other layer starts in automatic
    // mode and switching back restores the global document range.
    list->setCurrentIndex(list->model()->index(1, 0));
    sources->setCurrentIndex(sources->findText(QStringLiteral("X")));
    CHECK(automatic->isChecked());
    CHECK(minimum->value() == Catch::Approx(-5.0));
    CHECK(maximum->value() == Catch::Approx(100.0));
    list->setCurrentIndex(list->model()->index(0, 0));
    CHECK_FALSE(automatic->isChecked());
    CHECK(minimum->value() == Catch::Approx(0.0));
    CHECK(maximum->value() == Catch::Approx(50.0));
    automatic->setChecked(true);
    CHECK_FALSE(viewportPointer->document()
                    ->layer(firstLayer.id)
                    ->colorMode.manualRange);
    CHECK(minimum->value() == Catch::Approx(-5.0));
    CHECK(maximum->value() == Catch::Approx(100.0));
}

TEST_CASE("layer panel reflects and toggles document layers",
          "[ui][mainwindow]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    auto *openAction =
        window.findChild<QAction *>(QStringLiteral("openFilesAction"));
    auto *panel =
        window.findChild<QDockWidget *>(QStringLiteral("pointCloudLayerPanel"));
    auto *toggle = window.findChild<QAction *>(
        QStringLiteral("pointCloudLayerPanelToggleAction"));
    auto *list =
        window.findChild<QListView *>(QStringLiteral("pointCloudLayerList"));
    REQUIRE(openAction != nullptr);
    REQUIRE(panel != nullptr);
    REQUIRE(toggle != nullptr);
    REQUIRE(list != nullptr);
    CHECK(list->model()->rowCount() == 0);

    window.loadPointCloud("first.las");
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 1;
    }));
    const auto firstLayer = viewportPointer->document()->pointLayers().front();
    viewportPointer->emitDisplayReady(firstLayer.id);
    REQUIRE(waitFor([&] {
        return openAction->isEnabled();
    }));

    window.loadPointCloud("second.las", pci::PointCloudLoadMode::Add);
    REQUIRE(waitFor([&] {
        return viewportPointer->document()->layerCount() == 2;
    }));
    const auto secondLayer = viewportPointer->document()->pointLayers().at(1);

    REQUIRE(waitFor([&] {
        return list->model()->rowCount() == 2;
    }));
    CHECK(list->model()->index(0, 0).data().toString() ==
          QStringLiteral("first.las"));
    CHECK(list->model()->index(1, 0).data().toString() ==
          QStringLiteral("second.las"));
    CHECK(list->model()->index(0, 0).data(Qt::CheckStateRole).toInt() ==
          Qt::Checked);
    CHECK(list->model()->index(1, 0).data(Qt::CheckStateRole).toInt() ==
          Qt::Checked);

    // Selecting a layer populates the Properties fields, and those value
    // fields must be laid out with real width (regression: elided value
    // labels collapsed to zero width and showed nothing).
    window.resize(900, 700);
    list->setCurrentIndex(list->model()->index(0, 0));
    auto *pointsValue =
        window.findChild<QLabel *>(QStringLiteral("layerPointsValue"));
    auto *sourceValue =
        window.findChild<QLabel *>(QStringLiteral("layerSourceValue"));
    REQUIRE(pointsValue != nullptr);
    REQUIRE(sourceValue != nullptr);
    CHECK(sourceValue->text() == QStringLiteral("first.las"));
    CHECK(pointsValue->text() == QStringLiteral("3"));
    REQUIRE(waitFor([&] {
        return sourceValue->width() > 10;
    }));

    const int renderRequestsBeforeVisibility =
        viewportPointer->renderRequestCount();
    list->model()->setData(
        list->model()->index(1, 0), Qt::Unchecked, Qt::CheckStateRole);
    CHECK(viewportPointer->renderRequestCount() >
          renderRequestsBeforeVisibility);
    CHECK_FALSE(viewportPointer->document()->layer(secondLayer.id)->visible);
    CHECK(viewportPointer->document()->layer(firstLayer.id)->visible);

    list->model()->setData(
        list->model()->index(0, 0), Qt::Unchecked, Qt::CheckStateRole);
    CHECK_FALSE(viewportPointer->document()->layer(firstLayer.id)->visible);

    list->model()->setData(
        list->model()->index(1, 0), Qt::Checked, Qt::CheckStateRole);
    CHECK(viewportPointer->document()->layer(secondLayer.id)->visible);

    // The View-menu action toggles the dock panel's visibility.
    const bool wasVisible = panel->isVisible();
    toggle->trigger();
    CHECK(panel->isVisible() != wasVisible);
}

TEST_CASE("scene panel displays point and vector layers in document order",
          "[ui][vector][order]")
{
    pci::SceneLayersDock panel;
    pci::PointCloudMetadata metadata;
    metadata.sourcePath = "survey.las";
    auto scene = std::make_shared<pci::PointCloudScene>(metadata);
    auto data = std::make_shared<pci::VectorLayerData>();
    data->sublayerName = "Parcel boundaries";
    data->featureCount = 12;
    const std::vector points{pci::PointCloudLayer{.id = pci::SceneLayerId{7},
                                                  .scene = std::move(scene)}};
    const std::vector vectors{pci::VectorLayer{.id = pci::SceneLayerId{42},
                                               .data = std::move(data),
                                               .visible = false}};
    panel.setDocumentSnapshot(makeSnapshot(
        points, vectors, {pci::SceneLayerId{42}, pci::SceneLayerId{7}}));
    auto *list =
        panel.findChild<QListView *>(QStringLiteral("pointCloudLayerList"));
    REQUIRE(list != nullptr);
    REQUIRE(list->model()->rowCount() == 2);
    CHECK(list->model()->index(0, 0).data().toString() ==
          QStringLiteral("Parcel boundaries"));
    CHECK(list->model()->index(0, 0).data(Qt::CheckStateRole).toInt() ==
          Qt::Unchecked);
    CHECK(list->model()->index(1, 0).data().toString() ==
          QStringLiteral("survey.las"));
}

TEST_CASE("scene panel gates raster colorize and revert actions",
          "[ui][raster][colorize]")
{
    pci::SceneLayersDock panel;
    pci::PointCloudLayer point{
        .id = pci::SceneLayerId{7},
        .scene = makeUiColorizableScene("target.laz"),
    };
    const auto rasterData = makeUiRasterData("ortho.tif");
    const pci::RasterLayer raster{.id = pci::SceneLayerId{8},
                                  .data = rasterData};

    panel.setDocumentSnapshot(
        makeSnapshot({point}, {}, {point.id, raster.id}, {raster}));
    auto *colorize =
        panel.findChild<QAction *>(QStringLiteral("colorizeFromRasterAction"));
    auto *revert =
        panel.findChild<QAction *>(QStringLiteral("revertRasterColorsAction"));
    REQUIRE(colorize != nullptr);
    REQUIRE(revert != nullptr);
    CHECK(panel.findChild<QWidget *>(
              QStringLiteral("rasterColorizeActionStrip")) == nullptr);
    CHECK(colorize->isEnabled());
    CHECK(colorize->text() == QStringLiteral("Colorize from raster…"));
    CHECK_FALSE(revert->isEnabled());
    CHECK_FALSE(revert->isVisible());

    point.rasterColors = pci::RasterPointColorBinding{
        .rasterLayerId = raster.id,
        .rasterSourceId = rasterData->sourceId,
        .rasterSourcePath = rasterData->metadata().sourcePath,
        .decode = std::make_shared<pci::RasterDecodeParameters>(),
        .rasterRenderGeneration = raster.renderGeneration,
        .coloredPoints = 1,
        .crsRelation = pci::SpatialReferenceRelation::Same,
    };
    panel.setDocumentSnapshot(
        makeSnapshot({point}, {}, {point.id, raster.id}, {raster}));
    CHECK(revert->isEnabled());
    CHECK(revert->isVisible());
    CHECK(colorize->text() == QStringLiteral("Recolor from raster…"));

    panel.setColorizeJobState(true, false);
    CHECK_FALSE(colorize->isEnabled());
    CHECK(revert->isEnabled());
    CHECK(revert->toolTip().contains(QStringLiteral("Cancel")));

    panel.setColorizeJobState(true, true);
    CHECK_FALSE(revert->isEnabled());
    CHECK(revert->toolTip().contains(QStringLiteral("commit")));
}

TEST_CASE("raster colorize dialog and point inspector expose frozen bake state",
          "[ui][raster][colorize][inspector]")
{
    pci::PointCloudLayer point{
        .id = pci::SceneLayerId{17},
        .scene = makeUiColorizableScene("target.laz"),
    };
    const auto rasterData = makeUiRasterData("ortho.tif");
    const pci::RasterLayer raster{.id = pci::SceneLayerId{18},
                                  .data = rasterData};
    const auto snapshot =
        makeSnapshot({point}, {}, {point.id, raster.id}, {raster});

    pci::ColorizeFromRasterDialog dialog(
        point, snapshot, {}, std::numeric_limits<std::uint64_t>::max());
    auto *combo =
        dialog.findChild<QComboBox *>(QStringLiteral("colorizeRasterCombo"));
    auto *warning =
        dialog.findChild<QLabel *>(QStringLiteral("colorizeRasterWarning"));
    auto *apply = dialog.findChild<QPushButton *>(
        QStringLiteral("colorizeRasterApplyButton"));
    REQUIRE(combo != nullptr);
    REQUIRE(warning != nullptr);
    REQUIRE(apply != nullptr);
    CHECK(combo->count() == 1);
    CHECK(combo->currentText().contains(QStringLiteral("visible")));
    CHECK(dialog.selectedRasterLayerId() == raster.id);
    CHECK(warning->text() ==
          QStringLiteral("CRS comparison is unavailable in this build."));
    CHECK(apply->isEnabled());
    CHECK(apply->property("primary").toBool());
    auto *estimate =
        dialog.findChild<QLabel *>(QStringLiteral("colorizeRasterEstimate"));
    auto *behavior = dialog.findChild<QLabel *>(
        QStringLiteral("colorizeRasterBehaviorNotice"));
    REQUIRE(estimate != nullptr);
    REQUIRE(behavior != nullptr);
    CHECK(estimate->text().contains(QStringLiteral("Working memory")));
    CHECK(estimate->text().contains(QStringLiteral("Temporary disk")));
    CHECK(behavior->text().contains(QStringLiteral("opaque raster pixels")));

    point.rasterColors = pci::RasterPointColorBinding{
        .rasterLayerId = raster.id,
        .rasterSourceId = rasterData->sourceId,
        .rasterSourcePath = rasterData->metadata().sourcePath,
        .decode = std::make_shared<pci::RasterDecodeParameters>(),
        .rasterRenderGeneration = raster.renderGeneration,
        .coloredPoints = 1,
        .uncoloredPoints = 2,
        .crsRelation = pci::SpatialReferenceRelation::Same,
    };
    pci::LayerInspectorDock inspector;
    inspector.setDocumentSnapshot(
        makeSnapshot({point}, {}, {point.id, raster.id}, {raster}), point.id);
    auto *state = inspector.findChild<QLabel *>(
        QStringLiteral("layerRasterColorStateValue"));
    auto *details =
        inspector.findChild<QLabel *>(QStringLiteral("layerRasterColorsValue"));
    auto *revert = inspector.findChild<QPushButton *>(
        QStringLiteral("revertRasterColorsButton"));
    auto *colorize = inspector.findChild<QPushButton *>(
        QStringLiteral("inspectorColorizeFromRasterButton"));
    REQUIRE(state != nullptr);
    REQUIRE(details != nullptr);
    REQUIRE(revert != nullptr);
    REQUIRE(colorize != nullptr);
    CHECK(state->text() == QStringLiteral("Raster colors · ortho.tif"));
    CHECK(details->text().contains(QStringLiteral("1 colored")));
    CHECK(details->text().contains(QStringLiteral("2 kept source")));
    CHECK(details->text().contains(QStringLiteral("same CRS")));
    CHECK_FALSE(revert->isHidden());
    CHECK(colorize->text() == QStringLiteral("Recolor from raster…"));

    std::optional<pci::SceneLayerId> reverted;
    std::optional<pci::SceneLayerId> recolored;
    QObject::connect(&inspector,
                     &pci::LayerInspectorDock::revertRasterColorsRequested,
                     [&reverted](const pci::SceneLayerId id) {
                         reverted = id;
                     });
    QObject::connect(&inspector,
                     &pci::LayerInspectorDock::colorizeFromRasterRequested,
                     [&recolored](const pci::SceneLayerId id) {
                         recolored = id;
                     });
    colorize->click();
    CHECK(recolored == point.id);
    revert->click();
    CHECK(reverted == point.id);

    pci::ColorizeFromRasterDialog replacement(
        point, snapshot, {}, std::numeric_limits<std::uint64_t>::max());
    auto *replace = replacement.findChild<QPushButton *>(
        QStringLiteral("colorizeRasterApplyButton"));
    REQUIRE(replace != nullptr);
    CHECK(replace->text() == QStringLiteral("Replace raster colors"));
    CHECK(replacement.findChild<QLabel *>(
              QStringLiteral("colorizeRasterReplacementNotice")) != nullptr);
}

TEST_CASE("vector inspector projects and edits planar overlay style",
          "[ui][vector][inspector]")
{
    pci::LayerInspectorDock panel;
    auto data = std::make_shared<pci::VectorLayerData>();
    data->sublayerName = "Survey control";
    data->extentDisjointXY = true;
    data->featureCount = 17;
    data->summary.polygonParts = 4;
    data->sourceDriver = "GPKG";
    data->sourcePath = "survey.gpkg";
    data->spatialReferenceWkt = "EPSG:3006";
    pci::VectorLayer layer{
        .id = pci::SceneLayerId{42}, .data = std::move(data), .visible = false};
    layer.style.opacity = 0.75F;
    layer.style.zOffset = 12.0;

    std::optional<std::pair<pci::SceneLayerId, pci::VectorLayerStyle>> changed;
    QObject::connect(&panel,
                     &pci::LayerInspectorDock::vectorStyleChanged,
                     [&changed](const pci::SceneLayerId id,
                                const pci::VectorLayerStyle style) {
                         changed.emplace(id, style);
                     });
    panel.setDocumentSnapshot(makeSnapshot({}, {layer}, {layer.id}), layer.id);

    auto *inspector = &panel;
    REQUIRE(inspector != nullptr);
    auto *properties = inspector->findChild<QWidget *>(
        QStringLiteral("vectorLayerProperties"));
    auto *opacity = inspector->findChild<QDoubleSpinBox *>(
        QStringLiteral("vectorOpacitySpinBox"));
    auto *offset = inspector->findChild<QDoubleSpinBox *>(
        QStringLiteral("vectorZOffsetSpinBox"));
    auto *shape = inspector->findChild<QComboBox *>(
        QStringLiteral("vectorMarkerShapeComboBox"));
    auto *onTop = inspector->findChild<QCheckBox *>(
        QStringLiteral("vectorAlwaysOnTopCheckBox"));
    auto *type = inspector->findChild<QLabel *>(
        QStringLiteral("pointCloudInspectorType"));
    auto *warning =
        inspector->findChild<QWidget *>(QStringLiteral("vectorExtentWarning"));
    auto *showAnyway = inspector->findChild<QPushButton *>(
        QStringLiteral("vectorShowAnywayButton"));
    auto *features =
        inspector->findChild<QLabel *>(QStringLiteral("vectorFeaturesValue"));
    auto *driver =
        inspector->findChild<QLabel *>(QStringLiteral("vectorDriverValue"));
    auto *crs =
        inspector->findChild<QLabel *>(QStringLiteral("vectorCrsValue"));
    REQUIRE(properties != nullptr);
    REQUIRE(opacity != nullptr);
    REQUIRE(offset != nullptr);
    REQUIRE(shape != nullptr);
    REQUIRE(onTop != nullptr);
    REQUIRE(type != nullptr);
    REQUIRE(warning != nullptr);
    REQUIRE(showAnyway != nullptr);
    REQUIRE(features != nullptr);
    REQUIRE(driver != nullptr);
    REQUIRE(crs != nullptr);
    // The standalone panel's inspector dock is not shown, so assert its
    // local visibility state rather than effective on-screen visibility.
    CHECK_FALSE(properties->isHidden());
    CHECK(type->text() == QStringLiteral("Planar vector overlay"));
    CHECK(opacity->value() == Catch::Approx(75.0));
    CHECK(offset->value() == Catch::Approx(12.0));
    CHECK_FALSE(warning->isHidden());
    CHECK(features->text() == QStringLiteral("17"));
    CHECK(driver->text() == QStringLiteral("GPKG"));
    CHECK(crs->text() == QStringLiteral("EPSG:3006"));

    opacity->setValue(40.0);
    REQUIRE(changed.has_value());
    CHECK(changed->first == pci::SceneLayerId{42});
    CHECK(changed->second.opacity == Catch::Approx(0.4F));

    changed.reset();
    onTop->setChecked(true);
    REQUIRE(changed.has_value());
    CHECK(changed->second.alwaysOnTop);

    changed.reset();
    shape->setCurrentIndex(1);
    REQUIRE(changed.has_value());
    CHECK(changed->second.markerShape == pci::VectorMarkerShape::Square);

    std::optional<pci::SceneLayerId> shown;
    QObject::connect(&panel,
                     &pci::LayerInspectorDock::showVectorAnywayRequested,
                     [&shown](const pci::SceneLayerId id) {
                         shown = id;
                     });
    showAnyway->click();
    CHECK(shown == pci::SceneLayerId{42});
}

TEST_CASE("task rows expose vector retry without a context menu",
          "[ui][vector][tasks]")
{
    pci::TaskDock panel;
    std::optional<pci::LoadJobKey> retried;
    QObject::connect(
        &panel,
        &pci::TaskDock::jobActionRequested,
        [&retried](const pci::LoadJobKey key, const pci::LoadJobAction action) {
            if (action == pci::LoadJobAction::Retry) {
                retried = key;
            }
        });
    panel.setRows({
        {.key = {.kind = pci::LoadJobKind::Vector, .id = pci::LoadJobId{7}},
         .title = QStringLiteral("Vector import"),
         .detail = QStringLiteral(
             "Loaded 2 of 3 · 1 failed — Retry failed sublayers"),
         .completion = 1.0,
         .terminal = true,
         .capabilities = {.canRetry = true, .canDismiss = true}},
    });

    auto *tasks = &panel;
    auto *list = tasks->findChild<QListView *>(
        QStringLiteral("pointCloudLoadStateList"));
    auto *retry =
        tasks->findChild<QPushButton *>(QStringLiteral("retryLoadTaskButton"));
    auto *progress =
        tasks->findChild<QProgressBar *>(QStringLiteral("loadTaskProgressBar"));
    REQUIRE(list != nullptr);
    REQUIRE(retry != nullptr);
    REQUIRE(progress != nullptr);
    CHECK(list->model()->rowCount() == 1);
    CHECK(
        retry->toolTip().contains(QStringLiteral("failed or not-yet-loaded")));
    CHECK(progress->value() == progress->maximum());

    retry->click();
    REQUIRE(retried.has_value());
    CHECK(retried->kind == pci::LoadJobKind::Vector);
    CHECK(retried->id == pci::LoadJobId{7});
}

TEST_CASE("task row buttons preserve every job kind identity", "[ui][tasks]")
{
    constexpr std::array kinds{pci::LoadJobKind::PointCloud,
                               pci::LoadJobKind::Vector,
                               pci::LoadJobKind::Raster,
                               pci::LoadJobKind::Colorize};
    for (const pci::LoadJobKind kind : kinds) {
        pci::TaskDock panel;
        const pci::LoadJobKey expected{.kind = kind, .id = pci::LoadJobId{41}};
        std::optional<pci::LoadJobKey> cancelled;
        std::optional<pci::LoadJobKey> retried;
        std::optional<pci::LoadJobKey> prioritized;
        std::optional<pci::LoadJobKey> dismissed;
        QObject::connect(
            &panel,
            &pci::TaskDock::jobActionRequested,
            [&](const pci::LoadJobKey key, const pci::LoadJobAction action) {
                switch (action) {
                case pci::LoadJobAction::Cancel:
                    cancelled = key;
                    break;
                case pci::LoadJobAction::Retry:
                    retried = key;
                    break;
                case pci::LoadJobAction::Prioritize:
                    prioritized = key;
                    break;
                case pci::LoadJobAction::Dismiss:
                    dismissed = key;
                    break;
                }
            });
        panel.setRows({
            {.key = expected,
             .title = QStringLiteral("Import"),
             .detail = QStringLiteral("Queued"),
             .completion = 0.25,
             .terminal = false,
             .capabilities = {.canCancel = true,
                              .canRetry = true,
                              .canPrioritize = true,
                              .canDismiss = true}},
        });

        auto *tasks = &panel;
        const auto click = [tasks](const char *objectName) {
            auto *button = tasks->findChild<QPushButton *>(objectName);
            REQUIRE(button != nullptr);
            button->click();
        };
        click("cancelLoadTaskButton");
        click("retryLoadTaskButton");
        click("prioritizeLoadTaskButton");
        click("dismissLoadTaskButton");

        CAPTURE(kind);
        CHECK(cancelled == expected);
        CHECK(retried == expected);
        CHECK(prioritized == expected);
        CHECK(dismissed == expected);
    }
}

TEST_CASE("task row context menu preserves every job kind identity",
          "[ui][tasks]")
{
    constexpr std::array kinds{pci::LoadJobKind::PointCloud,
                               pci::LoadJobKind::Vector,
                               pci::LoadJobKind::Raster,
                               pci::LoadJobKind::Colorize};
    for (const pci::LoadJobKind kind : kinds) {
        pci::TaskDock panel;
        const pci::LoadJobKey expected{.kind = kind, .id = pci::LoadJobId{73}};
        std::optional<pci::LoadJobKey> cancelled;
        std::optional<pci::LoadJobKey> retried;
        std::optional<pci::LoadJobKey> prioritized;
        std::optional<pci::LoadJobKey> dismissed;
        QObject::connect(
            &panel,
            &pci::TaskDock::jobActionRequested,
            [&](const pci::LoadJobKey key, const pci::LoadJobAction action) {
                switch (action) {
                case pci::LoadJobAction::Cancel:
                    cancelled = key;
                    break;
                case pci::LoadJobAction::Retry:
                    retried = key;
                    break;
                case pci::LoadJobAction::Prioritize:
                    prioritized = key;
                    break;
                case pci::LoadJobAction::Dismiss:
                    dismissed = key;
                    break;
                }
            });
        panel.setRows({
            {.key = expected,
             .title = QStringLiteral("Import"),
             .detail = QStringLiteral("Queued"),
             .completion = 0.25,
             .terminal = false,
             .capabilities = {.canCancel = true,
                              .canRetry = true,
                              .canPrioritize = true,
                              .canDismiss = true}},
        });

        auto *tasks = &panel;
        auto *list = tasks->findChild<QListView *>(
            QStringLiteral("pointCloudLoadStateList"));
        REQUIRE(list != nullptr);
        const QModelIndex item = list->model()->index(0, 0);
        REQUIRE(item.isValid());
        const QPoint itemPosition = list->visualRect(item).center();

        const auto choose = [list, itemPosition](const QString &label) {
            bool foundAction = false;
            QTimer::singleShot(0, [&foundAction, label] {
                auto *menu =
                    qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) {
                    return;
                }
                const QList<QAction *> actions = menu->actions();
                const auto found =
                    std::ranges::find(actions, label, &QAction::text);
                if (found != actions.end()) {
                    foundAction = true;
                    QTest::mouseClick(menu,
                                      Qt::LeftButton,
                                      Qt::NoModifier,
                                      menu->actionGeometry(*found).center());
                } else {
                    menu->close();
                }
            });
            list->customContextMenuRequested(itemPosition);
            CHECK(foundAction);
        };

        choose(QStringLiteral("Cancel"));
        choose(QStringLiteral("Retry"));
        choose(QStringLiteral("Prioritize"));
        choose(QStringLiteral("Dismiss"));

        CAPTURE(kind);
        CHECK(cancelled == expected);
        CHECK(retried == expected);
        CHECK(prioritized == expected);
        CHECK(dismissed == expected);
    }
}

TEST_CASE("vector placement offers scene-relative actions",
          "[ui][vector][inspector][placement]")
{
    pci::LayerInspectorDock panel;
    pci::PointCloudMetadata metadata;
    metadata.sourcePath = "terrain.laz";
    metadata.sourceBounds = {
        .minimum = {0.0, 0.0, 100.0},
        .maximum = {100.0, 100.0, 200.0},
    };
    auto scene = std::make_shared<pci::PointCloudScene>(metadata);
    const std::vector points{pci::PointCloudLayer{
        .id = pci::SceneLayerId{1}, .scene = scene, .visible = true}};

    auto data = std::make_shared<pci::VectorLayerData>();
    data->sublayerName = "Parcels";
    data->bounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {10.0, 10.0, 0.0},
    };
    const std::vector vectors{pci::VectorLayer{
        .id = pci::SceneLayerId{2}, .data = std::move(data), .visible = true}};
    panel.setDocumentSnapshot(
        makeSnapshot(
            points, vectors, {pci::SceneLayerId{1}, pci::SceneLayerId{2}}),
        pci::SceneLayerId{2});

    std::optional<pci::VectorLayerStyle> changed;
    QObject::connect(
        &panel,
        &pci::LayerInspectorDock::vectorStyleChanged,
        [&changed](const pci::SceneLayerId, const pci::VectorLayerStyle style) {
            changed = style;
        });
    auto *inspector = &panel;
    auto *notice =
        inspector->findChild<QLabel *>(QStringLiteral("vectorPlacementNotice"));
    auto *above = inspector->findChild<QPushButton *>(
        QStringLiteral("vectorPlaceAboveSceneButton"));
    auto *floor = inspector->findChild<QPushButton *>(
        QStringLiteral("vectorMatchFloorButton"));
    REQUIRE(notice != nullptr);
    REQUIRE(above != nullptr);
    REQUIRE(floor != nullptr);
    CHECK(notice->text().contains(QStringLiteral("below")));
    CHECK(above->isEnabled());
    above->click();
    REQUIRE(changed.has_value());
    CHECK(changed->zOffset == Catch::Approx(201.0));

    changed.reset();
    floor->click();
    REQUIRE(changed.has_value());
    CHECK(changed->zOffset == Catch::Approx(100.0));
}

TEST_CASE(
    "main window loads a selected file batch in parallel, preserving order",
    "[ui][mainwindow]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    auto *openAction =
        window.findChild<QAction *>(QStringLiteral("openFilesAction"));
    REQUIRE(openAction != nullptr);

    window.loadPointClouds({"first.las", "second.las", "third.las"},
                           pci::PointCloudLoadMode::Replace);

    // The files decode concurrently and are admitted as layers in the given
    // order without waiting for one another's first frame.
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 3;
    }));
    CHECK_FALSE(openAction->isEnabled());
    CHECK(viewportPointer->documentSetCount() == 1);

    const auto layers = viewportPointer->document()->pointLayers();
    REQUIRE(layers.size() == 3);
    CHECK(layers[0].scene->metadata().sourcePath ==
          std::filesystem::path("first.las"));
    CHECK(layers[1].scene->metadata().sourcePath ==
          std::filesystem::path("second.las"));
    CHECK(layers[2].scene->metadata().sourcePath ==
          std::filesystem::path("third.las"));

    // The batch finishes only once every layer reaches the renderer's final
    // display mode; a coarse preview alone is not completion.
    for (const auto &layer : layers) {
        viewportPointer->emitDisplayReady(layer.id);
    }
    REQUIRE(waitFor([&] {
        return openAction->isEnabled();
    }));
    CHECK(viewportPointer->document()->layerCount() == 3);
#ifndef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    auto *activity =
        window.findChild<QWidget *>(QStringLiteral("pointCloudLoadActivity"));
    REQUIRE(activity != nullptr);
    CHECK(activity->isHidden());
#endif
}

TEST_CASE("replace batch reframes the camera as later sources are admitted",
          "[ui][mainwindow][batch][camera]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    window.loadPointClouds({"first.las", "second.las", "third.las"},
                           pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 3;
    }));

    // The first admission frames through setDocument(). Sources admitted after
    // it must re-frame the grown extent; otherwise they stay outside the
    // frustum, receive no uploads, and never become display-ready.
    CHECK(viewportPointer->documentSetCount() == 1);
    CHECK(viewportPointer->lastDocumentWasFramed());
    CHECK(viewportPointer->frameVisibleLayersCount() == 2);
}

TEST_CASE("add batch leaves the camera where the user put it",
          "[ui][mainwindow][batch][camera]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    auto *openAction =
        window.findChild<QAction *>(QStringLiteral("openFilesAction"));
    REQUIRE(openAction != nullptr);

    window.loadPointClouds({"first.las"}, pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 1;
    }));
    // The first load must finish before another can start.
    for (const auto &layer : viewportPointer->document()->pointLayers()) {
        viewportPointer->emitDisplayReady(layer.id);
    }
    REQUIRE(waitFor([&] {
        return openAction->isEnabled();
    }));
    const int framedAfterReplace = viewportPointer->frameVisibleLayersCount();

    window.loadPointClouds({"second.las", "third.las"},
                           pci::PointCloudLoadMode::Add);
    REQUIRE(waitFor([&] {
        return viewportPointer->document()->layerCount() == 3;
    }));

    // Adding to an existing view must not yank the camera; off-screen additions
    // rely on the display-readiness frustum exemption instead.
    CHECK(viewportPointer->frameVisibleLayersCount() == framedAfterReplace);
}

TEST_CASE("batch loading progress runs monotonically from zero to one hundred",
          "[ui][mainwindow][batch][progress]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    auto *overlay =
        window.findChild<QWidget *>(QStringLiteral("loadingOverlay"));
    auto *bar =
        window.findChild<QProgressBar *>(QStringLiteral("loadingProgressBar"));
    REQUIRE(overlay != nullptr);
    REQUIRE(bar != nullptr);

    window.loadPointClouds({"first.las", "second.las"},
                           pci::PointCloudLoadMode::Replace);
    CHECK_FALSE(overlay->isHidden());
    CHECK(bar->value() == 0);

    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 2 &&
               bar->value() >= 70;
    }));
    std::vector<int> percentages{0, bar->value()};
    CHECK(bar->value() <= 75);

    const auto layers = viewportPointer->document()->pointLayers();
    for (const auto &layer : layers) {
        viewportPointer->emitLoadProgress({
            .layerId = layer.id,
            .stage = pci::RenderLoadStage::FullDetailWarming,
            .completed = 5,
            .total = 10,
            .decoded = 8,
            .uploaded = 5,
        });
        percentages.push_back(bar->value());
    }
    CHECK(bar->value() >= 87);
    CHECK(bar->value() < 100);

    viewportPointer->emitDisplayReady(layers.front().id, 10);
    percentages.push_back(bar->value());
    CHECK(bar->value() >= 93);
    CHECK(bar->value() < 100);

    viewportPointer->emitDisplayReady(layers.back().id, 10);
    percentages.push_back(bar->value());
    CHECK(bar->value() == 100);
    CHECK_FALSE(overlay->isHidden());
    CHECK(std::ranges::is_sorted(percentages));
    REQUIRE(waitFor([&] {
        return overlay->isHidden();
    }));
}

TEST_CASE("cached paged batch progress starts with measured residency work",
          "[ui][mainwindow][batch][progress][paging]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services =
        makeTestImportServices(std::make_shared<CachedPagedLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    auto *bar =
        window.findChild<QProgressBar *>(QStringLiteral("loadingProgressBar"));
    REQUIRE(bar != nullptr);
    window.loadPointClouds({"first.laz", "second.laz"},
                           pci::PointCloudLoadMode::Replace);
    CHECK(bar->value() == 0);

    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 2 &&
               bar->value() > 0;
    }));
    CHECK(bar->value() <= 5);

    const auto layers = viewportPointer->document()->pointLayers();
    for (const auto &layer : layers) {
        viewportPointer->emitLoadProgress({
            .layerId = layer.id,
            .stage = pci::RenderLoadStage::Uploading,
            .completed = 1,
            .total = 10,
        });
        viewportPointer->emitLoadProgress({
            .layerId = layer.id,
            .stage = pci::RenderLoadStage::FirstFrameReady,
            .completed = 1,
            .total = 10,
        });
    }
    CHECK(bar->value() >= 9);
    CHECK(bar->value() < 20);

    for (const auto &layer : layers) {
        viewportPointer->emitLoadProgress({
            .layerId = layer.id,
            .stage = pci::RenderLoadStage::FullDetailWarming,
            .completed = 5,
            .total = 10,
            .decoded = 5,
            .uploaded = 5,
        });
    }
    CHECK(bar->value() >= 52);
    CHECK(bar->value() < 60);

    for (const auto &layer : layers) {
        viewportPointer->emitDisplayReady(layer.id, 10);
    }
    CHECK(bar->value() == 100);
}

TEST_CASE("later batch previews publish without waiting for a slow first file",
          "[ui][mainwindow][multi-file][progressive]")
{
    auto loader = std::make_shared<OutOfOrderPreviewLoader>();
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(loader);
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    window.loadPointClouds({"slow-first.las", "fast-second.las"},
                           pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&] {
        return loader->slowStarted.load();
    }));
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 1;
    }));
    CHECK(viewportPointer->document()
              ->pointLayers()
              .front()
              .scene->metadata()
              .sourcePath.filename() ==
          std::filesystem::path("fast-second.las"));

    loader->releaseSlow = true;
    REQUIRE(waitFor([&] {
        return viewportPointer->document()->layerCount() == 2;
    }));
    const auto layers = viewportPointer->document()->pointLayers();
    CHECK(layers[0].scene->metadata().sourcePath.filename() ==
          std::filesystem::path("slow-first.las"));
    CHECK(layers[1].scene->metadata().sourcePath.filename() ==
          std::filesystem::path("fast-second.las"));
    for (const auto &layer : layers) {
        viewportPointer->emitDisplayReady(layer.id, 1);
    }
}

TEST_CASE("main window keeps replacements transactional and removes cancelled "
          "additions",
          "[ui][mainwindow]")
{
    auto loader = std::make_shared<FirstImmediateThenGatedLoader>();
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(loader);
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    auto *openAction =
        window.findChild<QAction *>(QStringLiteral("openFilesAction"));
    auto *cancel =
        window.findChild<QPushButton *>(QStringLiteral("loadingCancelButton"));
    REQUIRE(openAction != nullptr);
    REQUIRE(cancel != nullptr);

    window.loadPointCloud("first.las");
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 1;
    }));
    const auto originalDocument = viewportPointer->document();
    const auto originalLayer = originalDocument->pointLayers().front();
    viewportPointer->emitDisplayReady(originalLayer.id);
    REQUIRE(waitFor([&] {
        return openAction->isEnabled();
    }));

    window.loadPointCloud("pending.las", pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&] {
        return loader->gatedLoads.load() == 1;
    }));
    // The replacement shell is admitted immediately so immutable chunks can
    // render before EOF; cancellation must still restore the old document.
    REQUIRE(waitFor([&] {
        return viewportPointer->document() != originalDocument;
    }));
    CHECK(viewportPointer->document()->layerCount() == 1);
    CHECK_FALSE(openAction->isEnabled());
    cancel->click();
    REQUIRE(waitFor([&] {
        return openAction->isEnabled();
    }));
    CHECK(viewportPointer->document() == originalDocument);
    CHECK(viewportPointer->document()
              ->pointLayers()
              .front()
              .scene->metadata()
              .sourcePath == std::filesystem::path("first.las"));

    loader->allowGatedCompletion = false;
    window.loadPointCloud("pending.las", pci::PointCloudLoadMode::Add);
    REQUIRE(waitFor([&] {
        return loader->gatedLoads.load() == 2 &&
               viewportPointer->document()->layerCount() == 2;
    }));
    cancel->click();
    REQUIRE(waitFor([&] {
        return openAction->isEnabled() &&
               viewportPointer->document()->layerCount() == 1;
    }));
    const auto afterCancelledAddition = viewportPointer->document();

    loader->allowGatedCompletion = true;
    window.loadPointCloud("pending.las", pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&] {
        return viewportPointer->document() != afterCancelledAddition &&
               viewportPointer->document()->layerCount() == 1;
    }));
    CHECK(viewportPointer->lastDocumentWasFramed());
    const auto replacementLayer =
        viewportPointer->document()->pointLayers().front();
    viewportPointer->emitDisplayReady(replacementLayer.id);
    REQUIRE(waitFor([&] {
        return openAction->isEnabled();
    }));
}

TEST_CASE("replacement failure after preview restores the previous document",
          "[ui][mainwindow][scene-session-characterization][rollback]")
{
    auto loader = std::make_shared<FirstImmediateThenPreviewFailureLoader>();
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(loader);
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    window.loadPointCloud("original.las");
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 1;
    }));
    const auto original = viewportPointer->document();
    viewportPointer->emitDisplayReady(original->pointLayers().front().id);

    window.loadPointCloud("broken-after-preview.las",
                          pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&] {
        return loader->previewPublished.load() &&
               viewportPointer->document() != original;
    }));
    REQUIRE(viewportPointer->document()->layerCount() == 1);
    CHECK(viewportPointer->document()
              ->pointLayers()
              .front()
              .scene->metadata()
              .sourcePath.filename() ==
          std::filesystem::path("broken-after-preview.las"));

    loader->releaseFailure = true;
    REQUIRE(waitFor([&] {
        return viewportPointer->document() == original &&
               window.statusBar()->currentMessage().contains(
                   QStringLiteral("Loading failed"));
    }));
}

TEST_CASE("main window keeps loading visible until final display readiness",
          "[ui][mainwindow]")
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    window.loadPointCloud("fixture.las");
    auto *overlay =
        window.findChild<QWidget *>(QStringLiteral("loadingOverlay"));
    auto *bar =
        window.findChild<QProgressBar *>(QStringLiteral("loadingProgressBar"));
    auto *details =
        window.findChild<QLabel *>(QStringLiteral("loadingDetailsLabel"));
    REQUIRE(overlay != nullptr);
    REQUIRE(bar != nullptr);
    REQUIRE(details != nullptr);
    REQUIRE(waitFor([&] {
        return viewportPointer->totalPointCount() == 3 &&
               !overlay->isHidden()
               // Renderer progress is deliberately deferred until the final
               // queued source-progress event has completed the read phase.
               && bar->value() >= 70 && bar->value() < 100;
    }));

    const auto layer = viewportPointer->document()->pointLayers().front();
    viewportPointer->emitLoadProgress({
        .layerId = layer.id,
        .stage = pci::RenderLoadStage::Uploading,
        .completed = 1,
        .total = 10,
    });
    CHECK(bar->value() == 77);
    viewportPointer->emitLoadProgress({
        .layerId = layer.id,
        .stage = pci::RenderLoadStage::FirstFrameReady,
        .completed = 3,
        .total = 3,
    });
    CHECK_FALSE(overlay->isHidden());

    viewportPointer->emitLoadProgress({
        .layerId = layer.id,
        .stage = pci::RenderLoadStage::FullDetailWarming,
        .completed = 4,
        .total = 10,
        .decoded = 7,
        .uploaded = 4,
    });
    CHECK(bar->value() == 84);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    CHECK(details->text().contains(QStringLiteral("7 / 10 decoded")));
    CHECK(details->text().contains(QStringLiteral("4 / 10 uploaded")));
#else
    CHECK(details->text() == QStringLiteral("Preparing point cloud…"));
#endif
    CHECK_FALSE(overlay->isHidden());

    viewportPointer->emitLoadProgress({
        .layerId = layer.id,
        .stage = pci::RenderLoadStage::DisplayReady,
        .completed = 10,
        .total = 10,
        .decoded = 10,
        .uploaded = 10,
    });

    REQUIRE(waitFor([&] {
        return overlay->isHidden();
    }));
    // The final count arrives with the queued `loaded` signal, which may
    // trail the first rendered frame during progressive loads.
    REQUIRE(waitFor([&] {
        return window.statusBar()->currentMessage().contains(
            QStringLiteral("3 points"));
    }));
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    CHECK(window.statusBar()->currentMessage().contains(
        QStringLiteral("first points")));
    viewportPointer->emitMetrics({
        .deviceName = QStringLiteral("Fake GPU"),
        .timingSource = QStringLiteral("CPU"),
    });
    CHECK(window.statusBar()->currentMessage().contains(
        QStringLiteral("First ")));
    CHECK_FALSE(window.statusBar()->currentMessage().contains(
        QStringLiteral("First -")));
#else
    const QString cleanStatus = window.statusBar()->currentMessage();
    viewportPointer->emitMetrics({
        .deviceName = QStringLiteral("Fake GPU"),
        .timingSource = QStringLiteral("CPU"),
    });
    CHECK(window.statusBar()->currentMessage() == cleanStatus);
    CHECK_FALSE(cleanStatus.contains(QStringLiteral("first points")));
    CHECK_FALSE(cleanStatus.contains(QStringLiteral("FPS")));
#endif
}

TEST_CASE("main window cancellation updates the overlay and status",
          "[ui][mainwindow]")
{
    auto loader = std::make_shared<SlowLoader>();
    auto services = makeTestImportServices(loader);
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.show();

    window.loadPointCloud("fixture.las");
    REQUIRE(waitFor([&] {
        return loader->started.load();
    }));
    REQUIRE(viewportPointer->document());
    CHECK_FALSE(viewportPointer->document()->hasAnyLayer());

    auto *overlay =
        window.findChild<QWidget *>(QStringLiteral("loadingOverlay"));
    auto *bar =
        window.findChild<QProgressBar *>(QStringLiteral("loadingProgressBar"));
    auto *cancel =
        window.findChild<QPushButton *>(QStringLiteral("loadingCancelButton"));
    REQUIRE(overlay != nullptr);
    REQUIRE(bar != nullptr);
    REQUIRE(cancel != nullptr);
    REQUIRE(waitFor([&] {
        return bar->value() == 35;
    }));
    CHECK_FALSE(overlay->isHidden());
    CHECK(cancel->isEnabled());

    cancel->click();
    CHECK_FALSE(cancel->isEnabled());
    REQUIRE(waitFor([&] {
        return overlay->isHidden();
    }));
    REQUIRE(viewportPointer->document());
    CHECK_FALSE(viewportPointer->document()->hasAnyLayer());
    CHECK(window.statusBar()->currentMessage().contains(
        QStringLiteral("cancelled")));
}

TEST_CASE("main window hides loading UI after import failure",
          "[ui][mainwindow]")
{
    auto services = makeTestImportServices(std::make_shared<FailingLoader>());
    pci::MainWindow window(
        std::make_unique<FakeViewport>(), std::move(services), 100);
    window.show();

    window.loadPointCloud("broken.las");
    auto *overlay =
        window.findChild<QWidget *>(QStringLiteral("loadingOverlay"));
    REQUIRE(overlay != nullptr);
    REQUIRE(waitFor([&] {
        return overlay->isHidden() &&
               window.statusBar()->currentMessage().contains(
                   QStringLiteral("Loading failed"));
    }));
    auto *states = window.findChild<QListView *>(
        QStringLiteral("pointCloudLoadStateList"));
    REQUIRE(states != nullptr);
    REQUIRE(states->model()->rowCount() == 1);
    CHECK(states->model()
              ->index(0, 0)
              .data(Qt::ToolTipRole)
              .toString()
              .contains(QStringLiteral("fixture failure")));
}

#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
TEST_CASE("main window archives native Release H metrics",
          "[ui][mainwindow][metrics][qualification]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);
    window.setGdalCacheControls(pci::GdalCacheControls{
        .setByteBudget = [](std::uint64_t) {},
        .usedBytes =
            [] {
                return std::uint64_t{512};
            },
    });
    const std::filesystem::path report =
        pci::qStringToPath(directory.path()) / "report.json";
    window.configureQualificationReport(report);
    window.setGdalRuntimeInfo(pci::GdalRuntimeInfo{
        .version = QStringLiteral("3.9.0"),
        .tileIndexDriver = true,
        .virtualRasterDriver = true,
        .geoPackageDriver = false,
        .flatGeobufDriver = true,
        .shapefileDriver = false,
        .probed = true,
    });
    window.show();

    window.loadPointCloud("qualified.las");
    static_cast<void>(window.importRasterLayer(
        pci::RasterImportRequest{.sourcePath = "catalog.gti.gpkg"}));
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 1 &&
               viewportPointer->document()->rasterLayerCount() == 1;
    }));
    viewportPointer->emitMetrics({
        .deviceName = QStringLiteral("Qualification GPU"),
        .requestedBackend = QStringLiteral("auto"),
        .selectedBackend = QStringLiteral("TestRhi"),
        .gpuValidationEnabled = true,
        .frameMilliseconds = 7.5,
        .drawCalls = 4,
        .sourcePoints = 100,
        .submittedPoints = 100,
        .fullDetailActive = true,
        .fullDetailDecodedNodes = 3,
        .fullDetailTotalNodes = 3,
        .decodedPointBytes = 1024,
        .decodedPointBudgetBytes = 4096,
        .peakDecodedPointBytes = 2048,
        .gpuPointBudgetBytes = 8192,
        .gpuPointBytes = 2048,
        .peakGpuPointBytes = 4096,
        .visibleLayerCount = 1,
        .coveredLayerCount = 1,
        .processResidentBytes = 16'384,
        .peakProcessResidentBytes = 32'768,
        .rasterCpuBytes = 2048,
        .rasterCpuBudgetBytes = 8192,
        .rasterCpuPeakBytes = 4096,
        .rasterGpuBytes = 1024,
        .rasterGpuBudgetBytes = 4096,
        .rasterGpuPeakBytes = 2048,
        .rasterTilesRequested = 9,
        .rasterTilesCompleted = 8,
        .rasterResidentTiles = 7,
        .rasterFinestLevel = 1,
        .rasterCoarsestLevel = 2,
    });
    viewportPointer->emitDisplayReady(
        viewportPointer->document()->pointLayers().front().id);
    REQUIRE(waitFor([&] {
        return std::filesystem::exists(report);
    }));

    QFile input(pci::pathToQString(report));
    REQUIRE(input.open(QIODevice::ReadOnly));
    const QJsonObject json = QJsonDocument::fromJson(input.readAll()).object();
    CHECK(json.value(QStringLiteral("schema")).toString() ==
          QStringLiteral("pcinspector.release-h.native.v1"));
    CHECK(json.value(QStringLiteral("gpu_device")).toString() ==
          QStringLiteral("Qualification GPU"));
    CHECK(json.value(QStringLiteral("gpu_peak_bytes")).toInteger() == 4096);
    CHECK(json.value(QStringLiteral("covered_sources")).toInteger() == 1);
    CHECK(json.value(QStringLiteral("draw_calls")).toInteger() == 4);
    CHECK(json.value(QStringLiteral("submitted_points")).toInteger() == 100);
    CHECK(json.value(QStringLiteral("full_detail_active")).toBool());
    const QJsonObject source =
        json.value(QStringLiteral("sources")).toArray().first().toObject();
    CHECK(source.value(QStringLiteral("job_id")).isDouble());
    CHECK(source.value(QStringLiteral("job_id")).toInteger() == 1);
    const QJsonObject layer =
        json.value(QStringLiteral("layers")).toArray().first().toObject();
    CHECK(layer.value(QStringLiteral("layer_id")).isDouble());
    // Point and raster layers share one id space, so the reported id is
    // checked against the document rather than against a fixed number.
    CHECK(layer.value(QStringLiteral("layer_id")).toInteger() ==
          static_cast<qint64>(
              viewportPointer->document()->pointLayers().front().id.value()));
    CHECK(json.value(QStringLiteral("display_ready_ms")).toDouble() >= 0.0);
    CHECK(json.value(QStringLiteral("frame_ms_p95")).toDouble() ==
          Catch::Approx(7.5));

    // A bug report has to distinguish "this build has no GTI" from "GTI is
    // present and the catalog is broken", so drivers are reported one by one
    // rather than collapsed into a single capability bit.
    CHECK(json.value(QStringLiteral("gdal_probed")).toBool());
    CHECK(json.value(QStringLiteral("gdal_version")).toString() ==
          QStringLiteral("3.9.0"));
    CHECK(json.value(QStringLiteral("gdal_driver_gti")).toBool());
    CHECK(json.value(QStringLiteral("gdal_driver_vrt")).toBool());
    CHECK_FALSE(json.value(QStringLiteral("gdal_driver_gpkg")).toBool());
    CHECK(json.value(QStringLiteral("gdal_driver_flatgeobuf")).toBool());
    // GTI plus one usable index format is enough, even without GPKG.
    CHECK(json.value(QStringLiteral("catalog_import_available")).toBool());
    CHECK(json.value(QStringLiteral("raster_cpu_bytes")).toInteger() == 2048);
    CHECK(json.value(QStringLiteral("raster_cpu_budget_bytes")).toInteger() ==
          8192);
    CHECK(json.value(QStringLiteral("raster_cpu_peak_bytes")).toInteger() ==
          4096);
    CHECK(json.value(QStringLiteral("raster_gpu_bytes")).toInteger() == 1024);
    CHECK(json.value(QStringLiteral("raster_gpu_budget_bytes")).toInteger() ==
          4096);
    CHECK(json.value(QStringLiteral("raster_gpu_peak_bytes")).toInteger() ==
          2048);
    CHECK(json.value(QStringLiteral("gdal_cache_budget_bytes")).toInteger() ==
          128LL * 1024 * 1024);
    CHECK(json.value(QStringLiteral("gdal_cache_used_bytes")).toInteger() ==
          512);
    CHECK(json.value(QStringLiteral("raster_tiles_requested")).toInteger() ==
          9);
    CHECK(json.value(QStringLiteral("raster_finest_level")).toInteger() == 1);

    const QJsonArray rasters =
        json.value(QStringLiteral("raster_layers")).toArray();
    REQUIRE(rasters.size() == 1);
    const QJsonObject raster = rasters.first().toObject();
    CHECK(raster.value(QStringLiteral("layer_id")).isDouble());
    CHECK(raster.value(QStringLiteral("width")).toInteger() > 0);
    CHECK(raster.value(QStringLiteral("levels")).toInteger() >= 1);
    CHECK(raster.value(QStringLiteral("backed_levels")).toInteger() >= 1);
    CHECK(raster.contains(QStringLiteral("generated_levels")));
    // Source quality and automatic preview provenance travel into the report,
    // so a "looks wrong" bug can be triaged without the file.
    CHECK(raster.contains(QStringLiteral("insufficient_overviews")));
    CHECK(raster.contains(QStringLiteral("driver")));
}

#endif

} // namespace

TEST_CASE("main window exposes raster opening through the unified action",
          "[ui][mainwindow][raster]")
{
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(
        std::make_unique<FakeViewport>(), std::move(services), 100);

    auto *open = window.findChild<QAction *>(QStringLiteral("openFilesAction"));
    auto *fileMenu = window.findChild<QMenu *>(QStringLiteral("fileMenu"));
    auto *toolBar =
        window.findChild<QToolBar *>(QStringLiteral("pointCloudToolBar"));

    REQUIRE(open != nullptr);
    REQUIRE(fileMenu != nullptr);
    REQUIRE(toolBar != nullptr);
    CHECK(fileMenu->actions().contains(open));
    CHECK(toolBar->actions().contains(open));
    CHECK(open->shortcut() == QKeySequence::Open);
    CHECK_FALSE(open->icon().isNull());
    CHECK(window.findChild<QAction *>(
              QStringLiteral("importRasterLayerAction")) == nullptr);
}

TEST_CASE("main window publishes imported rasters to the viewport",
          "[ui][mainwindow][raster]")
{
    auto rasterLoader = std::make_shared<StubRasterLoader>();
    auto viewport = std::make_unique<FakeViewport>();
    auto *viewportPointer = viewport.get();
    auto services =
        makeTestImportServices(std::make_shared<ImmediateLoader>(),
                               std::make_shared<DisjointVectorLoader>(),
                               {},
                               rasterLoader);
    pci::MainWindow window(std::move(viewport), std::move(services), 100);

    pci::RasterImportRequest request;
    request.sourcePath = "ortho.tif";
    static_cast<void>(window.importRasterLayer(std::move(request)));

    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->rasterLayerCount() == 1;
    }));
    CHECK(rasterLoader->inspectCalls.load() == 1);
    CHECK(rasterLoader->lastSourcePath.filename() == "ortho.tif");

    const pci::SceneDocumentSnapshotPtr document = viewportPointer->document();
    const pci::RasterLayer layer = document->rasterLayers().front();
    CHECK(layer.visible);
    CHECK(layer.data->metadata().width == 64);
    // Point and vector counts stay in their own domains.
    CHECK(document->layerCount() == 0);
    CHECK(document->vectorLayerCount() == 0);

    // The first layer in an empty document frames the view.
    auto *fit = window.findChild<QAction *>(QStringLiteral("fitSceneAction"));
    REQUIRE(fit != nullptr);
    fit->trigger();
    CHECK(viewportPointer->frameVisibleLayersCount() >= 1);
}

TEST_CASE("main window keeps rasters when point clouds are replaced",
          "[ui][mainwindow][raster]")
{
    auto viewport = std::make_unique<FakeViewport>();
    auto *viewportPointer = viewport.get();
    auto services = makeTestImportServices(std::make_shared<ImmediateLoader>());
    pci::MainWindow window(std::move(viewport), std::move(services), 100);

    static_cast<void>(window.importRasterLayer({.sourcePath = "ortho.tif"}));
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->rasterLayerCount() == 1;
    }));
    const pci::SceneLayerId rasterId =
        viewportPointer->document()->rasterLayers().front().id;

    window.loadPointCloud("first.las", pci::PointCloudLoadMode::Replace);
    REQUIRE(waitFor([&] {
        return viewportPointer->document() &&
               viewportPointer->document()->layerCount() == 1;
    }));

    // Replacing point clouds copies overlays across, and the copy preserves
    // the layer id: renderer caches are keyed by it, so a fresh id would evict
    // and re-upload every overlay.
    const pci::SceneDocumentSnapshotPtr document = viewportPointer->document();
    REQUIRE(document->rasterLayerCount() == 1);
    CHECK(document->rasterLayers().front().id == rasterId);
}
