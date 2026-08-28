#include "app/ApplicationConfig.h"

#include "foundation/CheckedArithmetic.h"
#include "import/SupportedSource.h"
#include "platform/QtPath.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace pci {
namespace {

constexpr std::uint64_t bytesPerMiB = std::uint64_t{1024} * 1024;

ConfigEarlyExit error(QString message, const int exitCode = 2)
{
    return {
        .message = std::move(message),
        .exitCode = exitCode,
        .writeToStandardError = true,
    };
}

} // namespace

ApplicationInvocation parseApplicationInvocation(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Cross-platform point-cloud inspector"));
    const QCommandLineOption helpOption = parser.addHelpOption();
    const QCommandLineOption versionOption = parser.addVersionOption();
    parser.addPositionalArgument(
        QStringLiteral("files"),
        QStringLiteral(
            "Point-cloud, vector, or raster sources to open. Wildcards (*, "
            "?, [set]) are expanded."),
        QStringLiteral("[files...]"));
    const QCommandLineOption pointsOption(
        QStringList{QStringLiteral("p"), QStringLiteral("points")},
        QStringLiteral("Create a synthetic point cube with this many points."),
        QStringLiteral("count"));
    const QCommandLineOption smokeOption(
        QStringLiteral("smoke-test"),
        QStringLiteral("Exit after three rendered frames."));
    const QCommandLineOption maximumPointsOption(
        QStringLiteral("max-points"),
        QStringLiteral(
            "Maximum points retained for a non-paged point-cloud source. "
            "Paged and hierarchical sources retain all points."),
        QStringLiteral("count"),
        QStringLiteral("10000000"));
    const QCommandLineOption cpuCacheOption(
        QStringLiteral("cpu-cache-mb"),
        QStringLiteral(
            "Total resident point-payload budget shared by flat previews and "
            "hierarchical decoded pages, in MiB, or 'auto'."),
        QStringLiteral("MiB|auto"),
        QStringLiteral("auto"));
    const QCommandLineOption gpuCacheOption(
        QStringLiteral("gpu-cache-mb"),
        QStringLiteral("GPU point-buffer residency budget, in MiB."),
        QStringLiteral("MiB"),
        QStringLiteral("512"));
    const QCommandLineOption rasterCpuCacheOption(
        QStringLiteral("raster-cpu-cache-mb"),
        QStringLiteral("Decoded raster tile budget, in MiB. Separate from the "
                       "point caches."),
        QStringLiteral("MiB"),
        QString::number(defaultRasterCpuCacheMebibytes));
    const QCommandLineOption rasterGpuCacheOption(
        QStringLiteral("raster-gpu-cache-mb"),
        QStringLiteral("Raster tile texture budget, in MiB. Separate from the "
                       "point caches."),
        QStringLiteral("MiB"),
        QString::number(defaultRasterGpuCacheMebibytes));
    const QCommandLineOption gdalCacheOption(
        QStringLiteral("gdal-cache-mb"),
        QStringLiteral("GDAL block cache, in MiB. Process-global and invisible "
                       "to the application's own accounting."),
        QStringLiteral("MiB"),
        QString::number(defaultGdalCacheMebibytes));
    const QCommandLineOption rasterWorkersOption(
        QStringLiteral("raster-workers"),
        QStringLiteral("Raster tile read workers. Applies after restart."),
        QStringLiteral("count"),
        QString::number(defaultRasterReadWorkers));
    const QCommandLineOption graphicsApiOption(
        QStringLiteral("graphics-api"),
        QStringLiteral(
            "Graphics API: auto, metal, vulkan, d3d11, d3d12, or opengl."),
        QStringLiteral("api"),
        QStringLiteral("auto"));
    // Deliberately outside the diagnostic-UI guard: packaged builds are the
    // ones whose driver set differs from a developer machine's, and they are
    // built with diagnostics off.
    const QCommandLineOption gdalCapabilitiesOption(
        QStringLiteral("gdal-capabilities"),
        QStringLiteral("Print the GDAL version and raster driver availability "
                       "of this build, then exit."));
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    const QCommandLineOption gpuValidationOption(
        QStringLiteral("gpu-validation"),
        QStringLiteral("Enable the graphics backend debug/validation layer."));
    const QCommandLineOption qualificationReportOption(
        QStringLiteral("qualification-report"),
        QStringLiteral("Write Release H native GPU/load qualification metrics "
                       "as JSON after display readiness."),
        QStringLiteral("path"));
    const QCommandLineOption qualificationExitOption(
        QStringLiteral("qualification-exit"),
        QStringLiteral("Exit after writing --qualification-report."));
#endif
    parser.addOption(pointsOption);
    parser.addOption(smokeOption);
    parser.addOption(maximumPointsOption);
    parser.addOption(cpuCacheOption);
    parser.addOption(gpuCacheOption);
    parser.addOption(rasterCpuCacheOption);
    parser.addOption(rasterGpuCacheOption);
    parser.addOption(gdalCacheOption);
    parser.addOption(rasterWorkersOption);
    parser.addOption(graphicsApiOption);
    parser.addOption(gdalCapabilitiesOption);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    parser.addOption(gpuValidationOption);
    parser.addOption(qualificationReportOption);
    parser.addOption(qualificationExitOption);
#endif

    if (!parser.parse(arguments)) {
        return error(parser.errorText(), 1);
    }
    if (parser.isSet(versionOption)) {
        return ConfigEarlyExit{
            .message = QStringLiteral("%1 %2").arg(
                QCoreApplication::applicationName(),
                QCoreApplication::applicationVersion()),
        };
    }
    if (parser.isSet(helpOption) || parser.isSet(QStringLiteral("help-all"))) {
        return ConfigEarlyExit{.message = parser.helpText()};
    }

    ApplicationConfig config;
    if (parser.isSet(pointsOption)) {
        const auto pointCount =
            parsePointCount(parser.value(pointsOption).toStdString());
        if (!pointCount) {
            return error(
                QStringLiteral("--points must be a positive 64-bit integer"));
        }
        config.syntheticPointCount = pointCount;
    }
    const auto maximumPoints =
        parsePointCount(parser.value(maximumPointsOption).toStdString());
    if (!maximumPoints) {
        return error(
            QStringLiteral("--max-points must be a positive 64-bit integer"));
    }
    config.maximumLoadPoints = *maximumPoints;

    const std::uint64_t maximumMiB =
        std::numeric_limits<std::uint64_t>::max() / bytesPerMiB;
    const auto cpuCache =
        parseMemoryBudgetOption(parser.value(cpuCacheOption).toStdString());
    if (!cpuCache ||
        (!cpuCache->automatic && cpuCache->mebibytes > maximumMiB)) {
        return error(
            QStringLiteral("--cpu-cache-mb must be 'auto' or a positive "
                           "byte-representable integer"));
    }
    config.cpuBudget = *cpuCache;
    const auto gpuCache =
        parsePointCount(parser.value(gpuCacheOption).toStdString());
    if (!gpuCache || *gpuCache > maximumMiB) {
        return error(QStringLiteral(
            "--gpu-cache-mb must be a positive byte-representable integer"));
    }
    config.gpuByteBudget = *gpuCache * bytesPerMiB;

    // Every raster budget is validated before any byte conversion, so a value
    // that would overflow the multiplication is rejected rather than wrapped.
    struct RasterOption {
        const QCommandLineOption &option;
        const char *name;
        std::uint64_t minimum;
        std::uint64_t *target;
    };
    std::uint64_t rasterWorkers = config.raster.readWorkers;
    const std::array<RasterOption, 4> rasterOptions{
        RasterOption{rasterCpuCacheOption,
                     "--raster-cpu-cache-mb",
                     minimumRasterCpuCacheMebibytes,
                     &config.raster.cpuCacheMebibytes},
        RasterOption{rasterGpuCacheOption,
                     "--raster-gpu-cache-mb",
                     minimumRasterGpuCacheMebibytes,
                     &config.raster.gpuCacheMebibytes},
        RasterOption{gdalCacheOption,
                     "--gdal-cache-mb",
                     minimumGdalCacheMebibytes,
                     &config.raster.gdalCacheMebibytes},
        RasterOption{rasterWorkersOption,
                     "--raster-workers",
                     minimumRasterReadWorkers,
                     &rasterWorkers},
    };
    for (const RasterOption &entry : rasterOptions) {
        const bool workers = entry.target == &rasterWorkers;
        const std::uint64_t ceiling =
            workers ? maximumRasterReadWorkers : maximumMiB;
        const auto parsed =
            parsePointCount(parser.value(entry.option).toStdString());
        if (!parsed || *parsed < entry.minimum || *parsed > ceiling) {
            return error(QStringLiteral("%1 must be between %2 and %3")
                             .arg(QString::fromLatin1(entry.name))
                             .arg(entry.minimum)
                             .arg(ceiling));
        }
        *entry.target = *parsed;
    }
    config.raster.readWorkers = static_cast<std::uint32_t>(rasterWorkers);

    const auto graphicsApi =
        parseGraphicsApi(parser.value(graphicsApiOption).toStdString());
    if (!graphicsApi) {
        return error(QStringLiteral(
            "--graphics-api must be one of: auto, metal, vulkan, d3d11, "
            "d3d12, opengl"));
    }
    if (!graphicsApiSupportedOnPlatform(*graphicsApi)) {
        return error(
            QStringLiteral("Graphics API '%1' is not supported on this "
                           "platform; use --graphics-api auto")
                .arg(parser.value(graphicsApiOption)));
    }
    config.graphicsApi = *graphicsApi;
    config.smokeTest = parser.isSet(smokeOption);
    config.reportGdalCapabilities = parser.isSet(gdalCapabilitiesOption);

    const QStringList positional = parser.positionalArguments();
    std::vector<std::filesystem::path> paths;
    paths.reserve(positional.size());
    for (const QString &argument : positional) {
        paths.push_back(qStringToPath(argument));
    }
    config.sources = expandPathArguments(paths);
    if (!positional.isEmpty() && config.sources.empty()) {
        return error(QStringLiteral("No files matched the given arguments"));
    }
    if (config.smokeTest &&
        std::ranges::any_of(config.sources, [](const auto &path) {
            return supportedSourceKind(path) != SupportedSourceKind::PointCloud;
        })) {
        return error(
            QStringLiteral("--smoke-test supports point-cloud sources only"));
    }

#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    config.gpuValidation = parser.isSet(gpuValidationOption);
    if (parser.isSet(qualificationExitOption) &&
        !parser.isSet(qualificationReportOption)) {
        return error(QStringLiteral(
            "--qualification-exit requires --qualification-report"));
    }
    if (parser.isSet(qualificationReportOption) &&
        std::ranges::none_of(config.sources, [](const auto &path) {
            return supportedSourceKind(path) == SupportedSourceKind::PointCloud;
        })) {
        return error(QStringLiteral(
            "--qualification-report requires at least one point-cloud "
            "source"));
    }
    if (parser.isSet(qualificationReportOption) && config.smokeTest) {
        return error(QStringLiteral(
            "--qualification-report uses display readiness and cannot be "
            "combined with --smoke-test"));
    }
    if (parser.isSet(qualificationReportOption)) {
        config.qualification = QualificationOptions{
            .reportPath =
                qStringToPath(parser.value(qualificationReportOption)),
            .exitAfterWrite = parser.isSet(qualificationExitOption),
        };
    }
#endif
    return config;
}

