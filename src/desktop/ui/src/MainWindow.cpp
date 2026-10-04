#include "WindowActions.h"
#include <pci/desktop/ui/MainWindow.h>

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
                                  const SceneRuntimeMetrics &hierarchy)
{
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
        showMetrics(withDocumentMetrics(metrics, session_->documentMetrics()));
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
        viewport_->isMapView()
            ? QStringLiteral("Pan  Left-drag    Move  W / A / S / D    "
                             "Zoom  Wheel    Fit  F")
            : QStringLiteral(
                  "Orbit  Left-drag    Pan  Right-drag    Zoom  Wheel    "
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
    layerInspectorDock_ = new LayerInspectorDock(this, session_->colorMaps());
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
                    session_->documentSnapshot(), layerId);
                updateUiContext();
            });

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
            layerInspectorDock_->setRasterSurfaceCapability(capability, reason);
        });

    actions_ = std::make_unique<WindowActions>(
        *this,
        *session_,
        *viewport_,
        *sceneLayersDock_,
        *layerInspectorDock_,
        *taskDock_,
        *navigationHintLabel_,
        WindowActionCallbacks{.chooseSources =
                                  [this] {
                                      chooseSources();
                                  },
                              .showSettings =
                                  [this] {
                                      showSettings();
                                  },
                              .showControlsReference =
                                  [this] {
                                      showControlsReference();
                                  },
                              .cancelLoading =
                                  [this] {
                                      requestLoadCancellation();
                                  }}
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
        ,
        diagnosticsDock_
#endif
    );

    WorkspaceSettings::restore(*this);
    refreshLayerPanel();
    updateUiContext();

    connect(loadingOverlay_, &LoadingOverlay::cancelRequested, this, [this] {
        requestLoadCancellation();
    });

    connect(session_.get(),
            &SceneSession::documentChanged,
            this,
            [this](DocumentUpdate update) {
                const bool frameVisibleLayers =
                    update.viewAdjustment == ViewAdjustment::FrameVisibleLayers;
                if (update.rendererPolicy ==
                    RendererDocumentPolicy::ResetPointView) {
                    viewport_->setDocument(std::move(update.snapshot),
                                           std::move(update.runtime),
                                           update.runtimeBudget,
                                           update.sessionGeneration,
                                           frameVisibleLayers);
                } else {
                    viewport_->updateDocument(std::move(update.snapshot),
                                              std::move(update.runtime),
                                              update.runtimeBudget,
                                              update.sessionGeneration);
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
                actions_->openAction_->setEnabled(!loading);
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
                        if (qualificationReplayStarted_ ||
                            qualificationReplayFinished_) {
                            return;
                        }
                        viewport_->setContinuousMetricsEnabled(true);
                        if (viewport_->startQualificationCameraPath()) {
                            qualificationReporter_.beginReplay();
                            qualificationReplayStarted_ = true;
                            return;
                        }
                        viewport_->setContinuousMetricsEnabled(false);
                        qualificationReplayFinished_ = true;
                        writeQualificationReport(status);
                    });
                }
#endif
            });
    connect(session_.get(),
            &SceneSession::taskRowChanged,
            this,
            [this](LoadJobRow row) {
                taskDock_->updateRow(std::move(row));
                updateUiContext();
            });
    connect(session_.get(),
            &SceneSession::taskRowRemoved,
            this,
            [this](LoadJobKey key) {
                taskDock_->removeRow(key);
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

    auto *dialog = new SettingsDialog(viewport_->viewportSettings(),
                                      performanceSettings_,
                                      this,
                                      session_->storageMaintenance());
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
    actions_->eyeDomeLightingAction_->setChecked(
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
                       "Fit visible scene — F<br><br>"
                       "<b>2D Map View</b><br>"
                       "Pan — left-drag or W / A / S / D<br>"
                       "Zoom — wheel or trackpad scroll<br>"
                       "Zoom in / out — hold Q / Z<br>"
                       "The camera stays top-down and cannot rotate"),
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
        QString::fromStdString(supportedSourceDialogFilter()));
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
    const bool hasLayers = session_->documentSnapshot()->hasAnyLayer();
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

    if (actions_->fitSceneAction_) {
        actions_->fitSceneAction_->setEnabled(hasLayers);
    }
    if (actions_->topDownSceneAction_) {
        actions_->topDownSceneAction_->setEnabled(hasLayers);
    }
    if (actions_->fitSelectedLayerAction_) {
        actions_->fitSelectedLayerAction_->setEnabled(hasSelection);
    }
    if (actions_->isolateSelectedLayerAction_) {
        actions_->isolateSelectedLayerAction_->setEnabled(hasSelection);
    }
    if (actions_->showAllLayersAction_) {
        actions_->showAllLayersAction_->setEnabled(hasLayers);
    }
    if (actions_->removeSelectedLayerAction_) {
        actions_->removeSelectedLayerAction_->setEnabled(hasSelection);
    }
    if (actions_->measureToolAction_) {
        actions_->measureToolAction_->setEnabled(hasLayers);
    }
    if (!hasLayers && actions_->navigateToolAction_ &&
        actions_->measureToolAction_ &&
        actions_->measureToolAction_->isChecked()) {
        actions_->navigateToolAction_->setChecked(true);
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
    qualificationReplayStarted_ = false;
    qualificationReplayFinished_ = false;
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
        const SceneDocumentSnapshotPtr snapshot = session_->documentSnapshot();
        sceneLayersDock_->setDocumentSnapshot(snapshot);
    }
    updateUiContext();
}

void MainWindow::showLayerStatistics(const PointCloudLayerId layerId)
{
    const std::optional<PointCloudLayerSnapshot> layer =
        session_->documentSnapshot()->layer(layerId);
    if (!layer) {
        return;
    }
    auto *dialog = new PointCloudStatisticsDialog(
        layer->descriptor.metadata,
        [this, layerId](StatisticsSubscription::Observer observer) {
            return session_->analyzeStatistics(layerId, std::move(observer));
        },
        this);
    dialog->show();
    dialog->startAnalysis();
}

void MainWindow::showColorizeFromRaster(const PointCloudLayerId layerId)
{
    const SceneDocumentSnapshotPtr document = session_->documentSnapshot();
    const auto layer = document->layer(layerId);
    if (!layer) {
        return;
    }
    ColorizeFromRasterDialog dialog(
        *layer,
        document,
        [this, layerId](const SceneLayerId rasterId) {
            return session_->rasterColorizeResourceEstimate(layerId, rasterId);
        },
        session_->spatialReferenceComparator(),
        session_->availablePointMemoryBytes(),
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
    if (qualificationReplayStarted_ && metrics.qualificationFinalFrame) {
        qualificationReplayStarted_ = false;
        qualificationReplayFinished_ = true;
        viewport_->setContinuousMetricsEnabled(false);
        QTimer::singleShot(0, this, [this] {
            writeQualificationReport(
                QStringLiteral("Qualification camera path complete"));
        });
    }
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
        QTimer::singleShot(0, qApp, [passed = result.qualificationPassed] {
            QCoreApplication::exit(passed ? 0 : 3);
        });
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
