#pragma once
#include <pci/desktop/session/SceneSession.h>
namespace pci {
struct PointLoadTransaction {
    LoadJobId jobId;
    std::size_t order = 0;
    std::filesystem::path sourcePath;
    QString sourceName;
    PointCloudLoadMode mode = PointCloudLoadMode::Add;
    PointDatasetRuntimePtr scene;
    PreparedPointDatasetPtr importSeed = {};
    SceneDocumentPtr previousDocument;
    SceneRuntimeSnapshotPtr previousRuntime;
    std::optional<PointCloudLayerId> layerId;
    BindingGeneration bindingGeneration;
    bool admitted = false;
    bool importCompleted = false;
    bool firstFrameCompleted = false;
    bool displayCompleted = false;
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

// Concrete point batch transaction: progressive admission, rollback and the
// independent import/first-frame/display milestones share one owner.
class PointSceneCoordinator final {
public:
    using ActiveLoad = PointLoadTransaction;
    explicit PointSceneCoordinator(SceneSession &session);
    void loadPointCloud(const std::filesystem::path &sourcePath,
                        const PointCloudLoadMode mode);
    void loadPointClouds(std::vector<std::filesystem::path> sourcePaths,
                         const PointCloudLoadMode firstMode);
    void beginLoad(const std::filesystem::path &sourcePath,
                   const PointCloudLoadMode mode);
    LoadJobId startLoadJob(const std::filesystem::path &sourcePath,
                           const PointCloudLoadMode mode,
                           const std::size_t order);
    void trackLoadJob(const LoadJobId jobId,
                      const std::filesystem::path &sourcePath,
                      const PointCloudLoadMode mode,
                      const std::size_t order,
                      const std::chrono::steady_clock::time_point dispatchTime);
    void cancelAll();
    void cancelJob(const LoadJobId jobId);
    void retryJob(const LoadJobId jobId);
    void prioritizeJob(const LoadJobId jobId);
    void dismissJob(const LoadJobId jobId);
    void showLoadProgress(const LoadJobId jobId,
                          const PointCloudImportStage stage,
                          const std::uint64_t processed,
                          const std::uint64_t total);
    void onRenderLoadProgress(const RenderLoadProgress &progress);
    void handlePointData(const LoadJobId jobId, const PointDatasetEvent &event);
    void handlePointPrepared(const LoadJobId jobId,
                             const PreparedPointDatasetPtr &dataset);
    void handleSceneReadySingle(ActiveLoad &load);
    void admitBatchLayers();
    void handleLoadCompleted(const LoadJobId jobId,
                             const PreparedPointDatasetPtr &);
    void markFirstFrameReady(ActiveLoad &load);
    void markDisplayCompleted(ActiveLoad &load);
    void finishJobIfComplete(ActiveLoad &load);
    void finalizeLoads(const QString &status);
    void handleLoadCancelled(const LoadJobId jobId);
    void rollbackAdmittedLoad(ActiveLoad &load);
    void showLoadFailure(const LoadJobId jobId, const QString &message);
    void updateBatchProgress();
    ActiveLoad *activeLoad(const LoadJobId jobId);
    ActiveLoad *activeLoadByLayerId(const PointCloudLayerId layerId);

private:
    friend class SceneSession;
    friend class SceneSessionTestAccess;
    friend struct SceneOperationBindings;
    SceneSession &session_;
    LoadingProgressModel loadingProgress_;
    QTimer progressTimer_;
    QElapsedTimer progressStageTimer_;
    std::unordered_map<LoadJobId, ActiveLoad> activeLoads_;
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
