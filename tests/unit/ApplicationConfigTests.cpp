#include "app/ApplicationConfig.h"
#include "app/ApplicationOptions.h"
#include "app/MemoryBudgetPolicy.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

pci::ApplicationConfig configFor(const QStringList &arguments)
{
    const pci::ApplicationInvocation invocation =
        pci::parseApplicationInvocation(arguments);
    REQUIRE(std::holds_alternative<pci::ApplicationConfig>(invocation));
    return std::get<pci::ApplicationConfig>(invocation);
}

pci::ConfigEarlyExit earlyExitFor(const QStringList &arguments)
{
    const pci::ApplicationInvocation invocation =
        pci::parseApplicationInvocation(arguments);
    REQUIRE(std::holds_alternative<pci::ConfigEarlyExit>(invocation));
    return std::get<pci::ConfigEarlyExit>(invocation);
}

TEST_CASE("application invocation starts without a synthetic scene",
          "[unit][app-config][cli]")
{
    const pci::ApplicationConfig config =
        configFor({QStringLiteral("pcinspector")});
    CHECK_FALSE(config.syntheticPointCount.has_value());
}

TEST_CASE("application invocation parser returns typed configuration",
          "[unit][app-config][cli]")
{
    const pci::ApplicationConfig config = configFor({
        QStringLiteral("pcinspector"),
        QStringLiteral("--points"),
        QStringLiteral("25"),
        QStringLiteral("--max-points=50"),
        QStringLiteral("--cpu-cache-mb=256"),
        QStringLiteral("--gpu-cache-mb=64"),
        QStringLiteral("--graphics-api=auto"),
        QStringLiteral("--smoke-test"),
        QStringLiteral("cloud.las"),
    });
    CHECK(config.syntheticPointCount == 25);
    CHECK(config.maximumLoadPoints == 50);
    CHECK(config.cpuBudget ==
          pci::MemoryBudgetOption{.automatic = false, .mebibytes = 256});
    CHECK(config.gpuByteBudget == std::uint64_t{64} * 1024 * 1024);
    CHECK(config.graphicsApi == pci::GraphicsApi::Auto);
    CHECK(config.smokeTest);
    REQUIRE(config.sources.size() == 1);
    CHECK(config.sources.front() == std::filesystem::path("cloud.las"));
}

TEST_CASE("application invocation parser reports CLI failures without exiting",
          "[unit][app-config][cli]")
{
    struct FailureCase {
        QStringList arguments;
        QString message;
        int exitCode;
    };
    const std::array cases{
        FailureCase{
            {QStringLiteral("pcinspector"), QStringLiteral("--unknown")},
            QStringLiteral("Unknown option"),
            1},
        FailureCase{{QStringLiteral("pcinspector"), QStringLiteral("--points")},
                    QStringLiteral("Missing value"),
                    1},
        FailureCase{
            {QStringLiteral("pcinspector"), QStringLiteral("--points=0")},
            QStringLiteral("--points must"),
            2},
        FailureCase{{QStringLiteral("pcinspector"),
                     QStringLiteral("--max-points=18446744073709551616")},
                    QStringLiteral("--max-points must"),
                    2},
        FailureCase{{QStringLiteral("pcinspector"),
                     QStringLiteral("--cpu-cache-mb=17592186044416")},
                    QStringLiteral("--cpu-cache-mb must"),
                    2},
        FailureCase{{QStringLiteral("pcinspector"),
                     QStringLiteral("--gpu-cache-mb=17592186044416")},
                    QStringLiteral("--gpu-cache-mb must"),
                    2},
        FailureCase{{QStringLiteral("pcinspector"),
                     QStringLiteral("--graphics-api=software")},
                    QStringLiteral("--graphics-api must"),
                    2},
        FailureCase{
            {QStringLiteral("pcinspector"),
             QStringLiteral("/pci-path-that-does-not-exist-9137/*.las")},
            QStringLiteral("No files matched"),
            2},
    };
    for (const FailureCase &test : cases) {
        const pci::ConfigEarlyExit exit = earlyExitFor(test.arguments);
        CAPTURE(test.arguments, exit.message);
        CHECK(exit.exitCode == test.exitCode);
        CHECK(exit.writeToStandardError);
        CHECK(exit.message.contains(test.message, Qt::CaseInsensitive));
    }

#if defined(Q_OS_MACOS)
    constexpr auto unsupportedApi = "d3d11";
#elif defined(Q_OS_WIN)
    constexpr auto unsupportedApi = "metal";
#else
    constexpr auto unsupportedApi = "metal";
#endif
    const pci::ConfigEarlyExit unsupported =
        earlyExitFor({QStringLiteral("pcinspector"),
                      QStringLiteral("--graphics-api=%1").arg(unsupportedApi)});
    CHECK(unsupported.exitCode == 2);
    CHECK(unsupported.message.contains(QStringLiteral("not supported")));
}

