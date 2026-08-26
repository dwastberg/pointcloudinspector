#include "app/DiagnosticsDock.h"
#include "app/LayerInspectorDock.h"
#include "app/SceneLayersDock.h"
#include "app/TaskDock.h"
#include "app/WorkspaceSettings.h"
#include "support/TestPointColorMaps.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListView>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardItemModel>
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
    pci::LayerInspectorDock inspector(
        nullptr, pci::test::createTestPointColorMapCatalog());
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
    pci::LayerInspectorDock inspector(
        nullptr, pci::test::createTestPointColorMapCatalog());
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

namespace {

class InspectorRasterSource final : public pci::RasterTileSource {
public:
    explicit InspectorRasterSource(pci::RasterLayerMetadata metadata)
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
        throw pci::RasterReadError("the inspector fixture holds no pixels");
    }

private:
    pci::RasterLayerMetadata metadata_;
};

[[nodiscard]] pci::SceneDocumentSnapshotPtr
rasterInspectorSnapshot(const pci::RasterSampleKind kind,
                        const bool visible = true)
{
    pci::RasterLayerMetadata metadata;
    metadata.sourcePath = "/data/terrain.tif";
    metadata.sourceDriver = "GTiff";
    metadata.width = 2048;
    metadata.height = 1024;
    metadata.geoTransform = {674000.0, 0.5, 0.0, 6580000.0, 0.0, -0.5};
    metadata.spatialReferenceWkt = "STUBCRS";
    metadata.bounds = *pci::rasterPixelEdgeBounds(
        metadata.geoTransform, metadata.width, metadata.height);
    metadata.bands.push_back({.band = 1});
    metadata.defaultDisplay.sampleKind = kind;
    metadata.defaultDisplay.displayRange = pci::RasterDisplayRange{
        .minimum = 12.5,
        .maximum = 340.0,
        .origin = pci::RasterDisplayRange::Origin::Sampled};
    pci::RasterLevel base;
    base.width = metadata.width;
    base.height = metadata.height;
    base.channelCount = 1;
    metadata.levels.push_back(base);

    auto data = std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = std::make_shared<InspectorRasterSource>(std::move(metadata)),
    });
    auto snapshot = std::make_shared<pci::SceneDocumentSnapshot>();
    snapshot->layers.push_back(
        {.id = pci::SceneLayerId{1},
         .visible = visible,
         .payload = pci::RasterLayerState{
             .data = std::move(data),
             .style = pci::RasterLayerStyle{.opacity = 0.6F, .zOffset = 7.5}}});
    return snapshot;
}

} // namespace

TEST_CASE("layer inspector shows raster placement and metadata",
          "[ui][inspector][raster]")
{
    pci::LayerInspectorDock inspector(
        nullptr, pci::test::createTestPointColorMapCatalog());
    inspector.setDocumentSnapshot(
        rasterInspectorSnapshot(pci::RasterSampleKind::ContinuousScalar),
        pci::SceneLayerId{1});

    auto *panel =
        inspector.findChild<QWidget *>(QStringLiteral("rasterLayerProperties"));
    REQUIRE(panel != nullptr);
    CHECK(panel->isVisibleTo(&inspector));
    auto *vectorPanel =
        inspector.findChild<QWidget *>(QStringLiteral("vectorLayerProperties"));
    REQUIRE(vectorPanel != nullptr);
    CHECK_FALSE(vectorPanel->isVisibleTo(&inspector));

    auto *opacity = inspector.findChild<QDoubleSpinBox *>(
        QStringLiteral("rasterOpacitySpinBox"));
    auto *offset = inspector.findChild<QDoubleSpinBox *>(
        QStringLiteral("rasterZOffsetSpinBox"));
    REQUIRE(opacity != nullptr);
    REQUIRE(offset != nullptr);
    CHECK(opacity->value() == Catch::Approx(60.0));
    CHECK(offset->value() == Catch::Approx(7.5));

    auto *dimensions =
        inspector.findChild<QLabel *>(QStringLiteral("rasterDimensionsValue"));
    auto *overviews =
        inspector.findChild<QLabel *>(QStringLiteral("rasterOverviewsValue"));
    auto *range =
        inspector.findChild<QLabel *>(QStringLiteral("rasterRangeValue"));
    REQUIRE(dimensions != nullptr);
    CHECK(dimensions->text() == QStringLiteral("2048 x 1024"));
    // A base-only level table has no overviews to report.
    CHECK(overviews->text() == QStringLiteral("None"));
    // Provenance is shown because a sampled range is an estimate, not the
    // dataset's declared extremes.
    CHECK(range->text().contains(QStringLiteral("bounded sample")));
}