ResolvedMemoryBudget
resolveMemoryBudget(const ApplicationConfig &config,
                    const SystemMemoryInfo &memory) noexcept
{
    ResolvedMemoryBudget result;
    if (!config.cpuBudget.automatic) {
        result.pointByteBudget = config.cpuBudget.mebibytes * bytesPerMiB;
        return result;
    }
    result.automaticParameters.emplace();
    // Unified-memory devices draw point and raster textures from the same
    // pool, so the GPU allowance is their sum rather than the point budget.
    result.automaticParameters->gpuByteBudget =
        saturatingAdd(config.gpuByteBudget,
                      mebibytesToBytes(config.raster.gpuCacheMebibytes));
    result.automaticParameters->rasterCpuByteBudget =
        mebibytesToBytes(config.raster.cpuCacheMebibytes);
    result.automaticParameters->gdalCacheByteBudget =
        mebibytesToBytes(config.raster.gdalCacheMebibytes);
    result.automaticBudget =
        automaticMemoryBudget(memory, *result.automaticParameters);
    result.pointByteBudget = result.automaticBudget.pointByteBudget;
    return result;
}

std::filesystem::path
pointPageCacheDirectory(const QString &standardCacheLocation)
{
    return standardCacheLocation.isEmpty()
               ? std::filesystem::path{}
               : qStringToPath(standardCacheLocation) / "point-pages-v2";
}

std::filesystem::path
pointPageCacheConfigurationDirectory(const QString &standardConfigLocation)
{
    return standardConfigLocation.isEmpty()
               ? std::filesystem::path{}
               : qStringToPath(standardConfigLocation) / "cache-security";
}

} // namespace pci