TEST_CASE("application invocation parser returns help and version text",
          "[unit][app-config][cli]")
{
    QCoreApplication::setApplicationName(
        QStringLiteral("Point Cloud Inspector"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0-test"));

    const pci::ConfigEarlyExit help =
        earlyExitFor({QStringLiteral("pcinspector"), QStringLiteral("--help")});
    CHECK(help.exitCode == 0);
    CHECK_FALSE(help.writeToStandardError);
    CHECK(help.message.contains(QStringLiteral("Usage:")));
    CHECK(help.message.contains(QStringLiteral("--max-points")));

    const pci::ConfigEarlyExit helpAll = earlyExitFor(
        {QStringLiteral("pcinspector"), QStringLiteral("--help-all")});
    CHECK(helpAll.exitCode == 0);
    CHECK_FALSE(helpAll.writeToStandardError);
    CHECK(helpAll.message.contains(QStringLiteral("Qt")));

    const pci::ConfigEarlyExit version = earlyExitFor(
        {QStringLiteral("pcinspector"), QStringLiteral("--version")});
    CHECK(version.exitCode == 0);
    CHECK_FALSE(version.writeToStandardError);
    CHECK(version.message ==
          QStringLiteral("Point Cloud Inspector 0.1.0-test"));
}

TEST_CASE("memory and cache resolution are deterministic",
          "[unit][app-config][memory][path]")
{
    constexpr std::uint64_t MiB = std::uint64_t{1024} * 1024;
    pci::ApplicationConfig explicitConfig;
    explicitConfig.cpuBudget = {.automatic = false, .mebibytes = 768};
    const pci::ResolvedMemoryBudget explicitBudget =
        pci::resolveMemoryBudget(explicitConfig, {});
    CHECK(explicitBudget.pointByteBudget == 768 * MiB);
    CHECK_FALSE(explicitBudget.automaticParameters);

    pci::ApplicationConfig automaticConfig;
    automaticConfig.gpuByteBudget = 64 * MiB;
    const pci::ResolvedMemoryBudget automaticBudget = pci::resolveMemoryBudget(
        automaticConfig,
        {.totalPhysicalBytes = std::uint64_t{16} * 1024 * MiB,
         .availablePhysicalBytes = std::uint64_t{14} * 1024 * MiB});
    REQUIRE(automaticBudget.automaticParameters);
    CHECK(automaticBudget.automaticParameters->gpuByteBudget == 64 * MiB);
    CHECK(automaticBudget.pointByteBudget ==
          automaticBudget.automaticBudget.pointByteBudget);

    CHECK(pci::pointPageCacheDirectory(QStringLiteral("/cache/pcinspector"),
                                       "/tmp") ==
          std::filesystem::path("/cache/pcinspector/point-pages"));
    CHECK(pci::pointPageCacheDirectory({}, "/fallback") ==
          std::filesystem::path("/fallback/pcinspector/point-pages"));
}

#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
TEST_CASE("qualification CLI dependencies are validated",
          "[unit][app-config][cli][qualification]")
{
    CHECK(earlyExitFor({QStringLiteral("pcinspector"),
                        QStringLiteral("--qualification-exit")})
              .message.contains(QStringLiteral("requires")));
    CHECK(earlyExitFor({QStringLiteral("pcinspector"),
                        QStringLiteral("--qualification-report=report.json")})
              .message.contains(QStringLiteral("at least one")));
    CHECK(earlyExitFor({QStringLiteral("pcinspector"),
                        QStringLiteral("--qualification-report=report.json"),
                        QStringLiteral("--smoke-test"),
                        QStringLiteral("cloud.las")})
              .message.contains(QStringLiteral("cannot be combined")));

    const pci::ApplicationConfig config =
        configFor({QStringLiteral("pcinspector"),
                   QStringLiteral("--gpu-validation"),
                   QStringLiteral("--qualification-report=report.json"),
                   QStringLiteral("--qualification-exit"),
                   QStringLiteral("cloud.las")});
    CHECK(config.gpuValidation);
    REQUIRE(config.qualification);
    CHECK(config.qualification->reportPath == "report.json");
    CHECK(config.qualification->exitAfterWrite);
}
#endif

TEST_CASE("point-count parsing accepts positive 64-bit values",
          "[unit][app-config]")
{
    using pci::parsePointCount;

    CHECK(parsePointCount("10000000") == std::uint64_t{10'000'000});
    CHECK(parsePointCount("1") == std::uint64_t{1});
    CHECK_FALSE(parsePointCount("0"));
    CHECK_FALSE(parsePointCount("-1"));
    CHECK_FALSE(parsePointCount("12x"));
    CHECK_FALSE(parsePointCount("18446744073709551616"));
}

TEST_CASE("memory-budget parsing accepts auto or positive MiB",
          "[unit][app-config][options][memory]")
{
    const auto automatic = pci::parseMemoryBudgetOption("auto");
    REQUIRE(automatic);
    CHECK(automatic->automatic);
    CHECK(automatic->mebibytes == 0);

    const auto explicitBudget = pci::parseMemoryBudgetOption("8192");
    REQUIRE(explicitBudget);
    CHECK_FALSE(explicitBudget->automatic);
    CHECK(explicitBudget->mebibytes == 8192);
    CHECK_FALSE(pci::parseMemoryBudgetOption("Auto"));
    CHECK_FALSE(pci::parseMemoryBudgetOption("0"));
    CHECK_FALSE(pci::parseMemoryBudgetOption("8GiB"));
}

TEST_CASE("graphics API parsing accepts only documented values",
          "[unit][app-config][options]")
{
    using pci::GraphicsApi;
    CHECK(pci::parseGraphicsApi("auto") == GraphicsApi::Auto);
    CHECK(pci::parseGraphicsApi("metal") == GraphicsApi::Metal);
    CHECK(pci::parseGraphicsApi("vulkan") == GraphicsApi::Vulkan);
    CHECK(pci::parseGraphicsApi("d3d11") == GraphicsApi::Direct3D11);
    CHECK(pci::parseGraphicsApi("d3d12") == GraphicsApi::Direct3D12);
    CHECK(pci::parseGraphicsApi("opengl") == GraphicsApi::OpenGL);
    CHECK_FALSE(pci::parseGraphicsApi("Metal"));
    CHECK_FALSE(pci::parseGraphicsApi("software"));
    CHECK(pci::graphicsApiName(GraphicsApi::Direct3D12) == "d3d12");
}

TEST_CASE("automatic memory budget preserves serious system headroom",
          "[unit][app-config][memory]")
{
    constexpr std::uint64_t MiB = std::uint64_t{1024} * 1024;
    constexpr std::uint64_t GiB = std::uint64_t{1024} * MiB;
    const pci::AutomaticMemoryBudget budget = pci::automaticMemoryBudget({
        .totalPhysicalBytes = 16 * GiB,
        .availablePhysicalBytes = 14 * GiB,
    });

    CHECK_FALSE(budget.usedFallback);
    CHECK(budget.systemReserveBytes == 2 * GiB);
    CHECK(budget.workingReserveBytes == 1280 * MiB);
    CHECK(budget.pointByteBudget == 10188 * MiB);
}

TEST_CASE("automatic memory budget keeps existing point residency admissible",
          "[unit][app-config][memory]")
{
    constexpr std::uint64_t GiB = std::uint64_t{1024} * 1024 * 1024;
    pci::AutomaticMemoryBudgetParameters parameters;
    parameters.currentPointBytes = 6 * GiB;
    const pci::AutomaticMemoryBudget budget = pci::automaticMemoryBudget(
        {
            .totalPhysicalBytes = 16 * GiB,
            .availablePhysicalBytes = 3 * GiB,
        },
        parameters);
    CHECK(budget.pointByteBudget == 6 * GiB);
}

TEST_CASE("automatic memory budget has a deterministic query fallback",
          "[unit][app-config][memory]")
{
    constexpr std::uint64_t GiB = std::uint64_t{1024} * 1024 * 1024;
    const pci::AutomaticMemoryBudget budget = pci::automaticMemoryBudget({});
    CHECK(budget.usedFallback);
    CHECK(budget.pointByteBudget == GiB);
}

struct ScopedGlobFixture {
    std::filesystem::path dir;

    ScopedGlobFixture()
    {
        static int counter = 0;
        dir = std::filesystem::temp_directory_path() /
              ("pci-glob-test-" + std::to_string(++counter));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }

    ~ScopedGlobFixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void touch(const std::string &name) const
    {
        std::ofstream(dir / name) << "x";
    }
};

TEST_CASE("path arguments without wildcards pass through unchanged",
          "[unit][app-config][path]")
{
    const auto expanded =
        pci::expandPathArguments({"missing.las", "some/dir/cloud.laz"});
    REQUIRE(expanded.size() == 2);
    CHECK(expanded[0] == std::filesystem::path("missing.las"));
    CHECK(expanded[1] == std::filesystem::path("some/dir/cloud.laz"));
}

TEST_CASE("path arguments expand wildcards to sorted matches",
          "[unit][app-config][path]")
{
    const ScopedGlobFixture fixture;
    fixture.touch("b.las");
    fixture.touch("a.las");
    fixture.touch("c.laz");
    fixture.touch("notes.txt");

    SECTION("star matches by extension, sorted")
    {
        const auto expanded =
            pci::expandPathArguments({(fixture.dir / "*.las").string()});
        REQUIRE(expanded.size() == 2);
        CHECK(expanded[0] == fixture.dir / "a.las");
        CHECK(expanded[1] == fixture.dir / "b.las");
    }

    SECTION("question mark matches a single character")
    {
        const auto expanded =
            pci::expandPathArguments({(fixture.dir / "?.laz").string()});
        REQUIRE(expanded.size() == 1);
        CHECK(expanded[0] == fixture.dir / "c.laz");
    }

    SECTION("character class selects among extensions")
    {
        const auto expanded =
            pci::expandPathArguments({(fixture.dir / "*.la[sz]").string()});
        REQUIRE(expanded.size() == 3);
        CHECK(expanded[0] == fixture.dir / "a.las");
        CHECK(expanded[1] == fixture.dir / "b.las");
        CHECK(expanded[2] == fixture.dir / "c.laz");
    }

    SECTION("a pattern matching nothing yields no paths")
    {
        const auto expanded =
            pci::expandPathArguments({(fixture.dir / "*.copc").string()});
        CHECK(expanded.empty());
    }

    SECTION("overlapping patterns are de-duplicated, keeping order")
    {
        const auto expanded = pci::expandPathArguments({
            (fixture.dir / "a.las").string(),
            (fixture.dir / "*.las").string(),
        });
        REQUIRE(expanded.size() == 2);
        CHECK(expanded[0] == fixture.dir / "a.las");
        CHECK(expanded[1] == fixture.dir / "b.las");
    }
}

} // namespace

TEST_CASE("raster budgets parse and validate from the command line",
          "[unit][app-config][raster]")
{
    const auto parse = [](const QStringList &extra) {
        QStringList arguments{QStringLiteral("pcinspector")};
        arguments += extra;
        return pci::parseApplicationInvocation(arguments);
    };

    const auto defaults = parse({});
    REQUIRE(std::holds_alternative<pci::ApplicationConfig>(defaults));
    const pci::RasterPerformanceSettings &standard =
        std::get<pci::ApplicationConfig>(defaults).raster;
    CHECK(standard.cpuCacheMebibytes == pci::defaultRasterCpuCacheMebibytes);
    CHECK(standard.gpuCacheMebibytes == pci::defaultRasterGpuCacheMebibytes);
    CHECK(standard.gdalCacheMebibytes == pci::defaultGdalCacheMebibytes);
    CHECK(standard.readWorkers == pci::defaultRasterReadWorkers);

    const auto explicitly = parse({QStringLiteral("--raster-cpu-cache-mb=64"),
                                   QStringLiteral("--raster-gpu-cache-mb=32"),
                                   QStringLiteral("--gdal-cache-mb=16"),
                                   QStringLiteral("--raster-workers=4")});
    REQUIRE(std::holds_alternative<pci::ApplicationConfig>(explicitly));
    const pci::RasterPerformanceSettings &chosen =
        std::get<pci::ApplicationConfig>(explicitly).raster;
    CHECK(chosen.cpuCacheMebibytes == 64);
    CHECK(chosen.gpuCacheMebibytes == 32);
    CHECK(chosen.gdalCacheMebibytes == 16);
    CHECK(chosen.readWorkers == 4);

    // Below the working minima, and above the worker ceiling, are rejected
    // rather than silently clamped: a command line is an explicit request.
    for (const QString &bad : {QStringLiteral("--raster-cpu-cache-mb=1"),
                               QStringLiteral("--raster-gpu-cache-mb=0"),
                               QStringLiteral("--gdal-cache-mb=0"),
                               QStringLiteral("--raster-workers=99")}) {
        CHECK(std::holds_alternative<pci::ConfigEarlyExit>(parse({bad})));
    }
}

TEST_CASE("raster budget clamping keeps every value usable",
          "[unit][app-config][raster]")
{
    const pci::RasterPerformanceSettings tiny =
        pci::clampRasterPerformanceSettings({.cpuCacheMebibytes = 0,
                                             .gpuCacheMebibytes = 0,
                                             .gdalCacheMebibytes = 0,
                                             .readWorkers = 0});
    CHECK(tiny.cpuCacheMebibytes == pci::minimumRasterCpuCacheMebibytes);
    // Sixteen guttered RGBA tiles plus conservative binding accounting.
    CHECK(tiny.gpuCacheMebibytes == pci::minimumRasterGpuCacheMebibytes);
    CHECK(tiny.gdalCacheMebibytes == pci::minimumGdalCacheMebibytes);
    CHECK(tiny.readWorkers == pci::minimumRasterReadWorkers);

    // Each additional handle to a VRT or GTI dataset opens its own member
    // datasets, so the worker ceiling stays low on purpose.
    const pci::RasterPerformanceSettings huge =
        pci::clampRasterPerformanceSettings(
            {.cpuCacheMebibytes = 1ULL << 60, .readWorkers = 1000});
    CHECK(huge.readWorkers == pci::maximumRasterReadWorkers);
    // Byte conversion is saturating, so a nonsense value cannot wrap the
    // multiplication.
    CHECK(pci::mebibytesToBytes(huge.cpuCacheMebibytes) > 0);
    CHECK(pci::mebibytesToBytes(1ULL << 62) ==
          static_cast<std::uint64_t>(pci::maximumCacheMebibytes) * 1024 * 1024);
}
