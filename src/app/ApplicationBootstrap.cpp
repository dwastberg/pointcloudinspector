#include "app/ApplicationBootstrap.h"

#include "app/MainWindow.h"
#include "development/SyntheticScene.h"
#include "import/ImportServices.h"
#include "import/gdal/GdalRasterLoader.h"
#include "import/gdal/GdalRuntime.h"
#include "import/ogr/OgrVectorLoader.h"
#include "import/pdal/PdalPointCloudLoader.h"
#include "import/pdal/PdalPointCloudStatistics.h"
#include "platform/QtPath.h"
#include "platform/SystemMemoryInfo.h"
#include "pointcloud/PointColorMapCatalog.h"
#include "renderer/RenderViewport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>
#include <QTimer>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>

namespace pci {
namespace {

void configurePackagedGeospatialData()
{
    const QDir executableDirectory(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    const QDir dataRoot(
        executableDirectory.filePath(QStringLiteral("../Resources")));
#else
    const QDir dataRoot(
        executableDirectory.filePath(QStringLiteral("../share")));
#endif

    const auto usePackagedDirectory = [&](const char *variable,
                                          const QString &directoryName) {
        if (!qEnvironmentVariableIsEmpty(variable)) {
            return;
        }
        const QString path = dataRoot.filePath(directoryName);
        if (QDir(path).exists()) {
            qputenv(variable, path.toUtf8());
        }
    };
    usePackagedDirectory("GDAL_DATA", QStringLiteral("gdal"));
    usePackagedDirectory("PROJ_DATA", QStringLiteral("proj"));
}

void reportMemoryBudget(const ApplicationConfig &config,
                        const ResolvedMemoryBudget &memoryBudget)
{
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    constexpr double bytesPerGiB = 1024.0 * 1024.0 * 1024.0;
    if (config.cpuBudget.automatic) {
        qInfo().noquote()
            << QStringLiteral(
                   "CPU point budget: %1 GiB (automatic%2; %3 GiB effective "
                   "RAM, %4 GiB currently available, %5 GiB system reserve)")
                   .arg(static_cast<double>(memoryBudget.pointByteBudget) /
                            bytesPerGiB,
                        0,
                        'f',
                        2)
                   .arg(memoryBudget.automaticBudget.usedFallback
                            ? QStringLiteral(" fallback")
                            : QString{})
                   .arg(static_cast<double>(
                            memoryBudget.automaticBudget.effectiveTotalBytes) /
                            bytesPerGiB,
                        0,
                        'f',
                        2)
                   .arg(static_cast<double>(
                            memoryBudget.automaticBudget.availableBytes) /
                            bytesPerGiB,
                        0,
                        'f',
                        2)
                   .arg(static_cast<double>(
                            memoryBudget.automaticBudget.systemReserveBytes) /
                            bytesPerGiB,
                        0,
                        'f',
                        2);
    } else {
        qInfo().noquote()
            << QStringLiteral("CPU point budget: %1 GiB (explicit)")
                   .arg(static_cast<double>(memoryBudget.pointByteBudget) /
                            bytesPerGiB,
                        0,
                        'f',
                        2);
    }
#else
    static_cast<void>(config);
    static_cast<void>(memoryBudget);
#endif
}

std::filesystem::path defaultPointPageCacheDirectory()
{
    return pointPageCacheDirectory(
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation),
        std::filesystem::temp_directory_path());
}

void populateInitialDocument(RenderViewport &viewport,
                             const ApplicationConfig &config,
                             const std::uint64_t decodedByteBudget,
                             const std::filesystem::path &cacheDirectory,
                             const PointColorMapCatalogSnapshotPtr &colorMaps)
{
    if (config.sources.empty()) {
        auto document = std::make_shared<SceneDocument>(
            HierarchyResidencyCoordinator::defaultByteBudget,
            HierarchyResidencyCoordinator::defaultMaximumConcurrentDecodes,
            HierarchyDecodeAdmissionPtr{},
            PointMemoryBudgetPtr{},
            colorMaps);
        if (config.syntheticPointCount) {
            static_cast<void>(document->addLayer(
                buildSyntheticScene(*config.syntheticPointCount)));
        }
        viewport.setDocument(document->snapshot(), true);
        return;
    }
    if (!config.smokeTest) {
        return;
    }

    // Smoke tests exit after a few frames. Load synchronously so content is
    // resident for the first rendered frame and stdout remains deterministic.
    auto document = std::make_shared<SceneDocument>(
        decodedByteBudget,
        HierarchyResidencyCoordinator::defaultMaximumConcurrentDecodes,
        HierarchyDecodeAdmissionPtr{},
        PointMemoryBudgetPtr{},
        colorMaps);
    PdalPointCloudLoader loader;
    for (const std::filesystem::path &sourcePath : config.sources) {
        const PointCloudLoadOptions options{
            .sourcePath = sourcePath,
            .maximumPoints = config.maximumLoadPoints,
            .localPaging =
                {
                    .cacheDirectory = cacheDirectory,
                },
        };
        const PointCloudLoadResources resources{
            .decodedByteBudget = decodedByteBudget,
            .residency = document->residencyCoordinator(),
            .memoryBudget = {},
            .flatReservation = {},
        };
        const PointCloudImportPreflight preflight = loader.inspect(
            options, resources.decodedByteBudget, std::stop_token{});
        PointCloudScenePtr scene =
            loader.load(options, resources, preflight, PointCloudLoadContext{});
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
        qInfo().noquote() << QStringLiteral("Loaded %1: %2 / %3 points")
                                 .arg(pathToQString(sourcePath))
                                 .arg(scene->totalPointCount())
                                 .arg(scene->metadata().sourcePointCount);
#endif
        static_cast<void>(document->addLayer(std::move(scene)));
    }
    viewport.setDocument(document->snapshot(), true);
}

ImportServices createImportServices()
{
    ImportServices services;
    services.scheduler = std::make_unique<TaskScheduler>();
    services.pointCloud = std::make_unique<PointCloudLoadController>(
        std::make_shared<PdalPointCloudLoader>(), *services.scheduler);
    services.vector = std::make_unique<VectorLoadController>(
        std::make_shared<OgrVectorLoader>(), *services.scheduler);
    // Inspection and bounded range sampling only. Tile streaming will own its
    // own workers so interactive imagery is not starved by point imports.
    services.raster = std::make_unique<RasterLoadController>(
        std::make_shared<GdalRasterLoader>(), *services.scheduler);
    services.statistics = std::make_shared<PdalPointCloudStatistics>();
    return services;
}

} // namespace

std::unique_ptr<MainWindow>
bootstrapApplication(const ApplicationConfig &config,
                     PointColorMapCatalogSnapshotPtr colorMaps)
{
    configurePackagedGeospatialData();
    if (!colorMaps) {
        throw std::invalid_argument(
            "application bootstrap requires a color-map catalog");
    }
    // GDAL's block cache defaults to 5% of physical RAM, is process-global,
    // and is invisible to the application's own byte accounting. Setting it
    // explicitly is what makes the documented memory envelope true rather than
    // aspirational.
    setGdalBlockCacheBytes(mebibytesToBytes(config.raster.gdalCacheMebibytes));

    const ResolvedMemoryBudget memoryBudget =
        resolveMemoryBudget(config, systemMemoryInfo());
    reportMemoryBudget(config, memoryBudget);
    const std::filesystem::path cacheDirectory =
        defaultPointPageCacheDirectory();

    std::unique_ptr<RenderViewport> viewport =
        createRenderViewport(config.smokeTest,
                             config.gpuByteBudget,
                             config.graphicsApi,
                             config.gpuValidation,
                             colorMaps);
    populateInitialDocument(*viewport,
                            config,
                            memoryBudget.pointByteBudget,
                            cacheDirectory,
                            colorMaps);

    auto window = std::make_unique<MainWindow>(std::move(viewport),
                                               createImportServices(),
                                               config.maximumLoadPoints,
                                               memoryBudget.pointByteBudget,
                                               memoryBudget.automaticParameters,
                                               cacheDirectory,
                                               colorMaps);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    if (config.qualification) {
        window->configureQualificationReport(
            config.qualification->reportPath,
            config.qualification->exitAfterWrite);
    }
#endif
    window->resize(1280, 800);
    window->show();
    if (!config.sources.empty() && !config.smokeTest) {
        QTimer::singleShot(
            0, window.get(), [window = window.get(), sources = config.sources] {
                window->loadPointClouds(sources, PointCloudLoadMode::Replace);
            });
    }
    return window;
}

} // namespace pci
