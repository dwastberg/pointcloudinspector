#pragma once

#include <pci/color/PointColorMapCatalog.h>
#include <pci/desktop/config/MemoryBudgetPolicy.h>
#include <pci/desktop/config/PerformanceSettings.h>
#include <pci/desktop/operations/ImportServices.h>
#include <pci/desktop/session/PointCloudLoadMode.h>
#include <pci/desktop/session/SceneSession.h>
#include <pci/desktop/ui/GdalCacheControls.h>
#include <pci/desktop/ui/GdalRuntimeInfo.h>
#include <pci/desktop/ui/QualificationReporter.h>
#include <pci/desktop/viewport/RenderViewport.h>
#include <pci/operations/PointCloudImport.h>
#include <pci/operations/VectorImport.h>

#include <QMainWindow>
#include <QTimer>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

class QAction;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QLabel;

namespace pci {

class RenderViewport;
class LoadingOverlay;
class DiagnosticsDock;
class LayerInspectorDock;
class SceneLayersDock;
class TaskDock;
class WindowActions;

class MainWindow final : public QMainWindow {
public:
    MainWindow(
        std::unique_ptr<RenderViewport> viewport,
        ImportServices importServices,
        std::uint64_t maximumLoadPoints,
        std::uint64_t decodedByteBudget = defaultPointCloudDecodedByteBudget,
        std::optional<AutomaticMemoryBudgetParameters> automaticMemoryBudget =
            std::nullopt,
        LocalPageCacheContextPtr localPageCache = {},
        PointColorMapCatalogSnapshotPtr colorMaps = {});
    ~MainWindow() override;

    void loadPointCloud(const std::filesystem::path &sourcePath);
    void loadPointCloud(const std::filesystem::path &sourcePath,
                        PointCloudLoadMode mode);
    void loadPointClouds(std::vector<std::filesystem::path> sourcePaths,
                         PointCloudLoadMode firstMode);
    void openSources(std::vector<std::filesystem::path> sourcePaths);
    LoadJobId loadVectorLayers(VectorImportRequest request);
    LoadJobId importRasterLayer(RasterImportRequest request);
    // Installed by the application layer, which owns the GDAL link. Applying
    // the current settings once installed is the caller's responsibility.
    void setGdalCacheControls(GdalCacheControls controls);
    void setGdalRuntimeInfo(GdalRuntimeInfo info);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    void configureQualificationReport(std::filesystem::path outputPath,
                                      bool exitAfterWrite = false);
#endif

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void chooseSources();
    void showSettings();
    [[nodiscard]] bool
    applyPerformanceSettings(const PerformanceSettings &settings);
    void applySettingsPreview(const ViewportSettings &viewportSettings,
                              const PerformanceSettings &performanceSettings);
    void showControlsReference();
    void updateUiContext();
    void requestLoadCancellation();
    void refreshLayerPanel();
    void showLayerStatistics(PointCloudLayerId layerId);
    void showColorizeFromRaster(PointCloudLayerId layerId);
    void enqueueVectorSelection(LoadJobId jobId,
                                VectorImportPreflight preflight);
    void showNextVectorSelection();
    void setLoadingProgress(const LoadingProgressState &state);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    void showMetrics(const RenderMetrics &metrics);
#endif
    void showRendererFailure(const QString &message);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    void writeQualificationReport(const QString &status);
#endif

    std::unique_ptr<RenderViewport> viewport_;
    std::unique_ptr<SceneSession> session_;
    std::unique_ptr<WindowActions> actions_;
    LoadingOverlay *loadingOverlay_ = nullptr;
    SceneLayersDock *sceneLayersDock_ = nullptr;
    LayerInspectorDock *layerInspectorDock_ = nullptr;
    TaskDock *taskDock_ = nullptr;
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    DiagnosticsDock *diagnosticsDock_ = nullptr;
#endif
    QLabel *emptySceneLabel_ = nullptr;
    QLabel *navigationHintLabel_ = nullptr;
    struct PendingVectorSelection {
        LoadJobId jobId;
        VectorImportPreflight preflight;
    };
    std::deque<PendingVectorSelection> pendingVectorSelections_;
    std::optional<LoadJobId> activeVectorSelectionJob_;
    bool navigationHintDismissed_ = false;
    PerformanceSettings performanceSettings_;
    GdalCacheControls gdalCache_;
    GdalRuntimeInfo gdalRuntimeInfo_;
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    bool profileLoading_ = false;
    bool qualificationReplayStarted_ = false;
    bool qualificationReplayFinished_ = false;
    QualificationReporter qualificationReporter_;
#endif
};

} // namespace pci
