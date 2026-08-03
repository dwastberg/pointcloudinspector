#include "app/DiagnosticsDock.h"
#include "app/LayerInspectorDock.h"
#include "app/SceneLayersDock.h"
#include "app/TaskDock.h"
#include "app/WorkspaceSettings.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QListView>
#include <QMainWindow>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace {

pci::PointCloudLayer pointLayer(const std::uint64_t id,
                                const char *path,
                                const pci::PointColorMode colorMode)
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePath = path;
    metadata.sourceBounds = {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};
    metadata.sourcePointCount = 10;
    metadata.hasColor = true;
    metadata.hasIntensity = true;
    return {
        .id = pci::SceneLayerId{id},
        .scene = std::make_shared<pci::PointCloudScene>(metadata),
        .colorMode = colorMode,
    };
}

pci::VectorLayer vectorLayer(const std::uint64_t id,
                             const char *name,
                             const bool disjoint = false)
{
    auto data = std::make_shared<pci::VectorLayerData>();
    data->sublayerName = name;
    data->featureCount = 4;
    data->extentDisjointXY = disjoint;
    return {
        .id = pci::SceneLayerId{id},
        .data = std::move(data),
        .visible = !disjoint,
    };
}

pci::SceneDocumentSnapshotPtr
makeSnapshot(const std::vector<pci::PointCloudLayer> &points,
             const std::vector<pci::VectorLayer> &vectors,
             const std::vector<pci::SceneLayerId> &order)
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
                    },
            });
        } else {
            const auto vector =
                std::ranges::find(vectors, id, &pci::VectorLayer::id);
            REQUIRE(vector != vectors.end());
            snapshot->layers.push_back({
                .id = id,
                .visible = vector->visible,
                .payload =
                    pci::VectorLayerState{
                        .data = vector->data,
                        .style = vector->style,
                    },
            });
        }
    }
    return snapshot;
}

QListView &layerList(pci::SceneLayersDock &panel)
{
    auto *list =
        panel.findChild<QListView *>(QStringLiteral("pointCloudLayerList"));
    REQUIRE(list != nullptr);
    return *list;
}

struct MenuActionState {
    QString text;
    bool enabled = false;
};

std::vector<MenuActionState> contextMenuActions(QListView &list, const int row)
{
    std::vector<MenuActionState> result;
    const QModelIndex item = list.model()->index(row, 0);
    REQUIRE(item.isValid());
    const QPoint position = list.visualRect(item).center();
    QTimer::singleShot(0, [&result] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        REQUIRE(menu != nullptr);
        for (const QAction *action : menu->actions()) {
            if (!action->isSeparator()) {
                result.push_back({action->text(), action->isEnabled()});
            }
        }
        menu->close();
    });
    list.customContextMenuRequested(position);
    return result;
}

std::optional<MenuActionState>
findAction(const std::vector<MenuActionState> &actions, const QString &text)
{
    const auto found = std::ranges::find(actions, text, &MenuActionState::text);
    if (found == actions.end()) {
        return std::nullopt;
    }
    return *found;
}

} // namespace