TEST_CASE("layer inspector offers range and ramp only for scalar rasters",
          "[ui][inspector][raster]")
{
    pci::LayerInspectorDock inspector(
        nullptr, pci::test::createTestPointColorMapCatalog());

    auto *rangeWidget =
        inspector.findChild<QWidget *>(QStringLiteral("rasterRangeWidget"));
    auto *ramp = inspector.findChild<QComboBox *>(
        QStringLiteral("rasterColorRampCombo"));
    REQUIRE(rangeWidget != nullptr);
    REQUIRE(ramp != nullptr);

    inspector.setDocumentSnapshot(
        rasterInspectorSnapshot(pci::RasterSampleKind::ContinuousScalar),
        pci::SceneLayerId{1});
    CHECK(rangeWidget->isVisibleTo(inspector.widget()));
    CHECK(ramp->isVisibleTo(inspector.widget()));

    // An RGB source has no single meaningful range, so the controls are not
    // offered rather than shown inert.
    inspector.setDocumentSnapshot(
        rasterInspectorSnapshot(pci::RasterSampleKind::ContinuousColor),
        pci::SceneLayerId{1});
    CHECK_FALSE(rangeWidget->isVisibleTo(inspector.widget()));
    CHECK_FALSE(ramp->isVisibleTo(inspector.widget()));
}

TEST_CASE("layer inspector edits raster opacity and elevation",
          "[ui][inspector][raster]")
{
    pci::LayerInspectorDock inspector(
        nullptr, pci::test::createTestPointColorMapCatalog());
    inspector.setDocumentSnapshot(
        rasterInspectorSnapshot(pci::RasterSampleKind::ContinuousColor),
        pci::SceneLayerId{1});

    QSignalSpy styleChanged(&inspector,
                            &pci::LayerInspectorDock::rasterStyleChanged);
    auto *opacity = inspector.findChild<QDoubleSpinBox *>(
        QStringLiteral("rasterOpacitySpinBox"));
    opacity->setValue(25.0);
    REQUIRE_FALSE(styleChanged.empty());
    CHECK(styleChanged.back().at(1).value<pci::RasterLayerStyle>().opacity ==
          Catch::Approx(0.25F));

    // Reset returns the layer to the documented default of z = 0.
    auto *reset = inspector.findChild<QPushButton *>(
        QStringLiteral("rasterResetElevationButton"));
    REQUIRE(reset != nullptr);
    reset->click();
    CHECK(styleChanged.back().at(1).value<pci::RasterLayerStyle>().zOffset ==
          Catch::Approx(0.0));
}

