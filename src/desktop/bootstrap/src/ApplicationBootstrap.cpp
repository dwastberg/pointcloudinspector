#include <pci/desktop/bootstrap/ApplicationBootstrap.h>

#include <pci/adapters/gdal/GdalRasterLoader.h>
#include <pci/adapters/gdal/GdalSpatialReferenceComparator.h>
#include <pci/adapters/gdal/runtime/GdalRuntime.h>
#include <pci/adapters/ogr/OgrVectorLoader.h>
#include <pci/adapters/pdal/PdalPointCloudLoader.h>
#include <pci/adapters/pdal/PdalPointCloudStatistics.h>
#include <pci/adapters/platform/QtPath.h>
#include <pci/adapters/platform/SystemMemoryInfo.h>
#include <pci/adapters/storage/ManagedStorage.h>
#include <pci/adapters/storage/SecureStorage.h>
#include <pci/color/PointColorMapCatalog.h>
#include <pci/desktop/config/SupportedSource.h>
#include <pci/desktop/operations/ImportServices.h>
#include <pci/desktop/ui/MainWindow.h>
#include <pci/desktop/viewport/RenderViewport.h>
#include <pci/development/SyntheticScene.h>
#include <pci/operations/PointDatasetInstallation.h>
#include <pci/operations/local/LocalStorageMaintenance.h>
#include <pci/runtime/scene/SceneRuntime.h>

#include <pci/operations/local/LocalRasterColorizeRunStore.h>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>

namespace pci {
namespace {

PointCloudLayerId addInitialPointLayer(SceneRuntime &runtime,
                                       SceneDocument &document,
                                       const PointDatasetRuntimePtr &scene)
{
    const BindingGeneration binding =
        nextGeneration(document.lastBindingGeneration());
    if (!scene || !runtime.attachPoint({.descriptor = scene->descriptor(),
                                        .runtime = scene,
                                        .generation = binding})) {
        throw std::logic_error("initial point runtime attachment failed");
    }
    try {
        return document.addLayer(scene->datasetView(), binding);
    } catch (...) {
        static_cast<void>(runtime.detach(binding));
        throw;
    }
}

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

LocalPageCacheContextPtr
defaultPointPageCache(const std::filesystem::path &workingDirectory)
{
    const std::filesystem::path cacheDirectory = pointPageCacheDirectory(
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    const std::filesystem::path configurationDirectory =
        pointPageCacheConfigurationDirectory(QStandardPaths::writableLocation(
            QStandardPaths::AppConfigLocation));
    if (!cacheDirectory.empty() && !configurationDirectory.empty()) {
        try {
            return LocalPageCacheContext::createPersistent(
                cacheDirectory, configurationDirectory);
        } catch (const PrivateStorageError &error) {
            qWarning().noquote()
                << QStringLiteral(
                       "Persistent point-page cache is unavailable; using a "
                       "private temporary cache: %1")
                       .arg(QString::fromLocal8Bit(error.what()));
        }
    }
    return LocalPageCacheContext::createTemporary(workingDirectory);
}

void populateInitialDocument(RenderViewport &viewport,
                             const ApplicationConfig &config,
                             const std::uint64_t decodedByteBudget,
                             const LocalPageCacheContextPtr &cache,
                             const PointColorMapCatalogSnapshotPtr &colorMaps)
{
    SceneRuntime runtime(decodedByteBudget);
    if (config.sources.empty()) {
        auto document = std::make_shared<SceneDocument>(colorMaps);
        if (config.syntheticPointCount) {
            static_cast<void>(addInitialPointLayer(
                runtime,
                *document,
                buildSyntheticScene(*config.syntheticPointCount)));
        }
        viewport.setDocument(document->snapshot(),
                             runtime.snapshot(),
                             {.decodedPointBytes = runtime.decodedByteBudget()},
                             SessionGeneration{1},
                             true);
        return;
    }
    if (!config.smokeTest) {
        return;
    }

    // Smoke tests exit after a few frames. Load synchronously so content is
    // resident for the first rendered frame and stdout remains deterministic.
    auto document = std::make_shared<SceneDocument>(colorMaps);
    PdalPointCloudLoader loader;
    for (const std::filesystem::path &sourcePath : config.sources) {
        if (supportedSourceKind(sourcePath) !=
            SupportedSourceKind::PointCloud) {
            throw std::invalid_argument(
                "--smoke-test supports point-cloud sources only");
        }
        const PointCloudLoadOptions options{
            .sourcePath = sourcePath,
            .maximumPoints = config.maximumLoadPoints,
            .localPaging =
                {
                    .cache = cache,
                },
        };
        const PointCloudLoadResources resources{
            .decodedByteBudget = decodedByteBudget,
            .residency = runtime.residencyCoordinator(),
            .memoryBudget = {},
            .flatReservation = {},
        };
        const PointCloudImportPreflight preflight = loader.inspect(
            options, resources.decodedByteBudget, std::stop_token{});
        PointDatasetRuntimePtr scene =
            pci::createPointDatasetRuntime(loader.load(
                options, resources, preflight, PointCloudLoadContext{}));
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
        qInfo().noquote() << QStringLiteral("Loaded %1: %2 / %3 points")
                                 .arg(pathToQString(sourcePath))
                                 .arg(scene->totalPointCount())
                                 .arg(scene->metadata().sourcePointCount);
#endif
        static_cast<void>(addInitialPointLayer(runtime, *document, scene));
    }
    viewport.setDocument(document->snapshot(),
                         runtime.snapshot(),
                         {.decodedPointBytes = runtime.decodedByteBudget()},
                         SessionGeneration{1},
                         true);
}

ImportServices
createImportServices(const std::filesystem::path &workingDirectory)
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
    services.rasterElevation =
        std::make_unique<RasterElevationController>(*services.scheduler);
    services.colorize = std::make_unique<PointCloudColorizeController>(
        *services.scheduler, makeLocalRasterColorizeRunStoreFactory());
    services.spatialReferences =
        std::make_shared<GdalSpatialReferenceComparator>();
    services.statistics = std::make_shared<PdalPointCloudStatistics>();
    services.workingDirectory = workingDirectory;
    const auto cacheBase =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    services.storageMaintenance = makeLocalStorageMaintenance({
        .pointCache = pointPageCacheDirectory(cacheBase),
        .workingFiles = workingDirectory,
        .legacyPointCache = cacheBase.isEmpty()
                                ? std::filesystem::path{}
                                : qStringToPath(cacheBase) / "point-pages-v2",
        .legacyTemporaryBase =
            std::filesystem::canonical(qStringToPath(QDir::tempPath())),
    });
    return services;
}

} // namespace

int reportGdalCapabilities()
{
    configurePackagedGeospatialData();
    const GdalCatalogCapabilities &capabilities = gdalCatalogCapabilities();
    const auto yesNo = [](const bool value) {
        return value ? QStringLiteral("yes") : QStringLiteral("no");
    };
    const QString report =
        QStringLiteral("gdal_version: %1\n")
            .arg(QString::fromStdString(capabilities.version.release)) +
        QStringLiteral("driver_gti: %1\n").arg(yesNo(capabilities.tileIndex)) +
        QStringLiteral("driver_vrt: %1\n")
            .arg(yesNo(capabilities.virtualRaster)) +
        QStringLiteral("driver_gpkg: %1\n")
            .arg(yesNo(capabilities.geoPackage)) +
        QStringLiteral("driver_flatgeobuf: %1\n")
            .arg(yesNo(capabilities.flatGeobuf)) +
        QStringLiteral("driver_shapefile: %1\n")
            .arg(yesNo(capabilities.shapefile)) +
        QStringLiteral("catalog_import: %1")
            .arg(yesNo(capabilities.catalogImport()));
    QTextStream stream(stdout, QIODevice::WriteOnly);
    stream << report << '\n';
    stream.flush();
    // A non-zero exit is what lets a packaging job fail on a stripped GDAL
    // rather than shipping a build that silently cannot open catalogs or VRT
    // mosaics.
    return capabilities.catalogImport() && capabilities.virtualRaster ? 0 : 1;
}

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
    const auto workingDirectory =
        managedWorkingDirectory(qStringToPath(QDir::tempPath()));
    const LocalPageCacheContextPtr cache =
        defaultPointPageCache(workingDirectory);

