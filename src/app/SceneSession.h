#pragma once

#include "app/LoadingProgressModel.h"
#include "app/MemoryBudgetPolicy.h"
#include "app/PointCloudLoadMode.h"
#include "import/ImportServices.h"
#include "import/LoadJobRow.h"
#include "import/SupportedSource.h"
#include "renderer/RenderLoadProgress.h"
#include "scene/SceneDocumentSnapshot.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

namespace pci {

enum class VectorImportAvailability : std::uint8_t {
    Unknown,
    Supported,
    Unsupported,
};

struct SceneSessionTimings {
    std::optional<double> timeToFirstPointsMilliseconds;
    std::optional<double> timeToAllFirstPointsMilliseconds;
    std::optional<double> displayReadyMilliseconds;
};

using LoadJobRows = std::vector<LoadJobRow>;

class SceneSession final : public QObject {
    Q_OBJECT

public:
    SceneSession(ImportServices importServices,
                 std::uint64_t maximumLoadPoints,
                 std::uint64_t decodedByteBudget,
                 std::optional<AutomaticMemoryBudgetParameters>
                     automaticMemoryBudget = std::nullopt,
                 LocalPageCacheContextPtr localPageCache = {},
                 PointColorMapCatalogSnapshotPtr colorMaps = {},
                 QObject *parent = nullptr);
    ~SceneSession() override;

    [[nodiscard]] const SceneDocumentPtr &document() const noexcept;
    [[nodiscard]] PointCloudLoadControllerMetrics pointLoadMetrics() const;
    [[nodiscard]] std::vector<PointCloudLoadJobState>
    pointLoadJobStates() const;
    [[nodiscard]] const std::shared_ptr<const PointCloudStatisticsProvider> &
    statisticsProvider() const noexcept;
    [[nodiscard]] bool loading() const noexcept;
    [[nodiscard]] bool batchLoading() const noexcept;
    [[nodiscard]] bool hasActiveVectorLoads() const noexcept;
    [[nodiscard]] bool hasActiveRasterLoads() const noexcept;
    [[nodiscard]] bool hasActiveColorizeJobs() const noexcept;
    [[nodiscard]] bool
    hasActiveColorizeJob(PointCloudLayerId layerId) const noexcept;
    [[nodiscard]] bool
    colorizeCommitInProgress(PointCloudLayerId layerId) const noexcept;
    [[nodiscard]] PointCloudColorizeControllerMetrics
    colorizeMetrics() const noexcept;
    [[nodiscard]] const std::shared_ptr<const SpatialReferenceComparator> &
    spatialReferenceComparator() const noexcept;
    [[nodiscard]] SceneSessionTimings timings() const noexcept;
    [[nodiscard]] std::uint64_t maximumLoadPoints() const noexcept;
    [[nodiscard]] std::uint64_t decodedByteBudget() const noexcept;
    [[nodiscard]] bool automaticMemoryBudgetEnabled() const noexcept;

