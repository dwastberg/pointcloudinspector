#include "app/MainWindow.h"

#include "app/ColorizeFromRasterDialog.h"
#include "app/LayerInspectorDock.h"
#include "app/LoadingOverlay.h"
#include "app/PerformanceSettingsStore.h"
#include "app/PointCloudStatisticsDialog.h"
#include "app/RenderDiagnosticsFormatter.h"
#include "app/SceneLayersDock.h"
#include "app/SettingsDialog.h"
#include "app/TaskDock.h"
#include "app/ToolbarIcons.h"
#include "app/VectorSublayerDialog.h"
#include "app/ViewportSettingsStore.h"
#include "app/WorkspaceSettings.h"
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
#include "app/DiagnosticsDock.h"
#endif
#include "foundation/CheckedArithmetic.h"
#include "import/PointCloudLoadController.h"
#include "import/SupportedSource.h"
#include "import/VectorLoadController.h"
#include "platform/QtPath.h"
#include "renderer/RenderViewport.h"

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
namespace {

constexpr std::uint64_t bytesPerMebibyte = std::uint64_t{1024} * 1024;

[[nodiscard]] std::vector<std::filesystem::path>
localDroppedFiles(const QMimeData &mimeData)
{
    std::vector<std::filesystem::path> paths;
    if (!mimeData.hasUrls()) {
        return paths;
    }
    paths.reserve(static_cast<std::size_t>(mimeData.urls().size()));
    for (const QUrl &url : mimeData.urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString localPath = url.toLocalFile();
        if (!QFileInfo(localPath).isFile()) {
            continue;
        }
        paths.push_back(qStringToPath(localPath));
    }
    return paths;
}

QString loadingProgressDetails(const LoadingProgressState &state)
{
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    switch (state.phase) {
    case LoadingProgressPhase::Reading:
        return QStringLiteral("Reading point cloud… %1 / %2 source points")
            .arg(state.completed)
            .arg(state.total);
    case LoadingProgressPhase::Optimizing:
        return QStringLiteral("Optimizing points…");
    case LoadingProgressPhase::PreparingRenderer:
        return QStringLiteral("Preparing renderer…");
    case LoadingProgressPhase::Uploading:
        return QStringLiteral("Uploading points to GPU… %1 / %2")
            .arg(state.completed)
            .arg(state.total);
    case LoadingProgressPhase::FirstFrameReady:
        return QStringLiteral("Displaying preview…");
    case LoadingProgressPhase::DisplayReady:
        return QStringLiteral("Displaying point cloud…");
    }
    return {};
#else
    switch (state.phase) {
    case LoadingProgressPhase::Reading:
        return QStringLiteral("Reading point cloud…");
    case LoadingProgressPhase::Optimizing:
    case LoadingProgressPhase::PreparingRenderer:
    case LoadingProgressPhase::Uploading:
        return QStringLiteral("Preparing point cloud…");
    case LoadingProgressPhase::FirstFrameReady:
    case LoadingProgressPhase::DisplayReady:
        return QStringLiteral("Displaying point cloud…");
    }
    return {};
#endif
}

#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
RenderMetrics withDocumentMetrics(RenderMetrics metrics,
                                  const SceneDocument &document)
{
    const SceneDocumentMetrics hierarchy = document.hierarchyMetrics();
    metrics.retainedFlatBytes = hierarchy.retainedFlatBytes;
    metrics.decodedPointBudgetBytes = hierarchy.memoryBudget.byteBudget;
    metrics.peakDecodedPointBytes =
        std::max(hierarchy.cache.peakResidentBytes,
                 hierarchy.memoryBudget.peakReservedBytes);
    metrics.cacheHits = hierarchy.cache.hits;
    metrics.cacheMisses = hierarchy.cache.misses;
    metrics.cacheEvictions = hierarchy.cache.evictions;
    metrics.sourceRequests = hierarchy.source.requests;
    metrics.sourceRequestsCompleted = hierarchy.source.completed;
    metrics.sourceRequestsCancelled = hierarchy.source.cancelled;
    metrics.sourceRequestsFailed = hierarchy.source.failed;
    metrics.estimatedSourceBytesRequested =
        hierarchy.source.estimatedDecodedBytesRequested;
    metrics.decodedSourceBytesProduced = hierarchy.source.decodedBytesProduced;
    metrics.decodeRequestsQueued = hierarchy.decodeRequestsQueued;
    metrics.decodeRequestsStarted = hierarchy.decodeRequestsStarted;
    metrics.decodeRequestsCompleted = hierarchy.decodeRequestsCompleted;
    metrics.decodeRequestsCancelled = hierarchy.decodeRequestsCancelled;
    metrics.decodeRequestsFailed = hierarchy.decodeRequestsFailed;
    metrics.activeDecoderEstimatedBytes =
        hierarchy.scheduler.activeEstimatedBytes;
    metrics.peakDecoderEstimatedBytes =
        hierarchy.scheduler.peakActiveEstimatedBytes;
    metrics.activeDecodes =
        static_cast<std::uint64_t>(hierarchy.scheduler.active);
    metrics.pendingDecodes =
        static_cast<std::uint64_t>(hierarchy.scheduler.pending);
    metrics.persistentIndexBytes = hierarchy.persistentIndexBytes;
    metrics.localPersistentSources = hierarchy.localPersistentSources;
    metrics.reusedPersistentSources = hierarchy.reusedPersistentSources;
    metrics.activeColorTableBytes = hierarchy.activeColorTableBytes;
    metrics.flatDisplacedColorBytes = hierarchy.flatDisplacedColorBytes;
    metrics.retainedSourceRootBytes = hierarchy.retainedSourceRootBytes;
    metrics.retainedColoredRootBytes = hierarchy.retainedColoredRootBytes;
    return metrics;
}
#endif

} // namespace

