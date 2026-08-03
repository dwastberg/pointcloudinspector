#include "app/RenderDiagnosticsFormatter.h"
#include "app/WorkspaceSettings.h"

#include <catch2/catch_test_macros.hpp>

#include <QDockWidget>
#include <QMainWindow>
#include <QSettings>
#include <QTemporaryDir>

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
