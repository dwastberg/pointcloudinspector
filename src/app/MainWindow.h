#pragma once

#include "app/GdalCacheControls.h"
#include "app/MemoryBudgetPolicy.h"
#include "app/PerformanceSettings.h"
#include "app/PointCloudLoadMode.h"
#include "app/QualificationReporter.h"
#include "app/SceneSession.h"
#include "import/ImportServices.h"
#include "import/PointCloudImport.h"
#include "pointcloud/PointColorMapCatalog.h"
#include "renderer/RenderViewport.h"
#include "vector/VectorImport.h"

#include <QMainWindow>
#include <QTimer>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

class QAction;
class QCloseEvent;
class QLabel;

namespace pci {

class RenderViewport;
class LoadingOverlay;
class DiagnosticsDock;
class LayerInspectorDock;
class SceneLayersDock;
class TaskDock;

class MainWindow final : public QMainWindow {
public:
    MainWindow(
        std::unique_ptr<RenderViewport> viewport,
        ImportServices importServices,
        std::uint64_t maximumLoadPoints,
        std::uint64_t decodedByteBudget = defaultPointCloudDecodedByteBudget,
        std::optional<AutomaticMemoryBudgetParameters> automaticMemoryBudget =
            std::nullopt,
        std::filesystem::path localPageCacheDirectory = {},
        PointColorMapCatalogSnapshotPtr colorMaps = {});
    ~MainWindow() override;

    void loadPointCloud(const std::filesystem::path &sourcePath);
    void loadPointCloud(const std::filesystem::path &sourcePath,
                        PointCloudLoadMode mode);
    void loadPointClouds(std::vector<std::filesystem::path> sourcePaths,
                         PointCloudLoadMode firstMode);
    LoadJobId loadVectorLayers(VectorImportRequest request);
    LoadJobId importRasterLayer(RasterImportRequest request);
    // Installed by the application layer, which owns the GDAL link. Applying
    // the current settings once installed is the caller's responsibility.
    void setGdalCacheControls(GdalCacheControls controls);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    void configureQualificationReport(std::filesystem::path outputPath,
                                      bool exitAfterWrite = false);
#endif

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void choosePointCloud(
        std::optional<PointCloudLoadMode> requestedMode = std::nullopt);
    void chooseVectorLayers();
    void chooseRasterLayers();
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
    QAction *openAction_ = nullptr;
    QAction *addAction_ = nullptr;
    QAction *importVectorAction_ = nullptr;
    QAction *importRasterAction_ = nullptr;
    QAction *fitSceneAction_ = nullptr;
    QAction *topDownSceneAction_ = nullptr;
    QAction *orthographicAction_ = nullptr;
    QAction *eyeDomeLightingAction_ = nullptr;
    QAction *navigateToolAction_ = nullptr;
    QAction *measureToolAction_ = nullptr;
    QAction *fitSelectedLayerAction_ = nullptr;
    QAction *isolateSelectedLayerAction_ = nullptr;
    QAction *showAllLayersAction_ = nullptr;
    QAction *removeSelectedLayerAction_ = nullptr;
    LoadingOverlay *loadingOverlay_ = nullptr;
    SceneLayersDock *sceneLayersDock_ = nullptr;
    LayerInspectorDock *layerInspectorDock_ = nullptr;
    TaskDock *taskDock_ = nullptr;
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    DiagnosticsDock *diagnosticsDock_ = nullptr;
#endif
    QLabel *emptySceneLabel_ = nullptr;
    QLabel *navigationHintLabel_ = nullptr;
    bool navigationHintDismissed_ = false;
    PerformanceSettings performanceSettings_;
    GdalCacheControls gdalCache_;
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    bool profileLoading_ = false;
    QualificationReporter qualificationReporter_;
#endif
};

} // namespace pci