TEST_CASE("layer editor follows stable ids through reorder and removal",
          "[ui][layers][inspector][characterization]")
{
    pci::SceneLayersDock panel;
    pci::LayerInspectorDock inspector;
    const pci::PointCloudLayer rgb =
        pointLayer(11,
                   "rgb.las",
                   {.source = pci::PointColorSource::Rgb,
                    .colorMap = pci::PointColorMap::Rgb});
    const pci::PointCloudLayer intensity =
        pointLayer(22,
                   "intensity.las",
                   {.source = pci::PointColorSource::Intensity,
                    .colorMap = pci::PointColorMap::Viridis});
    auto snapshot = makeSnapshot({rgb, intensity}, {}, {rgb.id, intensity.id});
    QObject::connect(&panel,
                     &pci::SceneLayersDock::selectionChanged,
                     [&inspector, &snapshot](const pci::SceneLayerId id) {
                         inspector.setDocumentSnapshot(snapshot, id);
                     });
    panel.setDocumentSnapshot(snapshot);
    panel.show();
    inspector.show();
    QTest::qWait(20);

    QListView &list = layerList(panel);
    auto *source =
        inspector.findChild<QComboBox *>(QStringLiteral("colorSourceComboBox"));
    REQUIRE(source != nullptr);
    list.setCurrentIndex(list.model()->index(1, 0));
    CHECK(panel.currentLayerId() == intensity.id);
    CHECK(source->currentData().toInt() ==
          static_cast<int>(pci::PointColorSource::Intensity));

    snapshot = makeSnapshot({rgb, intensity}, {}, {intensity.id, rgb.id});
    panel.setDocumentSnapshot(snapshot);
    CHECK(panel.currentLayerId() == intensity.id);
    CHECK(list.currentIndex().row() == 0);
    CHECK(source->currentData().toInt() ==
          static_cast<int>(pci::PointColorSource::Intensity));

    source->setFocus();
    REQUIRE(source->hasFocus());
    snapshot = makeSnapshot({rgb}, {}, {rgb.id});
    panel.setDocumentSnapshot(snapshot);
    CHECK(panel.currentLayerId() == rgb.id);
    CHECK(source->currentData().toInt() ==
          static_cast<int>(pci::PointColorSource::Rgb));
}

TEST_CASE("layer and inspector edits route the selected id after reordering",
          "[ui][layers][inspector][routing][characterization]")
{
    pci::SceneLayersDock panel;
    pci::LayerInspectorDock inspector;
    const pci::PointCloudLayer first =
        pointLayer(101,
                   "first.las",
                   {.source = pci::PointColorSource::Rgb,
                    .colorMap = pci::PointColorMap::Rgb});
    const pci::PointCloudLayer second =
        pointLayer(202,
                   "second.las",
                   {.source = pci::PointColorSource::Intensity,
                    .colorMap = pci::PointColorMap::Viridis});
    const auto snapshot =
        makeSnapshot({first, second}, {}, {second.id, first.id});
    QObject::connect(&panel,
                     &pci::SceneLayersDock::selectionChanged,
                     [&inspector, &snapshot](const pci::SceneLayerId id) {
                         inspector.setDocumentSnapshot(snapshot, id);
                     });
    panel.setDocumentSnapshot(snapshot);

    std::optional<std::pair<pci::PointCloudLayerId, bool>> visibility;
    std::optional<pci::PointCloudLayerId> removed;
    std::optional<pci::PointCloudLayerId> edited;
    QObject::connect(
        &panel,
        &pci::SceneLayersDock::visibilityToggled,
        [&visibility](const pci::PointCloudLayerId id, const bool visible) {
            visibility.emplace(id, visible);
        });
    QObject::connect(&panel,
                     &pci::SceneLayersDock::removeRequested,
                     [&removed](const pci::PointCloudLayerId id) {
                         removed = id;
                     });
    QObject::connect(
        &inspector,
        &pci::LayerInspectorDock::pointColorModeChanged,
        [&edited](const pci::PointCloudLayerId id, const pci::PointColorMode) {
            edited = id;
        });

    QListView &list = layerList(panel);
    list.model()->setData(
        list.model()->index(0, 0), Qt::Unchecked, Qt::CheckStateRole);
    CHECK(visibility == std::pair{second.id, false});
    list.setCurrentIndex(list.model()->index(1, 0));
    panel.removeCurrentLayer();
    CHECK(removed == first.id);

    auto *source =
        inspector.findChild<QComboBox *>(QStringLiteral("colorSourceComboBox"));
    REQUIRE(source != nullptr);
    source->setCurrentIndex(
        source->findData(static_cast<int>(pci::PointColorSource::Z)));
    CHECK(edited == first.id);
}

