#pragma once

#include <pci/color/PointColorMapCatalog.h>
#include <pci/desktop/config/MemoryBudgetPolicy.h>
#include <pci/desktop/config/SupportedSource.h>
#include <pci/desktop/models/LoadingProgressModel.h>
#include <pci/desktop/operations/ImportServices.h>
#include <pci/desktop/operations/LoadJobRow.h>
#include <pci/desktop/session/DocumentUpdate.h>
#include <pci/desktop/session/PointCloudLoadMode.h>
#include <pci/desktop/viewport/RenderLoadProgress.h>
#include <pci/document/SceneDocument.h>
#include <pci/operations/PointCloudImport.h>
#include <pci/operations/PointDatasetInstallation.h>
#include <pci/operations/RasterPointColorize.h>
#include <pci/operations/StatisticsOperation.h>
#include <pci/operations/StorageMaintenanceOperation.h>
#include <pci/runtime/scene/SceneRuntime.h>

#include <pci/foundation/Generation.h>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace pci {

class PointSceneCoordinator;
struct PointLoadTransaction;
class SceneSessionTestAccess;
class ScenePublicationCoordinator;

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
using SceneSessionClock =
    std::function<std::chrono::steady_clock::time_point()>;

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
                 QObject *parent = nullptr,
                 SceneSessionClock clock = {});
    ~SceneSession() override;

    [[nodiscard]] StorageMaintenanceOperation *
    storageMaintenance() const noexcept;

    [[nodiscard]] SceneDocumentSnapshotPtr documentSnapshot() const;
    [[nodiscard]] const PointColorMapCatalogSnapshotPtr &
    colorMaps() const noexcept;
    [[nodiscard]] SceneRuntimeMetrics documentMetrics() const;
    [[nodiscard]] std::optional<PointCloudStorageMetrics>
    pointStorageMetrics(PointCloudLayerId layerId) const;
    [[nodiscard]] RasterColorizeResourceEstimate
    rasterColorizeResourceEstimate(PointCloudLayerId pointLayerId,
                                   SceneLayerId rasterLayerId) const;
    [[nodiscard]] std::uint64_t availablePointMemoryBytes() const noexcept;
    [[nodiscard]] PointCloudLoadControllerMetrics pointLoadMetrics() const;
    [[nodiscard]] std::vector<PointCloudLoadJobState>
    pointLoadJobStates() const;
    [[nodiscard]] StatisticsSubscription
    analyzeStatistics(PointCloudLayerId layerId,
                      StatisticsSubscription::Observer observer);
    [[nodiscard]] bool loading() const noexcept;
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
    [[nodiscard]] SessionGeneration sessionGeneration() const noexcept;

    void loadPointCloud(const std::filesystem::path &sourcePath,
                        PointCloudLoadMode mode);
    void loadPointClouds(std::vector<std::filesystem::path> sourcePaths,
                         PointCloudLoadMode firstMode);
    void openSources(std::vector<SupportedSource> sources);
    void setVectorImportAvailability(VectorImportAvailability availability,
                                     QString reason = {});
    void newScene();
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
    void retryRasterElevation(SceneLayerId layerId);
    void cancelRasterElevation(SceneLayerId layerId);
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
    void documentChanged(pci::DocumentUpdate update);
    void frameVisibleLayersRequested();
    void loadingChanged(bool loading, bool showOverlay, QString title);
    void loadingProgressChanged(pci::LoadingProgressState state);
    void batchProgressChanged(int percentage, QString details);
    void statusChanged(QString status);
    void taskRowsChanged(pci::LoadJobRows rows);
    void taskRowChanged(pci::LoadJobRow row);
    void taskRowRemoved(pci::LoadJobKey key);
    void showTasksRequested();
    void failureOccurred(QString title, QString message);
    void vectorSelectionRequired(pci::LoadJobId jobId,
                                 pci::VectorImportPreflight preflight);
    void vectorJobFinished(pci::LoadJobId jobId);