MainWindow::MainWindow(
    std::unique_ptr<RenderViewport> viewport,
    ImportServices importServices,
    const std::uint64_t maximumLoadPoints,
    const std::uint64_t decodedByteBudget,
    std::optional<AutomaticMemoryBudgetParameters> automaticMemoryBudget,
    LocalPageCacheContextPtr localPageCache,
    PointColorMapCatalogSnapshotPtr colorMaps)
    : viewport_(std::move(viewport))
    , session_(std::make_unique<SceneSession>(std::move(importServices),
                                              maximumLoadPoints,
                                              decodedByteBudget,
                                              automaticMemoryBudget,
                                              std::move(localPageCache),
                                              colorMaps))
{
    if (!viewport_) {
        throw std::invalid_argument("main window requires a viewport");
    }
    performanceSettings_ = PerformanceSettingsStore::restore({
        .automaticCpuCache = automaticMemoryBudget.has_value(),
        .cpuCacheMebibytes =
            std::max<std::uint64_t>(decodedByteBudget / bytesPerMebibyte, 1),
        .gpuCacheMebibytes = std::max<std::uint64_t>(
            viewport_->gpuByteBudget() / bytesPerMebibyte, 1),
        .maximumLoadPoints = maximumLoadPoints,
        .raster = performanceSettings_.raster,
    });
    static_cast<void>(applyPerformanceSettings(performanceSettings_));
    viewport_->setViewportSettings(
        ViewportSettingsStore::restore(viewport_->viewportSettings()));
    setWindowTitle(QStringLiteral("Point Cloud Inspector"));
    setAcceptDrops(true);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    profileLoading_ = qEnvironmentVariableIsSet("PCI_PROFILE_LOADING");
    viewport_->setMetricsCallback([this](const RenderMetrics &metrics) {
        showMetrics(withDocumentMetrics(metrics, *session_->document()));
    });
#endif
    viewport_->setFailureCallback([this](const QString &message) {
        showRendererFailure(message);
    });
    viewport_->setLoadProgressCallback(
        [this](const RenderLoadProgress &progress) {
            session_->onRenderLoadProgress(progress);
        });

    auto *centralContainer = new QWidget(this);
    auto *centralStack = new QStackedLayout(centralContainer);
    centralStack->setContentsMargins(0, 0, 0, 0);
    centralStack->setStackingMode(QStackedLayout::StackAll);
    centralStack->addWidget(viewport_->widget());

    emptySceneLabel_ = new QLabel(
        QStringLiteral("<b>Open point clouds, vectors, or rasters</b><br>"
                       "<span style=\"color:#a9b0ba\">"
                       "Select multiple files or drag them into this window."
                       "</span>"),
        centralContainer);
    emptySceneLabel_->setObjectName(QStringLiteral("emptySceneLabel"));
    emptySceneLabel_->setTextFormat(Qt::RichText);
    emptySceneLabel_->setAlignment(Qt::AlignCenter);
    emptySceneLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    emptySceneLabel_->setStyleSheet(QStringLiteral("QLabel#emptySceneLabel {"
                                                   "  color: #f2f4f7;"
                                                   "  font-size: 16px;"
                                                   "  padding: 28px;"
                                                   "}"));
    centralStack->addWidget(emptySceneLabel_);

    navigationHintLabel_ = new QLabel(
        QStringLiteral("Orbit  Left-drag    Pan  Right-drag    Zoom  Wheel    "
                       "Pivot  Double-click    Measure  M    Fit  F"),
        centralContainer);
    navigationHintLabel_->setObjectName(
        QStringLiteral("viewportNavigationHint"));
    navigationHintLabel_->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
    navigationHintLabel_->setContentsMargins(16, 16, 16, 16);
    navigationHintLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    navigationHintLabel_->setStyleSheet(
        QStringLiteral("QLabel#viewportNavigationHint {"
                       "  color: rgba(235, 239, 245, 190);"
                       "  background: transparent;"
                       "  font-size: 11px;"
                       "}"));
    centralStack->addWidget(navigationHintLabel_);

    loadingOverlay_ = new LoadingOverlay(centralContainer);
    centralStack->addWidget(loadingOverlay_);
    loadingOverlay_->hideLoading();
    setCentralWidget(centralContainer);

    sceneLayersDock_ = new SceneLayersDock(this);
    layerInspectorDock_ =
        new LayerInspectorDock(this, session_->document()->colorMaps());
    taskDock_ = new TaskDock(this);
    addDockWidget(Qt::LeftDockWidgetArea, sceneLayersDock_);
    addDockWidget(Qt::RightDockWidgetArea, layerInspectorDock_);
    addDockWidget(Qt::BottomDockWidgetArea, taskDock_);
    taskDock_->hide();
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    diagnosticsDock_ = new DiagnosticsDock(this);
    addDockWidget(Qt::BottomDockWidgetArea, diagnosticsDock_);
    diagnosticsDock_->hide();
#endif
    resizeDocks(
        {sceneLayersDock_, layerInspectorDock_}, {280, 320}, Qt::Horizontal);
    connect(sceneLayersDock_,
            &SceneLayersDock::visibilityToggled,
            session_.get(),
            &SceneSession::setLayerVisible);
    connect(sceneLayersDock_,
            &SceneLayersDock::showVectorAnywayRequested,
            this,
            [this](const SceneLayerId layerId) {
                session_->setLayerVisible(layerId, true);
                statusBar()->showMessage(QStringLiteral(
                    "Showing XY-disjoint vector layer; Fit Scene now includes "
                    "it."));
            });
    connect(layerInspectorDock_,
            &LayerInspectorDock::showVectorAnywayRequested,
            this,
            [this](const SceneLayerId layerId) {
                session_->setLayerVisible(layerId, true);
                statusBar()->showMessage(QStringLiteral(
                    "Showing XY-disjoint vector layer; Fit Scene now includes "
                    "it."));
            });
    connect(layerInspectorDock_,
            &LayerInspectorDock::vectorStyleChanged,
            session_.get(),
            &SceneSession::setVectorLayerStyle);
    connect(layerInspectorDock_,
            &LayerInspectorDock::showRasterAnywayRequested,
            this,
            [this](const SceneLayerId layerId) {
                session_->setLayerVisible(layerId, true);
                statusBar()->showMessage(QStringLiteral(
                    "Showing XY-disjoint raster layer; Fit Scene now includes "
                    "it."));
            });
    connect(layerInspectorDock_,
            &LayerInspectorDock::rasterStyleChanged,
            session_.get(),
            &SceneSession::setRasterLayerStyle);
    connect(layerInspectorDock_,
            &LayerInspectorDock::retryRasterElevationRequested,
            session_.get(),
            &SceneSession::retryRasterElevation);
    connect(layerInspectorDock_,
            &LayerInspectorDock::cancelRasterElevationRequested,
            session_.get(),
            &SceneSession::cancelRasterElevation);
    connect(taskDock_,
            &TaskDock::jobActionRequested,
            this,
            [this](const LoadJobKey key, const LoadJobAction action) {
                switch (action) {
                case LoadJobAction::Cancel:
                    session_->cancelJob(key);
                    break;
                case LoadJobAction::Retry:
                    session_->retryJob(key);
                    break;
                case LoadJobAction::Prioritize:
                    session_->prioritizeJob(key);
                    break;
                case LoadJobAction::Dismiss:
                    session_->dismissJob(key);
                    break;
                }
            });
    connect(layerInspectorDock_,
            &LayerInspectorDock::pointColorModeChanged,
            session_.get(),
            &SceneSession::setLayerColorMode);
    connect(layerInspectorDock_,
            &LayerInspectorDock::revertRasterColorsRequested,
            this,
            [this](const SceneLayerId id) {
                static_cast<void>(session_->revertPointCloudColors(id));
            });
    connect(layerInspectorDock_,
            &LayerInspectorDock::colorizeFromRasterRequested,
            this,
            &MainWindow::showColorizeFromRaster);
    connect(layerInspectorDock_,
            &LayerInspectorDock::allPointColorModesChanged,
            session_.get(),
            &SceneSession::setAllLayerColors);
    connect(layerInspectorDock_,
            &LayerInspectorDock::classificationFilterChanged,
            session_.get(),
            &SceneSession::setLayerClassificationFilter);
    connect(sceneLayersDock_,
            &SceneLayersDock::removeRequested,
            session_.get(),
            &SceneSession::removeLayer);
    connect(sceneLayersDock_,
            &SceneLayersDock::fitRequested,
            this,
            [this](const SceneLayerId layerId) {
                viewport_->frameLayer(layerId);
            });
    connect(sceneLayersDock_,
            &SceneLayersDock::isolateRequested,
            session_.get(),
            &SceneSession::isolateLayer);
    connect(sceneLayersDock_,
            &SceneLayersDock::statisticsRequested,
            this,
            &MainWindow::showLayerStatistics);
    connect(sceneLayersDock_,
            &SceneLayersDock::colorizeRequested,
            this,
            &MainWindow::showColorizeFromRaster);
    connect(sceneLayersDock_,
            &SceneLayersDock::revertColorsRequested,
            this,
            [this](const SceneLayerId id) {
                static_cast<void>(session_->revertPointCloudColors(id));
            });
    connect(sceneLayersDock_,
            &SceneLayersDock::showAllRequested,
            session_.get(),
            &SceneSession::showAllLayers);
    connect(sceneLayersDock_,
            &SceneLayersDock::addLayerRequested,
            this,
            &MainWindow::chooseSources);
    connect(sceneLayersDock_,
            &SceneLayersDock::selectionChanged,
            this,
            [this](const SceneLayerId layerId) {
                layerInspectorDock_->setDocumentSnapshot(
                    session_->document()->snapshot(), layerId);
                updateUiContext();
            });

    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->setObjectName(QStringLiteral("fileMenu"));
    openAction_ = fileMenu->addAction(QStringLiteral("&Open Files…"));
    openAction_->setObjectName(QStringLiteral("openFilesAction"));
    openAction_->setIconText(QStringLiteral("Open…"));
    openAction_->setIcon(toolbarIcon(ToolbarIcon::Open, palette()));
    openAction_->setShortcut(QKeySequence::Open);
    openAction_->setToolTip(
        QStringLiteral("Open point-cloud, vector, or raster files (%1)")
            .arg(openAction_->shortcut().toString(QKeySequence::NativeText)));
    connect(openAction_, &QAction::triggered, this, &MainWindow::chooseSources);

    const auto updateVectorImportCapability =
        [this](const VectorOverlayCapability capability,
               const QString &reason) {
            session_->setVectorImportAvailability(
                capability == VectorOverlayCapability::Supported
                    ? VectorImportAvailability::Supported
                : capability == VectorOverlayCapability::Unsupported
                    ? VectorImportAvailability::Unsupported
                    : VectorImportAvailability::Unknown,
                reason);
        };
    viewport_->setVectorOverlayCapabilityCallback(updateVectorImportCapability);
    // A viewport may publish its capability before the window subscribes, or
    // deliberately defer publication while its backend is unavailable. Apply
    // the current value as well so Unknown never leaves import enabled.
    updateVectorImportCapability(viewport_->vectorOverlayCapability(), {});

    viewport_->setRasterSurfaceCapabilityCallback(
        [this](const RasterSurfaceCapability capability,
               const QString &reason) {
            layerInspectorDock_->setRasterSurfaceCapability(capability,
                                                            reason);
        });

    fileMenu->addSeparator();
    QAction *cancelAction =
        fileMenu->addAction(QStringLiteral("Cancel Loading"));
    cancelAction->setObjectName(QStringLiteral("cancelLoadingAction"));
    connect(cancelAction, &QAction::triggered, this, [this] {
        requestLoadCancellation();
    });

    QMenu *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
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
    QAction *settingsAction = editMenu->addAction(QStringLiteral("&Settings…"));
    settingsAction->setObjectName(QStringLiteral("settingsAction"));
    settingsAction->setMenuRole(QAction::PreferencesRole);
    settingsAction->setIconText(QStringLiteral("Settings…"));
    settingsAction->setIcon(toolbarIcon(ToolbarIcon::Settings, palette()));
    settingsAction->setToolTip(QStringLiteral("Open application settings"));
    connect(
        settingsAction, &QAction::triggered, this, &MainWindow::showSettings);

    QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    viewMenu->setObjectName(QStringLiteral("viewMenu"));

    fitSceneAction_ = viewMenu->addAction(QStringLiteral("&Fit Scene"));
    fitSceneAction_->setObjectName(QStringLiteral("fitSceneAction"));
    fitSceneAction_->setIconText(QStringLiteral("Fit"));
    fitSceneAction_->setIcon(toolbarIcon(ToolbarIcon::Fit, palette()));
    fitSceneAction_->setShortcut(QKeySequence(QStringLiteral("F")));
    fitSceneAction_->setShortcutContext(Qt::ApplicationShortcut);
    fitSceneAction_->setToolTip(
        QStringLiteral("Fit all visible layers in the viewport (F)"));
    connect(fitSceneAction_, &QAction::triggered, this, [this] {
        viewport_->frameVisibleLayers();
    });

    topDownSceneAction_ = viewMenu->addAction(QStringLiteral("Top Down Scene"));
    topDownSceneAction_->setObjectName(QStringLiteral("topDownSceneAction"));
    topDownSceneAction_->setIconText(QStringLiteral("Top Down"));
    topDownSceneAction_->setIcon(toolbarIcon(ToolbarIcon::TopDown, palette()));
    topDownSceneAction_->setShortcut(QKeySequence(QStringLiteral("7")));
    topDownSceneAction_->setShortcutContext(Qt::ApplicationShortcut);
    topDownSceneAction_->setToolTip(
        QStringLiteral("Fit all visible layers in a top-down view (7)"));
    connect(topDownSceneAction_, &QAction::triggered, this, [this] {
        viewport_->frameVisibleLayersTopDown();
    });

    orthographicAction_ =
        viewMenu->addAction(QStringLiteral("Orthographic Camera"));
    orthographicAction_->setObjectName(
        QStringLiteral("orthographicCameraAction"));
    orthographicAction_->setIconText(QStringLiteral("Orthographic"));
    orthographicAction_->setIcon(
        toolbarIcon(ToolbarIcon::Orthographic, palette()));
    orthographicAction_->setCheckable(true);
    orthographicAction_->setChecked(viewport_->isOrthographic());
    orthographicAction_->setToolTip(QStringLiteral(
        "Toggle between perspective and orthographic camera projection"));
    connect(orthographicAction_,
            &QAction::toggled,
            this,
            [this](const bool enabled) {
                viewport_->setOrthographic(enabled);
            });

    QMenu *panelsMenu = viewMenu->addMenu(QStringLiteral("&Panels"));
    panelsMenu->setObjectName(QStringLiteral("panelsMenu"));
    QAction *toggleLayerPanelAction = sceneLayersDock_->toggleViewAction();
    toggleLayerPanelAction->setObjectName(
        QStringLiteral("pointCloudLayerPanelToggleAction"));
    toggleLayerPanelAction->setText(QStringLiteral("&Scene"));
    toggleLayerPanelAction->setIcon(
        style()->standardIcon(QStyle::SP_FileDialogDetailedView));
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
    eyeDomeLightingAction_->setChecked(viewport_->eyeDomeLightingEnabled());
    eyeDomeLightingAction_->setToolTip(
        QStringLiteral("Enhance point-cloud depth perception"));
    connect(eyeDomeLightingAction_,
            &QAction::toggled,
            this,
            [this](const bool enabled) {
                viewport_->setEyeDomeLightingEnabled(enabled);
            });

    QMenu *layerMenu = menuBar()->addMenu(QStringLiteral("&Layer"));
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

    QMenu *toolsMenu = menuBar()->addMenu(QStringLiteral("&Tools"));
    toolsMenu->setObjectName(QStringLiteral("toolsMenu"));
    navigateToolAction_ = toolsMenu->addAction(QStringLiteral("&Navigate"));
    navigateToolAction_->setObjectName(QStringLiteral("navigateToolAction"));
    navigateToolAction_->setIcon(toolbarIcon(ToolbarIcon::Navigate, palette()));
    navigateToolAction_->setCheckable(true);
    navigateToolAction_->setChecked(true);
    navigateToolAction_->setShortcut(QKeySequence(QStringLiteral("N")));
    navigateToolAction_->setShortcutContext(Qt::ApplicationShortcut);
    navigateToolAction_->setToolTip(QStringLiteral(
        "Navigate the viewport with orbit, pan, zoom, and fly controls"));
    measureToolAction_ = toolsMenu->addAction(QStringLiteral("&Measure"));
    measureToolAction_->setObjectName(QStringLiteral("measureToolAction"));
    measureToolAction_->setIcon(toolbarIcon(ToolbarIcon::Measure, palette()));
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
                    viewport_->setActiveTool(ViewportTool::Navigate);
                }
            });
    connect(measureToolAction_,
            &QAction::toggled,
            this,
            [this](const bool checked) {
                if (checked) {
                    viewport_->setActiveTool(ViewportTool::Measure);
                    statusBar()->showMessage(
                        QStringLiteral("Measure: hover to snap, click two "
                                       "points; drag to orbit; Esc clears."));
                }
            });

    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    helpMenu->setObjectName(QStringLiteral("helpMenu"));
    QAction *controlsAction =
        helpMenu->addAction(QStringLiteral("&Viewport Controls"));
    controlsAction->setObjectName(QStringLiteral("viewportControlsAction"));
    controlsAction->setShortcut(QKeySequence(QStringLiteral("?")));
    connect(controlsAction,
            &QAction::triggered,
            this,
            &MainWindow::showControlsReference);

    QToolBar *pointCloudToolBar = addToolBar(QStringLiteral("Commands"));
    pointCloudToolBar->setObjectName(QStringLiteral("pointCloudToolBar"));
    pointCloudToolBar->setMovable(false);
    pointCloudToolBar->setIconSize(QSize(20, 20));
    pointCloudToolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    pointCloudToolBar->addAction(openAction_);
    pointCloudToolBar->addSeparator();
    pointCloudToolBar->addAction(fitSceneAction_);
    pointCloudToolBar->addAction(topDownSceneAction_);
    pointCloudToolBar->addAction(orthographicAction_);
    pointCloudToolBar->addSeparator();
    auto *pointSizeSpinBox = new QSpinBox(pointCloudToolBar);
    pointSizeSpinBox->setObjectName(QStringLiteral("pointSizeSpinBox"));
    pointSizeSpinBox->setRange(minimumPointSizePixels, maximumPointSizePixels);
    pointSizeSpinBox->setValue(viewport_->pointSizePixels());
    pointSizeSpinBox->setPrefix(QStringLiteral("Point size  "));
    pointSizeSpinBox->setSuffix(QStringLiteral(" px"));
    pointSizeSpinBox->setToolTip(QStringLiteral("Set the rendered point size"));
    pointSizeSpinBox->setAccessibleName(QStringLiteral("Point size"));
    pointCloudToolBar->addWidget(pointSizeSpinBox);
    connect(pointSizeSpinBox,
            &QSpinBox::valueChanged,
            this,
            [this](const int pointSize) {
                viewport_->setPointSizePixels(pointSize);
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
    pointCloudToolBar->addAction(settingsAction);

    auto *toolRail = new QToolBar(QStringLiteral("Viewport tools"), this);
    toolRail->setObjectName(QStringLiteral("viewportToolRail"));
    toolRail->setOrientation(Qt::Vertical);
    toolRail->setMovable(false);
    toolRail->setIconSize(QSize(20, 20));
    toolRail->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    toolRail->addAction(navigateToolAction_);
    toolRail->addAction(measureToolAction_);
    addToolBar(Qt::LeftToolBarArea, toolRail);

    WorkspaceSettings::restore(*this);
    refreshLayerPanel();
    updateUiContext();

    connect(loadingOverlay_, &LoadingOverlay::cancelRequested, this, [this] {
        requestLoadCancellation();
    });

    connect(session_.get(),
            &SceneSession::documentChanged,
            this,
            [this](SceneDocumentSnapshotPtr snapshot,
                   const bool frameVisibleLayers,
                   const bool replaceRendererDocument) {
                if (replaceRendererDocument) {
                    viewport_->setDocument(std::move(snapshot),
                                           frameVisibleLayers);
                } else {
                    viewport_->updateDocument(std::move(snapshot));
                    if (frameVisibleLayers) {
                        viewport_->frameVisibleLayers();
                    }
                }
                refreshLayerPanel();
            });
    connect(session_.get(),
            &SceneSession::frameVisibleLayersRequested,
            this,
            [this] {
                viewport_->frameVisibleLayers();
            });
    connect(session_.get(),
            &SceneSession::loadingChanged,
            this,
            [this](const bool loading,
                   const bool showOverlay,
                   const QString &title) {
                openAction_->setEnabled(!loading);
                updateUiContext();
                if (!loading) {
                    loadingOverlay_->showComplete(QStringLiteral("Ready"));
                } else if (showOverlay) {
                    loadingOverlay_->showLoading(title);
                } else {
                    loadingOverlay_->hideLoading();
                }
            });
    connect(session_.get(),
            &SceneSession::loadingProgressChanged,
            this,
            &MainWindow::setLoadingProgress);
    connect(session_.get(),
            &SceneSession::batchProgressChanged,
            this,
            [this](const int percentage, const QString &details) {
                loadingOverlay_->setProgress(percentage, details);
            });
    connect(session_.get(),
            &SceneSession::statusChanged,
            this,
            [this](const QString &status) {
                statusBar()->showMessage(status);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
                if (!session_->loading() &&
                    qualificationReporter_.configured()) {
                    QTimer::singleShot(0, this, [this, status] {
                        writeQualificationReport(status);
                    });
                }
#endif
            });
    connect(session_.get(),
            &SceneSession::taskRowsChanged,
            this,
            [this](LoadJobRows rows) {
                taskDock_->setRows(std::move(rows));
                updateUiContext();
            });
    connect(session_.get(),
            &SceneSession::showTasksRequested,
            taskDock_,
            &QDockWidget::show);
    connect(
        session_.get(),
        &SceneSession::failureOccurred,
        this,
        [this](const QString &title, const QString &message) {
            loadingOverlay_->hideLoading();
            auto *dialog = new QMessageBox(
                QMessageBox::Critical, title, message, QMessageBox::Ok, this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->show();
        });

    connect(
        session_.get(),
        &SceneSession::vectorSelectionRequired,
        this,
        [this](const LoadJobId jobId, const VectorImportPreflight &preflight) {
            enqueueVectorSelection(jobId, preflight);
        });
    connect(
        session_.get(),
        &SceneSession::vectorJobFinished,
        this,
        [this](const LoadJobId jobId) {
            std::erase_if(pendingVectorSelections_, [jobId](const auto &item) {
                return item.jobId == jobId;
            });
            if (activeVectorSelectionJob_ != jobId) {
                return;
            }
            activeVectorSelectionJob_.reset();
            for (VectorSublayerDialog *dialog :
                 findChildren<VectorSublayerDialog *>()) {
                if (dialog->property("vectorLoadJobId").value<LoadJobId>() ==
                    jobId) {
                    dialog->reject();
                }
            }
            QTimer::singleShot(0, this, &MainWindow::showNextVectorSelection);
        });
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    statusBar()->showMessage(QStringLiteral("Initializing %1 renderer…")
                                 .arg(viewport_->backendName()));
#else
    statusBar()->showMessage(QStringLiteral("Ready"));
#endif
}

MainWindow::~MainWindow() = default;

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!session_->loading()) {
        const std::vector<std::filesystem::path> paths =
            localDroppedFiles(*event->mimeData());
        if (std::ranges::any_of(paths, [](const auto &path) {
                return supportedSourceKind(path).has_value();
            })) {
            event->acceptProposedAction();
            return;
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    std::vector<std::filesystem::path> paths =
        localDroppedFiles(*event->mimeData());
    if (session_->loading() ||
        std::ranges::none_of(paths, [](const auto &path) {
            return supportedSourceKind(path).has_value();
        })) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    openSources(std::move(paths));
}

LoadJobId MainWindow::loadVectorLayers(VectorImportRequest request)
{
    if (viewport_->vectorOverlayCapability() !=
        VectorOverlayCapability::Supported) {
        throw std::runtime_error(
            "vector overlays are not supported by the active renderer");
    }
    return session_->loadVectorLayers(std::move(request));
}

void MainWindow::setGdalCacheControls(GdalCacheControls controls)
{
    gdalCache_ = std::move(controls);
    static_cast<void>(applyPerformanceSettings(performanceSettings_));
}

void MainWindow::setGdalRuntimeInfo(GdalRuntimeInfo info)
{
    gdalRuntimeInfo_ = std::move(info);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    qualificationReporter_.setGdalRuntimeInfo(gdalRuntimeInfo_);
#endif
}

LoadJobId MainWindow::importRasterLayer(RasterImportRequest request)
{
    // Rasters draw through the same renderer path as points, so unlike vector
    // overlays there is no separate capability to probe here.
    return session_->startRasterImport(std::move(request));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (SettingsDialog *dialog = findChild<SettingsDialog *>();
        dialog && dialog->isVisible()) {
        dialog->reject();
    }
    WorkspaceSettings::save(*this);
    ViewportSettingsStore::save(viewport_->viewportSettings());
    PerformanceSettingsStore::save(performanceSettings_);
    QMainWindow::closeEvent(event);
}

void MainWindow::showSettings()
{
    if (SettingsDialog *dialog = findChild<SettingsDialog *>()) {
        dialog->raise();
        dialog->activateWindow();
        return;
    }

    auto *dialog = new SettingsDialog(
        viewport_->viewportSettings(), performanceSettings_, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    const ViewportSettings originalViewportSettings =
        viewport_->viewportSettings();
    const PerformanceSettings originalPerformanceSettings =
        performanceSettings_;
    connect(dialog,
            &SettingsDialog::settingsChanged,
            this,
            [this](const ViewportSettings viewportSettings,
                   const PerformanceSettings performanceSettings) {
                applySettingsPreview(viewportSettings, performanceSettings);
            });
    connect(dialog, &QDialog::accepted, this, [this, dialog] {
        applySettingsPreview(dialog->settings(), dialog->performanceSettings());
        ViewportSettingsStore::save(viewport_->viewportSettings());
        PerformanceSettingsStore::save(performanceSettings_);
    });
    connect(dialog,
            &QDialog::rejected,
            this,
            [this, originalViewportSettings, originalPerformanceSettings] {
                applySettingsPreview(originalViewportSettings,
                                     originalPerformanceSettings);
            });
    dialog->show();
}

void MainWindow::applySettingsPreview(
    const ViewportSettings &viewportSettings,
    const PerformanceSettings &performanceSettings)
{
    viewport_->setViewportSettings(viewportSettings);
    eyeDomeLightingAction_->setChecked(
        viewport_->viewportSettings().depthEnhancement.enabled);
    if (!applyPerformanceSettings(performanceSettings)) {
        statusBar()->showMessage(
            QStringLiteral("The CPU cache limit will fully apply after "
                           "restarting or closing loaded layers."),
            8000);
    }
}

bool MainWindow::applyPerformanceSettings(const PerformanceSettings &settings)
{
    performanceSettings_ = settings;
    performanceSettings_.cpuCacheMebibytes = std::clamp<std::uint64_t>(
        performanceSettings_.cpuCacheMebibytes, 1, maximumCacheMebibytes);
    performanceSettings_.gpuCacheMebibytes = std::clamp<std::uint64_t>(
        performanceSettings_.gpuCacheMebibytes, 1, maximumCacheMebibytes);
    performanceSettings_.maximumLoadPoints = std::clamp<std::uint64_t>(
        performanceSettings_.maximumLoadPoints,
        1,
        static_cast<std::uint64_t>(maximumLoadPointsSetting));
    performanceSettings_.raster =
        clampRasterPerformanceSettings(performanceSettings_.raster);

    const std::uint64_t gpuBytes =
        performanceSettings_.gpuCacheMebibytes * bytesPerMebibyte;
    viewport_->setGpuByteBudget(gpuBytes);
    session_->setMaximumLoadPoints(performanceSettings_.maximumLoadPoints);

    const std::uint64_t rasterCpuBytes =
        mebibytesToBytes(performanceSettings_.raster.cpuCacheMebibytes);
    const std::uint64_t rasterGpuBytes =
        mebibytesToBytes(performanceSettings_.raster.gpuCacheMebibytes);
    const std::uint64_t gdalCacheBytes =
        mebibytesToBytes(performanceSettings_.raster.gdalCacheMebibytes);
    viewport_->setRasterByteBudgets(rasterCpuBytes, rasterGpuBytes);
    // Process-global and outside the application's own accounting, so it is
    // set explicitly rather than left at GDAL's percentage-of-RAM default.
    if (gdalCache_.setByteBudget) {
        gdalCache_.setByteBudget(gdalCacheBytes);
    }

    std::optional<AutomaticMemoryBudgetParameters> automaticParameters;
    if (performanceSettings_.automaticCpuCache) {
        automaticParameters.emplace();
        // Both budgets share one pool on unified-memory devices.
        automaticParameters->gpuByteBudget =
            saturatingAdd(gpuBytes, rasterGpuBytes);
        automaticParameters->rasterCpuByteBudget = rasterCpuBytes;
        automaticParameters->gdalCacheByteBudget = gdalCacheBytes;
    }
    return session_->setDecodedByteBudget(
        performanceSettings_.cpuCacheMebibytes * bytesPerMebibyte,
        automaticParameters);
}

void MainWindow::showControlsReference()
{
    auto *dialog = new QMessageBox(
        QMessageBox::Information,
        QStringLiteral("Viewport controls"),
        QStringLiteral("<b>Mouse</b><br>"
                       "Orbit — left-drag<br>"
                       "Pan — right-drag<br>"
                       "Zoom toward cursor — wheel or trackpad scroll<br>"
                       "Set orbit pivot — double-click<br><br>"
                       "<b>Measure</b><br>"
                       "Measure — M, hover to snap, click two points<br>"
                       "Clear measurement — Escape<br><br>"
                       "<b>Keyboard</b><br>"
                       "Move — W / A / S / D<br>"
                       "Move down / up — Q / E<br>"
                       "Move faster — hold Shift<br>"
                       "Fine movement — hold Alt<br>"
                       "Fit visible scene — F"),
        QMessageBox::Ok,
        this);
    dialog->setObjectName(QStringLiteral("viewportControlsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::chooseSources()
{
    const QStringList selected = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("Open Files"),
        {},
        QStringLiteral(
            "Supported files (*.las *.laz *ept.json *.gpkg *.geojson *.json "
            "*.shp *.kml *.gml *.fgb *.dxf *.tif *.tiff *.cog *.vrt *.gti "
            "*.img *.jp2 *.png *.jpg *.jpeg);;"
            "Point clouds (*.las *.laz *.copc.laz *ept.json);;"
            "Vector files (*.gpkg *.geojson *.json *.shp *.kml *.gml *.fgb "
            "*.dxf);;"
            "Raster files (*.tif *.tiff *.cog *.vrt *.gti *.img *.jp2 *.png "
            "*.jpg *.jpeg);;All files (*)"));
    if (selected.isEmpty()) {
        return;
    }
    std::vector<std::filesystem::path> paths;
    paths.reserve(selected.size());
    for (const QString &path : selected) {
        paths.push_back(qStringToPath(path));
    }
    openSources(std::move(paths));
}

void MainWindow::updateUiContext()
{
    const bool hasLayers = session_->document()->hasAnyLayer();
    const bool hasSelection =
        sceneLayersDock_ && sceneLayersDock_->currentLayerId().has_value();
    if (sceneLayersDock_) {
        const SceneLayerId selected =
            sceneLayersDock_->currentLayerId().value_or(SceneLayerId{});
        const bool active = selected != SceneLayerId{} &&
                            session_->hasActiveColorizeJob(selected);
        const bool committing = selected != SceneLayerId{} &&
                                session_->colorizeCommitInProgress(selected);
        sceneLayersDock_->setColorizeJobState(active, committing);
        if (layerInspectorDock_) {
            layerInspectorDock_->setColorizeJobState(active, committing);
        }
    }

    if (fitSceneAction_) {
        fitSceneAction_->setEnabled(hasLayers);
    }
    if (topDownSceneAction_) {
        topDownSceneAction_->setEnabled(hasLayers);
    }
    if (fitSelectedLayerAction_) {
        fitSelectedLayerAction_->setEnabled(hasSelection);
    }
    if (isolateSelectedLayerAction_) {
        isolateSelectedLayerAction_->setEnabled(hasSelection);
    }
    if (showAllLayersAction_) {
        showAllLayersAction_->setEnabled(hasLayers);
    }
    if (removeSelectedLayerAction_) {
        removeSelectedLayerAction_->setEnabled(hasSelection);
    }
    if (measureToolAction_) {
        measureToolAction_->setEnabled(hasLayers);
    }
    if (!hasLayers && navigateToolAction_ && measureToolAction_ &&
        measureToolAction_->isChecked()) {
        navigateToolAction_->setChecked(true);
    }
    if (emptySceneLabel_) {
        emptySceneLabel_->setVisible(!hasLayers && !session_->loading());
    }
    if (navigationHintLabel_) {
        navigationHintLabel_->setVisible(hasLayers && !session_->loading() &&
                                         !navigationHintDismissed_);
        if (navigationHintLabel_->isVisible()) {
            QTimer::singleShot(12000, this, [this] {
                navigationHintDismissed_ = true;
                if (navigationHintLabel_) {
                    navigationHintLabel_->hide();
                }
            });
        }
    }
}

#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
void MainWindow::configureQualificationReport(std::filesystem::path outputPath,
                                              const bool exitAfterWrite)
{
    qualificationReporter_.configure(std::move(outputPath), exitAfterWrite);
    viewport_->setContinuousMetricsEnabled(true);
}
#endif

void MainWindow::loadPointCloud(const std::filesystem::path &sourcePath)
{
    loadPointCloud(sourcePath, PointCloudLoadMode::Replace);
}

void MainWindow::loadPointCloud(const std::filesystem::path &sourcePath,
                                const PointCloudLoadMode mode)
{
    session_->loadPointCloud(sourcePath, mode);
}

void MainWindow::loadPointClouds(std::vector<std::filesystem::path> sourcePaths,
                                 const PointCloudLoadMode firstMode)
{
    session_->loadPointClouds(std::move(sourcePaths), firstMode);
}

void MainWindow::openSources(std::vector<std::filesystem::path> sourcePaths)
{
    if (sourcePaths.empty()) {
        return;
    }
    if (session_->loading()) {
        statusBar()->showMessage(
            QStringLiteral("Finish or cancel the active point-cloud load "
                           "before opening more files."),
            8000);
        return;
    }

    SourceClassification classification =
        classifySupportedSources(std::move(sourcePaths));
    if (!classification.unsupported.empty()) {
        QStringList names;
        names.reserve(
            static_cast<qsizetype>(classification.unsupported.size()));
        for (const std::filesystem::path &path : classification.unsupported) {
            names.push_back(displayPathName(path));
        }
        auto *dialog = new QMessageBox(
            QMessageBox::Warning,
            QStringLiteral("Unsupported files"),
            QStringLiteral("These files were skipped because their type is "
                           "not supported:\n%1")
                .arg(names.join(QLatin1Char('\n'))),
            QMessageBox::Ok,
            this);
        dialog->setObjectName(QStringLiteral("unsupportedFilesDialog"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    }
    if (!classification.supported.empty()) {
        session_->openSources(std::move(classification.supported));
    }
}

void MainWindow::enqueueVectorSelection(const LoadJobId jobId,
                                        VectorImportPreflight preflight)
{
    pendingVectorSelections_.push_back(
        {.jobId = jobId, .preflight = std::move(preflight)});
    showNextVectorSelection();
}

void MainWindow::showNextVectorSelection()
{
    if (activeVectorSelectionJob_ || pendingVectorSelections_.empty()) {
        return;
    }
    PendingVectorSelection pending =
        std::move(pendingVectorSelections_.front());
    pendingVectorSelections_.pop_front();
    activeVectorSelectionJob_ = pending.jobId;

    auto *dialog = new VectorSublayerDialog(pending.preflight, this);
    dialog->setProperty("vectorLoadJobId", QVariant::fromValue(pending.jobId));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog,
            &QDialog::finished,
            this,
            [this, dialog, jobId = pending.jobId](const int result) {
                if (activeVectorSelectionJob_ != jobId) {
                    showNextVectorSelection();
                    return;
                }
                activeVectorSelectionJob_.reset();
                if (result == QDialog::Accepted) {
                    if (!session_->continueVectorImport(
                            jobId, dialog->selectedSublayers())) {
                        session_->cancelJob({LoadJobKind::Vector, jobId});
                    }
                } else {
                    session_->cancelJob({LoadJobKind::Vector, jobId});
                }
                QTimer::singleShot(
                    0, this, &MainWindow::showNextVectorSelection);
            });
    dialog->open();
}

void MainWindow::requestLoadCancellation()
{
    const bool cancellingPoints = session_->loading();
    const bool cancellingVectors = session_->hasActiveVectorLoads();
    const bool cancellingRasters = session_->hasActiveRasterLoads();
    const bool cancellingColorize = session_->hasActiveColorizeJobs();
    if (!cancellingPoints && !cancellingVectors && !cancellingRasters &&
        !cancellingColorize) {
        return;
    }
    if (cancellingPoints && loadingOverlay_) {
        loadingOverlay_->setCancelling();
    }
    session_->cancelAllLoads();
}

void MainWindow::refreshLayerPanel()
{
    if (sceneLayersDock_) {
        const SceneDocumentSnapshotPtr snapshot =
            session_->document()->snapshot();
        sceneLayersDock_->setDocumentSnapshot(snapshot);
    }
    updateUiContext();
}

void MainWindow::showLayerStatistics(const PointCloudLayerId layerId)
{
    const std::optional<PointCloudLayer> layer =
        session_->document()->layer(layerId);
    if (!layer) {
        return;
    }
    auto *dialog = new PointCloudStatisticsDialog(
        layer->scene->metadata(), session_->statisticsProvider(), this);
    dialog->show();
    dialog->startAnalysis();
}

void MainWindow::showColorizeFromRaster(const PointCloudLayerId layerId)
{
    const auto layer = session_->document()->layer(layerId);
    if (!layer) {
        return;
    }
    ColorizeFromRasterDialog dialog(
        *layer,
        session_->document()->snapshot(),
        session_->spatialReferenceComparator(),
        session_->document()->memoryBudget()->availableBytes(),
        this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const auto rasterId = dialog.selectedRasterLayerId();
    if (!rasterId) {
        return;
    }
    try {
        static_cast<void>(
            session_->colorizePointCloudFromRaster(layerId, *rasterId));
    } catch (const std::exception &error) {
        QMessageBox::warning(this,
                             QStringLiteral("Colorize from Raster"),
                             QString::fromUtf8(error.what()));
    }
}

void MainWindow::setLoadingProgress(const LoadingProgressState &state)
{
    loadingOverlay_->setProgress(state.percentage,
                                 loadingProgressDetails(state));
}

#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
void MainWindow::showMetrics(const RenderMetrics &metrics)
{
    qualificationReporter_.record(metrics);
    const PointCloudLoadControllerMetrics loadMetrics =
        session_->pointLoadMetrics();
    if (diagnosticsDock_) {
        std::optional<std::uint64_t> gdalCacheUsedBytes;
        if (gdalCache_.usedBytes) {
            gdalCacheUsedBytes = gdalCache_.usedBytes();
        }
        diagnosticsDock_->setDiagnostics(RenderDiagnosticsFormatter::panelText(
            metrics,
            loadMetrics,
            gdalCacheUsedBytes,
            mebibytesToBytes(performanceSettings_.raster.gdalCacheMebibytes),
            session_->colorizeMetrics()));
    }
    if (!session_->loading()) {
        statusBar()->showMessage(RenderDiagnosticsFormatter::statusText(
            metrics, session_->timings().timeToFirstPointsMilliseconds));
    }
}

void MainWindow::writeQualificationReport(const QString &status)
{
    const QualificationWriteResult result = qualificationReporter_.write(
        status,
        *session_,
        QualificationGdalCacheSnapshot{
            .budgetBytes = mebibytesToBytes(
                performanceSettings_.raster.gdalCacheMebibytes),
            .usedBytes =
                gdalCache_.usedBytes
                    ? std::optional<std::uint64_t>{gdalCache_.usedBytes()}
                    : std::nullopt,
        });
    if (!result.error.isEmpty()) {
        statusBar()->showMessage(result.error);
        return;
    }
    if (!result.written) {
        return;
    }
    qInfo().noquote() << QStringLiteral("Release H qualification report: %1")
                             .arg(result.filename);
    if (result.exitRequested) {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    }
}
#endif

void MainWindow::showRendererFailure(const QString &message)
{
    if (session_->loading()) {
        loadingOverlay_->hideLoading();
        session_->cancelAll();
    }
    statusBar()->showMessage(
        QStringLiteral("Renderer failed: %1").arg(message));
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    writeQualificationReport(
        QStringLiteral("Renderer failed: %1").arg(message));
#endif
}

} // namespace pci