TEST_CASE("layer inspector exposes true-elevation Surface controls",
          "[ui][inspector][raster][surface]")
{
    pci::LayerInspectorDock inspector(
        nullptr, pci::test::createTestPointColorMapCatalog());
    inspector.setRasterSurfaceCapability(
        pci::RasterSurfaceCapability::Supported);
    pci::SceneDocumentSnapshotPtr snapshot =
        rasterInspectorSnapshot(pci::RasterSampleKind::ContinuousScalar);
    auto &state = const_cast<pci::RasterLayerState &>(
        std::get<pci::RasterLayerState>(snapshot->layers[0].payload));
    auto &metadata =
        const_cast<pci::RasterLayerMetadata &>(state.data->metadata());
    metadata.elevation.available = true;
    metadata.elevation.band = 1;
    metadata.elevation.unit = "m";
    state.style.renderMode = pci::RasterRenderMode::Surface;
    state.style.verticalExaggeration = 2.5;
    state.style.surfaceShadingStrength = 0.75F;
    state.elevationStatus = pci::RasterElevationStatus::Ready;
    state.exactElevationRange =
        pci::RasterElevationRange{.minimum = 10.0, .maximum = 90.0};
    inspector.setDocumentSnapshot(snapshot, pci::SceneLayerId{1});

    auto *section = inspector.findChild<QWidget *>(
        QStringLiteral("rasterRenderingSection"));
    auto *mode = inspector.findChild<QComboBox *>(
        QStringLiteral("rasterRenderModeCombo"));
    auto *exaggeration = inspector.findChild<QDoubleSpinBox *>(
        QStringLiteral("rasterVerticalExaggerationSpinBox"));
    auto *shading = inspector.findChild<QDoubleSpinBox *>(
        QStringLiteral("rasterSurfaceShadingSpinBox"));
    auto *status = inspector.findChild<QLabel *>(
        QStringLiteral("rasterElevationStatusLabel"));
    REQUIRE(section != nullptr);
    REQUIRE(mode != nullptr);
    REQUIRE(exaggeration != nullptr);
    REQUIRE(shading != nullptr);
    REQUIRE(status != nullptr);
    CHECK(section->isVisibleTo(inspector.widget()));
    CHECK(mode->currentData().toInt() ==
          static_cast<int>(pci::RasterRenderMode::Surface));
    CHECK(exaggeration->value() == Catch::Approx(2.5));
    CHECK(shading->value() == Catch::Approx(75.0));
    CHECK(status->text().contains(QStringLiteral("10")));
    CHECK(status->text().contains(QStringLiteral("90")));
    CHECK(status->text().contains(QStringLiteral("m")));

    QSignalSpy styleChanged(&inspector,
                            &pci::LayerInspectorDock::rasterStyleChanged);
    exaggeration->setValue(4.0);
    REQUIRE_FALSE(styleChanged.empty());
    CHECK(styleChanged.back()
              .at(1)
              .value<pci::RasterLayerStyle>()
              .verticalExaggeration == Catch::Approx(4.0));

    inspector.setRasterSurfaceCapability(
        pci::RasterSurfaceCapability::Unsupported,
        QStringLiteral("No R32F support"));
    CHECK(status->text().contains(QStringLiteral("No R32F support")));
    auto *model = qobject_cast<QStandardItemModel *>(mode->model());
    REQUIRE(model != nullptr);
    CHECK_FALSE(model->item(mode->findData(
                                static_cast<int>(pci::RasterRenderMode::Surface)))
                    ->isEnabled());
}

TEST_CASE("layer inspector warns when a raster needs tiled rendering",
          "[ui][inspector][raster]")
{
    pci::LayerInspectorDock inspector(
        nullptr, pci::test::createTestPointColorMapCatalog());
    pci::SceneDocumentSnapshotPtr snapshot =
        rasterInspectorSnapshot(pci::RasterSampleKind::ContinuousColor);
    auto &state = std::get<pci::RasterLayerState>(snapshot->layers[0].payload);
    const_cast<pci::RasterLayerMetadata &>(state.data->metadata())
        .insufficientOverviews = true;
    inspector.setDocumentSnapshot(snapshot, pci::SceneLayerId{1});

    auto *warning = inspector.findChild<QWidget *>(
        QStringLiteral("rasterVisibilityWarning"));
    auto *label = inspector.findChild<QLabel *>(
        QStringLiteral("rasterVisibilityWarningLabel"));
    auto *showAnyway = inspector.findChild<QPushButton *>(
        QStringLiteral("rasterShowAnywayButton"));
    REQUIRE(warning != nullptr);
    CHECK(warning->isVisibleTo(inspector.widget()));
    CHECK(label->text().contains(
        QStringLiteral("automatic low-resolution preview")));
    // Overviews are a data problem, not a visibility one, so no Show anyway.
    CHECK_FALSE(showAnyway->isVisibleTo(warning));
}