private:
    friend class SceneSessionTestAccess;
    friend struct SceneOperationBindings;
    friend class PointSceneCoordinator;

    [[nodiscard]] PointCloudLoadController &pointLoadController() noexcept;
    [[nodiscard]] VectorLoadController &vectorLoadController() noexcept;
    [[nodiscard]] RasterLoadController &rasterLoadController() noexcept;
    [[nodiscard]] RasterElevationController &
    rasterElevationController() noexcept;
    [[nodiscard]] PointCloudColorizeController &colorizeController() noexcept;

    using ActiveLoad = PointLoadTransaction;

    void publishTaskRows();
    ScenePublicationCoordinator publicationCoordinator();
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
    void handlePointData(LoadJobId jobId, const PointDatasetEvent &event);
    void handlePointPrepared(LoadJobId jobId,
                             const PreparedPointDatasetPtr &dataset);
    void publishInstalledPointDocument(DocumentUpdate update = {}) noexcept;
    void notifyCommittedDocument(DocumentUpdate update) noexcept;
    void publishPreparedDocument(SceneDocumentPtr document,
                                 SceneRuntime runtime,
                                 DocumentUpdate update = {});
    [[nodiscard]] bool
    commitColorPublication(SceneDocumentPtr candidate,
                           const PointDatasetRuntimePtr &runtime,
                           PointCloudLayerId layerId,
                           std::unique_ptr<PointColorPublication> publication);
    void commitPointPublication(
        ActiveLoad &load, std::unique_ptr<PointDatasetPublication> publication);
    void handleSceneReadySingle(ActiveLoad &load);
    void admitBatchLayers();
    void handleLoadCompleted(LoadJobId jobId,
                             const PreparedPointDatasetPtr &dataset);
    void markFirstFrameReady(ActiveLoad &load);
    void markDisplayCompleted(ActiveLoad &load);
    void finishJobIfComplete(ActiveLoad &load);
    void finalizeLoads(const QString &status);
    void handleLoadCancelled(LoadJobId jobId);
    void rollbackAdmittedLoad(ActiveLoad &load);
    void showLoadFailure(LoadJobId jobId, const QString &message);
    [[nodiscard]] ActiveLoad *activeLoad(LoadJobId jobId);
    [[nodiscard]] ActiveLoad *activeLoadByLayerId(PointCloudLayerId layerId);
    [[nodiscard]] BindingGeneration allocateBindingGeneration();
    void attachRasterRuntime(const RasterLayerDataPtr &data,
                             BindingGeneration generation);
    [[nodiscard]] SceneRuntime runtimeForDocument(
        const SceneDocument &document,
        std::span<const PointRuntimeBinding> preparedPoints = {}) const;
    void installDocument(SceneDocumentPtr document,
                         std::vector<PointRuntimeBinding> preparedPoints = {},
                         bool advanceGeneration = true);
    void publishDocument(DocumentUpdate update = {});

    SceneSessionClock clock_;
    // A private allocation/failure seam used by the transaction tests.
    std::function<void()> beforePointCommit_ = {};
    std::unique_ptr<PointSceneCoordinator> pointLoads_;
    OperationRegistry operationRegistry_;
    ImportServices importServices_;
    std::unique_ptr<StatisticsOperation> statistics_;
    std::unique_ptr<StorageMaintenanceOperation> storageMaintenance_;
    QTimer operationRecoveryTimer_;
    PointCloudLoadController *loadController_ = nullptr;
    HierarchyDecodeAdmissionPtr decodeAdmission_;
    PointMemoryBudgetPtr memoryBudget_;
    SessionGeneration sessionGeneration_{1};
    DocumentGeneration documentGeneration_{1};
    BindingGeneration bindingGeneration_;
    SceneDocumentPtr document_;
    SceneRuntime runtime_;

    QTimer memoryBudgetRefreshTimer_;

    std::uint64_t maximumLoadPoints_;
    std::uint64_t decodedByteBudget_;
    std::optional<AutomaticMemoryBudgetParameters> automaticMemoryBudget_;
    LocalPageCacheContextPtr localPageCache_;

    std::unordered_map<LoadJobId, std::vector<VectorSublayerKey>>
        preselectedVectorSublayers_;
    std::vector<VectorImportRequest> pendingVectorImports_;
    VectorImportAvailability vectorImportAvailability_ =
        VectorImportAvailability::Unknown;
    QString vectorImportUnavailableReason_;
};

} // namespace pci

Q_DECLARE_METATYPE(pci::SceneDocumentSnapshotPtr)
Q_DECLARE_METATYPE(pci::SceneRuntimeSnapshotPtr)
Q_DECLARE_METATYPE(pci::DocumentUpdate)
Q_DECLARE_METATYPE(pci::LoadingProgressState)
Q_DECLARE_METATYPE(pci::LoadJobRows)