    std::unique_ptr<RenderViewport> viewport =
        createRenderViewport(config.smokeTest,
                             config.gpuByteBudget,
                             config.graphicsApi,
                             config.gpuValidation,
                             colorMaps);
    populateInitialDocument(
        *viewport, config, memoryBudget.pointByteBudget, cache, colorMaps);

    auto window =
        std::make_unique<MainWindow>(std::move(viewport),
                                     createImportServices(workingDirectory),
                                     config.maximumLoadPoints,
                                     memoryBudget.pointByteBudget,
                                     memoryBudget.automaticParameters,
                                     cache,
                                     colorMaps);
    // The window budgets and reports the GDAL cache without linking GDAL.
    window->setGdalCacheControls(GdalCacheControls{
        .setByteBudget =
            [](const std::uint64_t bytes) {
                setGdalBlockCacheBytes(bytes);
            },
        .usedBytes =
            [] {
                return gdalBlockCacheUsedBytes();
            },
    });
    const GdalCatalogCapabilities &capabilities = gdalCatalogCapabilities();
    window->setGdalRuntimeInfo(GdalRuntimeInfo{
        .version = QString::fromStdString(capabilities.version.release),
        .tileIndexDriver = capabilities.tileIndex,
        .virtualRasterDriver = capabilities.virtualRaster,
        .geoPackageDriver = capabilities.geoPackage,
        .flatGeobufDriver = capabilities.flatGeobuf,
        .shapefileDriver = capabilities.shapefile,
        .probed = true,
    });
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
                window->openSources(sources);
            });
    }
    return window;
}

} // namespace pci
