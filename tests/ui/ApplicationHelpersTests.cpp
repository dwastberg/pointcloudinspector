#include "app/PerformanceSettingsStore.h"
#include "app/RasterColorizeUiState.h"
#include "app/RenderDiagnosticsFormatter.h"
#include "app/ViewportSettingsStore.h"
#include "app/WorkspaceSettings.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDockWidget>
#include <QMainWindow>
#include <QSettings>
#include <QTemporaryDir>

#include <memory>

TEST_CASE("workspace settings preserve stable geometry and state keys",
          "[ui][workspace]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("workspace.ini")),
                       QSettings::IniFormat);
    QMainWindow source;
    source.setObjectName(QStringLiteral("sourceWindow"));
    source.resize(720, 480);
    QDockWidget sourceDock(QStringLiteral("Layers"), &source);
    sourceDock.setObjectName(QStringLiteral("layersDock"));
    source.addDockWidget(Qt::RightDockWidgetArea, &sourceDock);

    pci::WorkspaceSettings::save(source, settings);
    CHECK(settings.contains(QStringLiteral("workspace/geometry")));
    CHECK(settings.contains(QStringLiteral("workspace/state")));

    QMainWindow restored;
    QDockWidget restoredDock(QStringLiteral("Layers"), &restored);
    restoredDock.setObjectName(QStringLiteral("layersDock"));
    restored.addDockWidget(Qt::LeftDockWidgetArea, &restoredDock);
    pci::WorkspaceSettings::restore(restored, settings);

    CHECK(restored.size() == source.size());
    CHECK(restored.dockWidgetArea(&restoredDock) == Qt::RightDockWidgetArea);
}

TEST_CASE("raster colorize controls expose contextual state",
          "[ui][raster][colorize]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePath = "survey.laz";
    pci::PointCloudLayer point{
        .id = pci::SceneLayerId{7},
        .scene = std::make_shared<pci::PointCloudScene>(metadata),
    };
    point.scene->markLoadingComplete();

    auto state = pci::rasterColorizeUiState(&point, 1, false, false);
    CHECK(state.startEnabled);
    CHECK(state.startText == QStringLiteral("Colorize from raster…"));
    CHECK_FALSE(state.revertVisible);

    state = pci::rasterColorizeUiState(&point, 0, false, false);
    CHECK_FALSE(state.startEnabled);
    CHECK(state.startToolTip.contains(QStringLiteral("Import a raster")));

    point.rasterColors = pci::RasterPointColorBinding{};
    state = pci::rasterColorizeUiState(&point, 1, false, false);
    CHECK(state.startEnabled);
    CHECK(state.startText == QStringLiteral("Recolor from raster…"));
    CHECK(state.revertVisible);
    CHECK(state.revertEnabled);

    state = pci::rasterColorizeUiState(&point, 1, true, false);
    CHECK_FALSE(state.startEnabled);
    CHECK(state.startText == QStringLiteral("Colorizing…"));
    CHECK(state.revertEnabled);
    CHECK(state.revertToolTip.contains(QStringLiteral("Cancel")));

    state = pci::rasterColorizeUiState(&point, 1, true, true);
    CHECK(state.startText == QStringLiteral("Applying raster colors…"));
    CHECK_FALSE(state.revertEnabled);
    CHECK(state.revertToolTip.contains(QStringLiteral("commit")));
}

TEST_CASE("viewport settings preserve appearance and depth enhancement",
          "[ui][settings][persistence]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("viewport.ini")),
                       QSettings::IniFormat);
    const pci::ViewportSettings source{
        .backgroundColor = {.red = 0.2F, .green = 0.4F, .blue = 0.6F},
        .depthEnhancement =
            {
                .enabled = false,
                .radius = 2.5F,
                .strength = 42.0F,
            },
    };

    pci::ViewportSettingsStore::save(source, settings);
    const pci::ViewportSettings restored =
        pci::ViewportSettingsStore::restore(settings);

    CHECK(restored.backgroundColor.red ==
          Catch::Approx(source.backgroundColor.red).margin(0.0001F));
    CHECK(restored.backgroundColor.green ==
          Catch::Approx(source.backgroundColor.green).margin(0.0001F));
    CHECK(restored.backgroundColor.blue ==
          Catch::Approx(source.backgroundColor.blue).margin(0.0001F));
    CHECK(restored.depthEnhancement == source.depthEnhancement);
}

