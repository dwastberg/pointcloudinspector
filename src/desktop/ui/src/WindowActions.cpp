#include "WindowActions.h"
#include <QMainWindow>
#include <pci/desktop/session/SceneSession.h>

#include "PointCloudStatisticsDialog.h"
#include "ToolbarIcons.h"
#include <pci/desktop/ui/ColorizeFromRasterDialog.h>
#include <pci/desktop/ui/LayerInspectorDock.h>
#include <pci/desktop/ui/LoadingOverlay.h>
#include <pci/desktop/ui/PerformanceSettingsStore.h>
#include <pci/desktop/ui/RenderDiagnosticsFormatter.h>
#include <pci/desktop/ui/SceneLayersDock.h>
#include <pci/desktop/ui/SettingsDialog.h>
#include <pci/desktop/ui/TaskDock.h>
#include <pci/desktop/ui/VectorSublayerDialog.h>
#include <pci/desktop/ui/ViewportSettingsStore.h>
#include <pci/desktop/ui/WorkspaceSettings.h>
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
#include <pci/desktop/ui/DiagnosticsDock.h>
#endif
#include <pci/adapters/platform/QtPath.h>
#include <pci/desktop/config/SupportedSource.h>
#include <pci/desktop/operations/PointCloudLoadController.h>
#include <pci/desktop/operations/VectorLoadController.h>
#include <pci/desktop/viewport/RenderViewport.h>
#include <pci/foundation/CheckedArithmetic.h>

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
#include <QApplication>
#include <QDebug>
#endif
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedLayout>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVariant>
#include <QWidget>

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace pci {
WindowActions::WindowActions(QMainWindow &window,
                             SceneSession &session,
                             RenderViewport &viewport,
                             SceneLayersDock &layers,
                             LayerInspectorDock &inspector,
                             TaskDock &tasks,
                             QLabel &navigationHint,
                             WindowActionCallbacks callbacks,
                             DiagnosticsDock *diagnostics)
    : QObject(&window)
    , window_(window)
    , session_(session)
    , viewport_(viewport)
    , sceneLayersDock_(&layers)
    , layerInspectorDock_(&inspector)
    , taskDock_(&tasks)
    ,
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    diagnosticsDock_(diagnostics)
    ,
#endif
    navigationHintLabel_(&navigationHint)
    , callbacks_(std::move(callbacks))
{
    static_cast<void>(diagnostics);
    createFileActions();
    createEditActions();
    createViewActions();
    createLayerActions();
    createToolActions();
    createHelpActions();
    createToolbars();
}
void WindowActions::createFileActions()
{
    QMenu *fileMenu = window_.menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->setObjectName(QStringLiteral("fileMenu"));
    newSceneAction_ = fileMenu->addAction(QStringLiteral("&New Scene"));
    newSceneAction_->setObjectName(QStringLiteral("newSceneAction"));
    newSceneAction_->setIconText(QStringLiteral("New"));
    newSceneAction_->setIcon(toolbarIcon(ToolbarIcon::New, window_.palette()));
    newSceneAction_->setShortcut(QKeySequence::New);
    newSceneAction_->setToolTip(
        QStringLiteral("Remove all loaded items and start a new empty scene "
                       "(%1)")
            .arg(newSceneAction_->shortcut().toString(
                QKeySequence::NativeText)));
    connect(newSceneAction_,
            &QAction::triggered,
            &session_,
            &SceneSession::newScene);

    openAction_ = fileMenu->addAction(QStringLiteral("&Open Files…"));
    openAction_->setObjectName(QStringLiteral("openFilesAction"));
    openAction_->setIconText(QStringLiteral("Open…"));
    openAction_->setIcon(toolbarIcon(ToolbarIcon::Open, window_.palette()));
    openAction_->setShortcut(QKeySequence::Open);
    openAction_->setToolTip(
        QStringLiteral("Open point-cloud, vector, or raster files (%1)")
            .arg(openAction_->shortcut().toString(QKeySequence::NativeText)));
    connect(openAction_, &QAction::triggered, this, [this] {
        callbacks_.chooseSources();
    });

    fileMenu->addSeparator();
    QAction *cancelAction =
        fileMenu->addAction(QStringLiteral("Cancel Loading"));
    cancelAction->setObjectName(QStringLiteral("cancelLoadingAction"));
    connect(cancelAction, &QAction::triggered, this, [this] {
        callbacks_.cancelLoading();
    });
}
void WindowActions::createEditActions()
{
    QMenu *editMenu = window_.menuBar()->addMenu(QStringLiteral("&Edit"));
    editMenu->setObjectName(QStringLiteral("editMenu"));
    QAction *selectAllLayersAction =
        editMenu->addAction(QStringLiteral("Select All Layers"));
    selectAllLayersAction->setObjectName(
        QStringLiteral("selectAllLayersAction"));
    selectAllLayersAction->setShortcut(QKeySequence::SelectAll);
    connect(selectAllLayersAction,
            &QAction::triggered,
            sceneLayersDock_,
            &SceneLayersDock::selectAllLayers);
    editMenu->addSeparator();
    settingsAction_ = editMenu->addAction(QStringLiteral("&Settings…"));
    settingsAction_->setObjectName(QStringLiteral("settingsAction"));
    settingsAction_->setMenuRole(QAction::PreferencesRole);
    settingsAction_->setIconText(QStringLiteral("Settings…"));
    settingsAction_->setIcon(
        toolbarIcon(ToolbarIcon::Settings, window_.palette()));
    settingsAction_->setToolTip(QStringLiteral("Open application settings"));
    connect(settingsAction_, &QAction::triggered, this, [this] {
        callbacks_.showSettings();
    });
}
void WindowActions::createViewActions()
{
    QMenu *viewMenu = window_.menuBar()->addMenu(QStringLiteral("&View"));
    viewMenu->setObjectName(QStringLiteral("viewMenu"));

    fitSceneAction_ = viewMenu->addAction(QStringLiteral("&Fit Scene"));
    fitSceneAction_->setObjectName(QStringLiteral("fitSceneAction"));
    fitSceneAction_->setIconText(QStringLiteral("Fit"));
    fitSceneAction_->setIcon(toolbarIcon(ToolbarIcon::Fit, window_.palette()));
    fitSceneAction_->setShortcut(QKeySequence(QStringLiteral("F")));
    fitSceneAction_->setShortcutContext(Qt::ApplicationShortcut);
    fitSceneAction_->setToolTip(
        QStringLiteral("Fit all visible layers in the viewport (F)"));
    connect(fitSceneAction_, &QAction::triggered, this, [this] {
        viewport_.frameVisibleLayers();
    });

    topDownSceneAction_ = viewMenu->addAction(QStringLiteral("Top Down Scene"));
    topDownSceneAction_->setObjectName(QStringLiteral("topDownSceneAction"));
    topDownSceneAction_->setIconText(QStringLiteral("Top Down"));
    topDownSceneAction_->setIcon(
        toolbarIcon(ToolbarIcon::TopDown, window_.palette()));
    topDownSceneAction_->setShortcut(QKeySequence(QStringLiteral("7")));
    topDownSceneAction_->setShortcutContext(Qt::ApplicationShortcut);
    topDownSceneAction_->setToolTip(
        QStringLiteral("Fit all visible layers in a top-down view (7)"));
    connect(topDownSceneAction_, &QAction::triggered, this, [this] {
        viewport_.frameVisibleLayersTopDown();
    });

    mapViewAction_ = viewMenu->addAction(QStringLiteral("2D Map View"));
    mapViewAction_->setObjectName(QStringLiteral("mapViewAction"));
    mapViewAction_->setIconText(QStringLiteral("2D Map"));
    mapViewAction_->setIcon(toolbarIcon(ToolbarIcon::Map, window_.palette()));
    mapViewAction_->setCheckable(true);
    mapViewAction_->setChecked(viewport_.isMapView());
    mapViewAction_->setToolTip(QStringLiteral(
        "Use a locked top-down orthographic view with GIS-style controls"));

    orthographicAction_ =
        viewMenu->addAction(QStringLiteral("Orthographic Camera"));
    orthographicAction_->setObjectName(
        QStringLiteral("orthographicCameraAction"));
    orthographicAction_->setIconText(QStringLiteral("Orthographic"));
    orthographicAction_->setIcon(
        toolbarIcon(ToolbarIcon::Orthographic, window_.palette()));
    orthographicAction_->setCheckable(true);
    orthographicAction_->setChecked(viewport_.isOrthographic());
    orthographicAction_->setEnabled(!viewport_.isMapView());
    orthographicAction_->setToolTip(QStringLiteral(
        "Toggle between perspective and orthographic camera projection"));
    connect(orthographicAction_,
            &QAction::toggled,
            this,
            [this](const bool enabled) {
                viewport_.setOrthographic(enabled);
            });
    connect(
        mapViewAction_, &QAction::toggled, this, [this](const bool enabled) {
            viewport_.setMapView(enabled);
            {
                const QSignalBlocker blocker(orthographicAction_);
                orthographicAction_->setChecked(viewport_.isOrthographic());
            }
            orthographicAction_->setEnabled(!enabled);
            navigationHintLabel_->setText(
                enabled
                    ? QStringLiteral("Pan  Left-drag    Move  W / A / S / D    "
                                     "Zoom  Wheel / Q / Z    Fit  F")
                    : QStringLiteral("Orbit  Left-drag    Pan  Right-drag    "
                                     "Zoom  Wheel    Pivot  Double-click    "
                                     "Measure  M    Fit  F"));
            window_.statusBar()->showMessage(
                enabled ? QStringLiteral(
                              "2D Map View: left-drag or WASD to move; wheel "
                              "or Q (in) / Z (out) to zoom.")
                        : QStringLiteral("3D navigation enabled"),
                3000);
        });

    QMenu *panelsMenu = viewMenu->addMenu(QStringLiteral("&Panels"));
    panelsMenu->setObjectName(QStringLiteral("panelsMenu"));
    QAction *toggleLayerPanelAction = sceneLayersDock_->toggleViewAction();
    toggleLayerPanelAction->setObjectName(
        QStringLiteral("pointCloudLayerPanelToggleAction"));
    toggleLayerPanelAction->setText(QStringLiteral("&Scene"));
    toggleLayerPanelAction->setIcon(
        window_.style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    toggleLayerPanelAction->setToolTip(
        QStringLiteral("Show or hide the Scene panel"));
    panelsMenu->addAction(toggleLayerPanelAction);

    QAction *toggleInspectorAction = layerInspectorDock_->toggleViewAction();
    toggleInspectorAction->setObjectName(
        QStringLiteral("pointCloudInspectorPanelToggleAction"));
    toggleInspectorAction->setText(QStringLiteral("&Inspector"));
    panelsMenu->addAction(toggleInspectorAction);

    QAction *toggleTasksAction = taskDock_->toggleViewAction();
    toggleTasksAction->setObjectName(
        QStringLiteral("pointCloudTasksPanelToggleAction"));
    toggleTasksAction->setText(QStringLiteral("&Tasks"));
    panelsMenu->addAction(toggleTasksAction);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    QAction *toggleDiagnosticsAction = diagnosticsDock_->toggleViewAction();
    toggleDiagnosticsAction->setObjectName(
        QStringLiteral("pointCloudDiagnosticsPanelToggleAction"));
    toggleDiagnosticsAction->setText(QStringLiteral("&Diagnostics"));
    panelsMenu->addAction(toggleDiagnosticsAction);
#endif

    viewMenu->addSeparator();

    eyeDomeLightingAction_ =
        viewMenu->addAction(QStringLiteral("&Eye-Dome Lighting"));
    eyeDomeLightingAction_->setObjectName(
        QStringLiteral("eyeDomeLightingAction"));
    eyeDomeLightingAction_->setCheckable(true);
    eyeDomeLightingAction_->setChecked(viewport_.eyeDomeLightingEnabled());
    eyeDomeLightingAction_->setToolTip(
        QStringLiteral("Enhance point-cloud depth perception"));
    connect(eyeDomeLightingAction_,
            &QAction::toggled,
            this,
            [this](const bool enabled) {
                viewport_.setEyeDomeLightingEnabled(enabled);
            });
}
void WindowActions::createLayerActions()
{
    QMenu *layerMenu = window_.menuBar()->addMenu(QStringLiteral("&Layer"));
    layerMenu->setObjectName(QStringLiteral("layerMenu"));
    fitSelectedLayerAction_ =
        layerMenu->addAction(QStringLiteral("&Fit Selected Layer"));
    fitSelectedLayerAction_->setObjectName(
        QStringLiteral("fitSelectedLayerAction"));
    connect(fitSelectedLayerAction_,
            &QAction::triggered,
            sceneLayersDock_,
            &SceneLayersDock::fitCurrentLayer);
    isolateSelectedLayerAction_ =
        layerMenu->addAction(QStringLiteral("&Isolate Selected Layer"));
    isolateSelectedLayerAction_->setObjectName(
        QStringLiteral("isolateSelectedLayerAction"));
    connect(isolateSelectedLayerAction_,
            &QAction::triggered,
            sceneLayersDock_,
            &SceneLayersDock::isolateCurrentLayer);
    showAllLayersAction_ =
        layerMenu->addAction(QStringLiteral("Show &All Layers"));
    showAllLayersAction_->setObjectName(QStringLiteral("showAllLayersAction"));
    connect(showAllLayersAction_,
            &QAction::triggered,
            sceneLayersDock_,
            &SceneLayersDock::showAll);
    layerMenu->addSeparator();
    layerMenu->addAction(sceneLayersDock_->statisticsAction());
    layerMenu->addAction(sceneLayersDock_->colorizeAction());
    layerMenu->addAction(sceneLayersDock_->revertColorsAction());
    layerMenu->addSeparator();
    removeSelectedLayerAction_ =
        layerMenu->addAction(QStringLiteral("&Remove Selected Layer"));
    removeSelectedLayerAction_->setObjectName(
        QStringLiteral("removeSelectedLayerAction"));
    removeSelectedLayerAction_->setShortcut(QKeySequence::Delete);
    connect(removeSelectedLayerAction_,
            &QAction::triggered,
            sceneLayersDock_,
            &SceneLayersDock::removeCurrentLayer);
}
void WindowActions::createToolActions()
{
    QMenu *toolsMenu = window_.menuBar()->addMenu(QStringLiteral("&Tools"));
    toolsMenu->setObjectName(QStringLiteral("toolsMenu"));
    navigateToolAction_ = toolsMenu->addAction(QStringLiteral("&Navigate"));
    navigateToolAction_->setObjectName(QStringLiteral("navigateToolAction"));
    navigateToolAction_->setIcon(
        toolbarIcon(ToolbarIcon::Navigate, window_.palette()));
    navigateToolAction_->setCheckable(true);
    navigateToolAction_->setChecked(true);
    navigateToolAction_->setShortcut(QKeySequence(QStringLiteral("N")));
    navigateToolAction_->setShortcutContext(Qt::ApplicationShortcut);
    navigateToolAction_->setToolTip(QStringLiteral(
        "Navigate the viewport with orbit, pan, zoom, and fly controls"));
    measureToolAction_ = toolsMenu->addAction(QStringLiteral("&Measure"));
    measureToolAction_->setObjectName(QStringLiteral("measureToolAction"));
    measureToolAction_->setIcon(
        toolbarIcon(ToolbarIcon::Measure, window_.palette()));
    measureToolAction_->setCheckable(true);
    measureToolAction_->setShortcut(QKeySequence(QStringLiteral("M")));
    measureToolAction_->setShortcutContext(Qt::ApplicationShortcut);
    measureToolAction_->setToolTip(
        QStringLiteral("Measure between snapped visible points (M)"));
    auto *toolActionGroup = new QActionGroup(this);
    toolActionGroup->setExclusive(true);
    toolActionGroup->addAction(navigateToolAction_);
    toolActionGroup->addAction(measureToolAction_);
    connect(navigateToolAction_,
            &QAction::toggled,
            this,
            [this](const bool checked) {
                if (checked) {
                    viewport_.setActiveTool(ViewportTool::Navigate);
                }
            });
    connect(measureToolAction_,
            &QAction::toggled,
            this,
            [this](const bool checked) {
                if (checked) {
                    viewport_.setActiveTool(ViewportTool::Measure);
                    window_.statusBar()->showMessage(
                        QStringLiteral("Measure: hover to snap, click two "
                                       "points; drag to orbit; Esc clears."));
                }
            });
}
void WindowActions::createHelpActions()
{
    QMenu *helpMenu = window_.menuBar()->addMenu(QStringLiteral("&Help"));
    helpMenu->setObjectName(QStringLiteral("helpMenu"));
    QAction *controlsAction =
        helpMenu->addAction(QStringLiteral("&Viewport Controls"));
    controlsAction->setObjectName(QStringLiteral("viewportControlsAction"));
    controlsAction->setShortcut(QKeySequence(QStringLiteral("?")));
    connect(controlsAction, &QAction::triggered, this, [this] {
        callbacks_.showControlsReference();
    });
}
void WindowActions::createToolbars()
{
    QToolBar *pointCloudToolBar =
        window_.addToolBar(QStringLiteral("Commands"));
    pointCloudToolBar->setObjectName(QStringLiteral("pointCloudToolBar"));
    pointCloudToolBar->setMovable(false);
    pointCloudToolBar->setIconSize(QSize(20, 20));
    pointCloudToolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    pointCloudToolBar->addAction(newSceneAction_);
    pointCloudToolBar->addAction(openAction_);
    pointCloudToolBar->addSeparator();
    pointCloudToolBar->addAction(fitSceneAction_);
    pointCloudToolBar->addAction(topDownSceneAction_);
    pointCloudToolBar->addAction(mapViewAction_);
    pointCloudToolBar->addAction(orthographicAction_);
    pointCloudToolBar->addSeparator();
    auto *pointSizeSpinBox = new QSpinBox(pointCloudToolBar);
    pointSizeSpinBox->setObjectName(QStringLiteral("pointSizeSpinBox"));
    pointSizeSpinBox->setRange(minimumPointSizePixels, maximumPointSizePixels);
    pointSizeSpinBox->setValue(viewport_.pointSizePixels());
    pointSizeSpinBox->setPrefix(QStringLiteral("Point size  "));
    pointSizeSpinBox->setSuffix(QStringLiteral(" px"));
    pointSizeSpinBox->setToolTip(QStringLiteral("Set the rendered point size"));
    pointSizeSpinBox->setAccessibleName(QStringLiteral("Point size"));
    pointCloudToolBar->addWidget(pointSizeSpinBox);
    connect(pointSizeSpinBox,
            &QSpinBox::valueChanged,
            this,
            [this](const int pointSize) {
                viewport_.setPointSizePixels(pointSize);
            });
    auto *eyeDomeLightingCheckBox =
        new QCheckBox(QStringLiteral("Depth enhancement"), pointCloudToolBar);
    eyeDomeLightingCheckBox->setObjectName(
        QStringLiteral("eyeDomeLightingCheckBox"));
    eyeDomeLightingCheckBox->setChecked(eyeDomeLightingAction_->isChecked());
    eyeDomeLightingCheckBox->setToolTip(eyeDomeLightingAction_->toolTip());
    pointCloudToolBar->addWidget(eyeDomeLightingCheckBox);
    connect(eyeDomeLightingCheckBox,
            &QCheckBox::toggled,
            eyeDomeLightingAction_,
            &QAction::setChecked);
    connect(eyeDomeLightingAction_,
            &QAction::toggled,
            eyeDomeLightingCheckBox,
            &QCheckBox::setChecked);

    auto *toolbarSpacer = new QWidget(pointCloudToolBar);
    toolbarSpacer->setObjectName(QStringLiteral("commandToolbarSpacer"));
    toolbarSpacer->setSizePolicy(QSizePolicy::Expanding,
                                 QSizePolicy::Preferred);
    pointCloudToolBar->addWidget(toolbarSpacer);
    pointCloudToolBar->addAction(settingsAction_);

    auto *toolRail = new QToolBar(QStringLiteral("Viewport tools"), &window_);
    toolRail->setObjectName(QStringLiteral("viewportToolRail"));
    toolRail->setOrientation(Qt::Vertical);
    toolRail->setMovable(false);
    toolRail->setIconSize(QSize(20, 20));
    toolRail->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    toolRail->addAction(navigateToolAction_);
    toolRail->addAction(measureToolAction_);
    window_.addToolBar(Qt::LeftToolBarArea, toolRail);
}
} // namespace pci