TEST_CASE("point and vector context menus retain kind-specific actions",
          "[ui][layers][context-menu][characterization]")
{
    pci::SceneLayersDock panel;
    const pci::PointCloudLayer points =
        pointLayer(3,
                   "points.las",
                   {.source = pci::PointColorSource::Rgb,
                    .colorMap = pci::PointColorMap::Rgb});
    const pci::VectorLayer vector = vectorLayer(4, "Boundaries", true);
    panel.setDocumentSnapshot(
        makeSnapshot({points}, {vector}, {points.id, vector.id}));
    panel.resize(360, 420);
    panel.show();
    QTest::qWait(20);

    QListView &list = layerList(panel);
    CHECK(list.selectionMode() == QAbstractItemView::SingleSelection);
    const std::vector<MenuActionState> pointActions =
        contextMenuActions(list, 0);
    const auto pointStatistics =
        findAction(pointActions, QStringLiteral("Point Cloud &Statistics…"));
    REQUIRE(pointStatistics.has_value());
    CHECK(pointStatistics->enabled);
    CHECK_FALSE(
        findAction(pointActions, QStringLiteral("Show anyway")).has_value());

    const std::vector<MenuActionState> vectorActions =
        contextMenuActions(list, 1);
    const auto vectorStatistics =
        findAction(vectorActions, QStringLiteral("Point Cloud &Statistics…"));
    REQUIRE(vectorStatistics.has_value());
    CHECK_FALSE(vectorStatistics->enabled);
    CHECK(findAction(vectorActions, QStringLiteral("Show anyway")).has_value());
    CHECK(
        findAction(vectorActions, QStringLiteral("Remove layer")).has_value());
}

TEST_CASE("layer panel sibling docks preserve names defaults and layout",
          "[ui][layers][docks][workspace][characterization]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("docks.ini")),
                       QSettings::IniFormat);
    QMainWindow window;
    pci::SceneLayersDock panel(&window);
    pci::LayerInspectorDock inspectorDock(&window);
    pci::TaskDock taskDock(&window);
    QDockWidget *inspector = &inspectorDock;
    QDockWidget *tasks = &taskDock;
    window.addDockWidget(Qt::LeftDockWidgetArea, &panel);
    window.addDockWidget(Qt::RightDockWidgetArea, inspector);
    window.addDockWidget(Qt::BottomDockWidgetArea, tasks);
    tasks->hide();
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    pci::DiagnosticsDock diagnosticsDock(&window);
    QDockWidget *diagnostics = &diagnosticsDock;
    window.addDockWidget(Qt::BottomDockWidgetArea, diagnostics);
    diagnostics->hide();
#endif
    window.show();
    QTest::qWait(20);

    CHECK(panel.objectName() == QStringLiteral("pointCloudLayerPanel"));
    CHECK(inspector->objectName() ==
          QStringLiteral("pointCloudInspectorPanel"));
    CHECK(tasks->objectName() == QStringLiteral("pointCloudTasksPanel"));
    CHECK(panel.isVisible());
    CHECK(inspector->isVisible());
    CHECK(tasks->isHidden());
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    CHECK(diagnostics->objectName() ==
          QStringLiteral("pointCloudDiagnosticsPanel"));
    CHECK(diagnostics->isHidden());
    CHECK(window.findChild<QWidget *>(
              QStringLiteral("pointCloudResidencyDiagnostics")) != nullptr);
#else
    CHECK(window.findChild<QWidget *>(
              QStringLiteral("pointCloudResidencyDiagnostics")) == nullptr);
#endif

    pci::WorkspaceSettings::save(window, settings);
    window.addDockWidget(Qt::LeftDockWidgetArea, inspector);
    tasks->show();
    pci::WorkspaceSettings::restore(window, settings);
    CHECK(window.dockWidgetArea(inspector) == Qt::RightDockWidgetArea);
    CHECK(tasks->isHidden());
}