TEST_CASE("performance settings preserve cache and point budgets",
          "[ui][settings][persistence]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("performance.ini")),
                       QSettings::IniFormat);
    const pci::PerformanceSettings source{
        .automaticCpuCache = false,
        .cpuCacheMebibytes = 2048,
        .gpuCacheMebibytes = 768,
        .maximumLoadPoints = 75'000'000,
    };

    pci::PerformanceSettingsStore::save(source, settings);
    CHECK(pci::PerformanceSettingsStore::restore(settings) == source);
}

TEST_CASE("render diagnostics formatter owns panel and status presentation",
          "[ui][diagnostics]")
{
    const pci::RenderMetrics metrics{
        .deviceName = QStringLiteral("Test GPU"),
        .requestedBackend = QStringLiteral("auto"),
        .selectedBackend = QStringLiteral("Metal"),
        .timingSource = QStringLiteral("CPU"),
        .gpuValidationEnabled = true,
        .framesPerSecond = 200.0,
        .frameMilliseconds = 5.0,
        .decodedPointBytes = 2U * 1024U * 1024U,
        .decodedPointBudgetBytes = 4U * 1024U * 1024U,
        .gpuPointBytes = 3U * 1024U * 1024U,
        .visibleLayerCount = 2,
        .coveredLayerCount = 1,
    };
    const pci::PointCloudLoadControllerMetrics loadMetrics{};

    const QString panel =
        pci::RenderDiagnosticsFormatter::panelText(metrics, loadMetrics);
    CHECK(panel.contains(QStringLiteral("Coverage: 1 / 2 visible sources")));
    CHECK(panel.contains(QStringLiteral("CPU pages: 2.0 / 4 MiB")));

    const QString status =
        pci::RenderDiagnosticsFormatter::statusText(metrics, 12.25);
    CHECK(status.startsWith(QStringLiteral(
        "Test GPU [auto→Metal+validation] | CPU 5.00ms 200.0FPS")));
    CHECK(status.contains(QStringLiteral("First 12.3 ms")));
}

TEST_CASE("render diagnostics report all three raster allocators",
          "[ui][diagnostics][raster]")
{
    const pci::RenderMetrics metrics{
        .rasterCpuBytes = 6U * 1024U * 1024U,
        .rasterCpuBudgetBytes = 16U * 1024U * 1024U,
        .rasterCpuPeakBytes = 9U * 1024U * 1024U,
        .rasterGpuBytes = 4U * 1024U * 1024U,
        .rasterGpuBudgetBytes = 8U * 1024U * 1024U,
        .rasterGpuPeakBytes = 5U * 1024U * 1024U,
        .rasterTilesRequested = 40,
        .rasterTilesCompleted = 32,
        .rasterTilesCancelled = 5,
        .rasterTilesFailed = 1,
        .rasterCacheEvictions = 7,
        .rasterUploadedTiles = 30,
        .rasterResidentTiles = 28,
        .rasterSelectedTiles = 24,
        .rasterDrawnTiles = 26,
        .rasterPendingReads = 2,
        .rasterFinestLevel = 1,
        .rasterCoarsestLevel = 3,
    };
    const pci::PointCloudLoadControllerMetrics loadMetrics{};

    const QString panel = pci::RenderDiagnosticsFormatter::panelText(
        metrics,
        loadMetrics,
        std::uint64_t{12} * 1024 * 1024,
        std::uint64_t{32} * 1024 * 1024);
    CHECK(panel.contains(QStringLiteral(
        "Raster memory: CPU 6.0 / 16.0 MiB (peak 9.0), GPU 4.0 / 8.0 MiB "
        "(peak 5.0), GDAL block cache 12.0 MiB / 32.0 MiB")));
    CHECK(panel.contains(QStringLiteral(
        "Raster tiles: 26 drawn / 24 selected / 28 resident, 2 pending, "
        "levels 1-3")));
    CHECK(panel.contains(QStringLiteral(
        "Raster reads: 40 requested, 32 completed, 5 cancelled, 1 failed, "
        "30 uploaded, 7 evictions")));

    // Without the GDAL hook the third allocator is named as unavailable
    // rather than reported as zero, which would read as "nothing cached".
    const QString withoutGdal =
        pci::RenderDiagnosticsFormatter::panelText(metrics, loadMetrics);
    CHECK(withoutGdal.contains(QStringLiteral("unavailable")));
    CHECK_FALSE(withoutGdal.contains(QStringLiteral("GDAL block cache 0.0")));
}