    void loadPointCloud(const std::filesystem::path &sourcePath,
                        PointCloudLoadMode mode);
    void loadPointClouds(std::vector<std::filesystem::path> sourcePaths,
                         PointCloudLoadMode firstMode);
    void openSources(std::vector<SupportedSource> sources);
    void setVectorImportAvailability(VectorImportAvailability availability,
                                     QString reason = {});
    void cancelAll();
    void cancelJob(LoadJobId jobId);
    void retryJob(LoadJobId jobId);
    void prioritizeJob(LoadJobId jobId);
    void dismissJob(LoadJobId jobId);
    LoadJobId loadVectorLayers(VectorImportRequest request);
    LoadJobId startVectorImport(VectorImportRequest request);
    bool continueVectorImport(LoadJobId jobId,
                              std::vector<VectorSublayerKey> selected);
    // One job per selected path, so a failure in one source preserves the rest.
    LoadJobId startRasterImport(RasterImportRequest request);
    LoadJobId colorizePointCloudFromRaster(PointCloudLayerId pointLayerId,
                                           SceneLayerId rasterLayerId,
                                           RasterColorizeOptions options = {});
    bool revertPointCloudColors(PointCloudLayerId pointLayerId);
    void cancelAllLoads();
    void cancelJob(LoadJobKey key);
    void retryJob(LoadJobKey key);
    void prioritizeJob(LoadJobKey key);
    void dismissJob(LoadJobKey key);
    void setLayerVisible(SceneLayerId layerId, bool visible);
    void setVectorLayerStyle(SceneLayerId layerId, VectorLayerStyle style);
    void setRasterLayerStyle(SceneLayerId layerId, RasterLayerStyle style);
    void setLayerColorMode(PointCloudLayerId layerId, PointColorMode mode);
    void setAllLayerColors(PointColorMode mode);
    void setLayerClassificationFilter(PointCloudLayerId layerId,
                                      PointClassificationFilter filter,
                                      bool applyToAllLayers);
    void removeLayer(SceneLayerId layerId);
    void isolateLayer(SceneLayerId layerId);
    void showAllLayers();
    void onRenderLoadProgress(const RenderLoadProgress &progress);
    void refreshAutomaticMemoryBudget();
    void setMaximumLoadPoints(std::uint64_t maximumPoints);
    [[nodiscard]] bool setDecodedByteBudget(
        std::uint64_t byteBudget,
        std::optional<AutomaticMemoryBudgetParameters> automaticParameters);

signals:
    void documentChanged(pci::SceneDocumentSnapshotPtr snapshot,
                         bool frameVisibleLayers,
                         bool replaceRendererDocument);
    void frameVisibleLayersRequested();
    void loadingChanged(bool loading, bool showOverlay, QString title);
    void loadingProgressChanged(pci::LoadingProgressState state);
    void batchProgressChanged(int percentage, QString details);
    void statusChanged(QString status);
    void taskRowsChanged(pci::LoadJobRows rows);
    void showTasksRequested();
    void failureOccurred(QString title, QString message);
    void vectorSelectionRequired(pci::LoadJobId jobId,
                                 pci::VectorImportPreflight preflight);
    void vectorJobFinished(pci::LoadJobId jobId);

private:
    [[nodiscard]] PointCloudLoadController &pointLoadController() noexcept;
    [[nodiscard]] VectorLoadController &vectorLoadController() noexcept;
    [[nodiscard]] RasterLoadController &rasterLoadController() noexcept;
    [[nodiscard]] PointCloudColorizeController &colorizeController() noexcept;

    struct ActiveLoad {
        LoadJobId jobId;
        std::size_t order = 0;
        std::filesystem::path sourcePath;
        QString sourceName;
        PointCloudLoadMode mode = PointCloudLoadMode::Add;
        PointCloudScenePtr scene;
        SceneDocumentPtr previousDocument;
        std::optional<PointCloudLayerId> layerId;
        bool admitted = false;
        bool importCompleted = false;
        bool firstFrameCompleted = false;
        bool displayCompleted = false;
        bool fullDetailWarming = false;
        std::uint64_t fullDetailDecoded = 0;
        std::uint64_t fullDetailUploaded = 0;
        std::uint64_t fullDetailTotal = 0;
        std::uint64_t renderUploaded = 0;
        std::uint64_t renderUploadTotal = 0;
        PointCloudImportStage importStage = PointCloudImportStage::Reading;
        std::uint64_t importProcessed = 0;
        std::uint64_t importTotal = 0;
        bool incrementalImportProgress = false;
        QString loadedPointCountText;
        std::chrono::steady_clock::time_point dispatchTime;
        std::optional<double> timeToFirstPointsMilliseconds;
        std::optional<double> displayReadyMilliseconds;
    };

    void connectController();
    void connectVectorController();
    void connectRasterController();
    void connectColorizeController();
    void publishTaskRows();
    void beginLoad(const std::filesystem::path &sourcePath,
                   PointCloudLoadMode mode);
    LoadJobId startLoadJob(const std::filesystem::path &sourcePath,
                           PointCloudLoadMode mode,
                           std::size_t order);
    void trackLoadJob(LoadJobId jobId,
                      const std::filesystem::path &sourcePath,
                      PointCloudLoadMode mode,
                      std::size_t order,
                      std::chrono::steady_clock::time_point dispatchTime);
    void showLoadProgress(LoadJobId jobId,
                          PointCloudImportStage stage,
                          std::uint64_t processed,
                          std::uint64_t total);
    void updateBatchProgress();
    void handleSceneReady(LoadJobId jobId, const PointCloudScenePtr &scene);
    void handleSceneReadySingle(ActiveLoad &load);
    void admitBatchLayers();
    void handleLoadCompleted(LoadJobId jobId, const PointCloudScenePtr &scene);
    void handleLoadCompletedSingle(ActiveLoad &load,
                                   const PointCloudScenePtr &scene);
    void markFirstFrameReady(ActiveLoad &load);
    void markDisplayCompleted(ActiveLoad &load);
    void finishJobIfComplete(ActiveLoad &load);
    void finalizeLoads(const QString &status);
    void handleLoadCancelled(LoadJobId jobId);
    void rollbackAdmittedLoad(ActiveLoad &load);
    void showLoadFailure(LoadJobId jobId, const QString &message);
    [[nodiscard]] ActiveLoad *activeLoad(LoadJobId jobId);
    [[nodiscard]] ActiveLoad *activeLoadByLayerId(PointCloudLayerId layerId);
    void publishDocument(bool frameVisibleLayers = false,
                         bool replaceRendererDocument = false);

    ImportServices importServices_;
    PointCloudLoadController *loadController_ = nullptr;
    HierarchyDecodeAdmissionPtr decodeAdmission_;
    PointMemoryBudgetPtr memoryBudget_;
    SceneDocumentPtr document_;
    LoadingProgressModel loadingProgress_;
    QTimer progressTimer_;
    QTimer memoryBudgetRefreshTimer_;
    QElapsedTimer progressStageTimer_;
    std::uint64_t maximumLoadPoints_;
    std::uint64_t decodedByteBudget_;
    std::optional<AutomaticMemoryBudgetParameters> automaticMemoryBudget_;
    LocalPageCacheContextPtr localPageCache_;
    std::unordered_map<LoadJobId, ActiveLoad> activeLoads_;
    std::unordered_map<LoadJobId, std::vector<VectorSublayerKey>>
        preselectedVectorSublayers_;
    std::vector<VectorImportRequest> pendingVectorImports_;
    VectorImportAvailability vectorImportAvailability_ =
        VectorImportAvailability::Unknown;
    QString vectorImportUnavailableReason_;
    std::vector<LoadJobId> batchOrder_;
    std::size_t batchBaseLayerCount_ = 0;
    std::size_t batchTotal_ = 0;
    int batchProgressPercentage_ = 0;
    bool batchProgressUpdatesEnabled_ = false;
    std::uint64_t batchLoadedPoints_ = 0;
    std::uint64_t batchSafetySamplesAtStart_ = 0;
    std::optional<double> batchTimeToFirstPointsMilliseconds_;
    std::optional<double> batchTimeToAllFirstPointsMilliseconds_;
    std::optional<double> batchTimeToAllDisplayReadyMilliseconds_;
    SceneSessionTimings timings_;
    bool batchLoading_ = false;
    bool batchReplacing_ = false;
    bool loading_ = false;
};

} // namespace pci

Q_DECLARE_METATYPE(pci::SceneDocumentSnapshotPtr)
Q_DECLARE_METATYPE(pci::LoadingProgressState)
Q_DECLARE_METATYPE(pci::LoadJobRows)
