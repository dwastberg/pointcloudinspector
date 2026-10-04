#include "SceneOperationBindings.h"
#include "ScenePublicationCoordinator.h"
#include <pci/desktop/dispatch/QtCompletionExecutor.h>
#include <pci/desktop/session/PointSceneCoordinator.h>
#include <pci/desktop/session/SceneSession.h>

#include <pci/adapters/platform/QtPath.h>
#include <pci/adapters/platform/SystemMemoryInfo.h>
#include <pci/desktop/operations/PointCloudLoadController.h>
#include <pci/desktop/operations/VectorLoadController.h>
#include <pci/foundation/CheckedArithmetic.h>
#include <pci/pointcloud/PointColorPolicy.h>
#include <pci/raster/RasterLayerDisplay.h>

#include <QThread>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace pci {
namespace {
void assertOwnerThread(const QObject &object)
{
    Q_ASSERT(object.thread() == QThread::currentThread());
}
} // namespace
PointSceneCoordinator::PointSceneCoordinator(SceneSession &session)
    : session_(session)
{
    progressTimer_.setInterval(50);
    QObject::connect(&progressTimer_, &QTimer::timeout, &session_, [this] {
        if (!loading_) {
            progressTimer_.stop();
            return;
        }
        const LoadingProgressState state = loadingProgress_.advance(
            std::chrono::milliseconds(progressStageTimer_.elapsed()));
        emit session_.loadingProgressChanged(state);
        if (!state.estimated) {
            progressTimer_.stop();
        }
    });
}
void PointSceneCoordinator::loadPointCloud(
    const std::filesystem::path &sourcePath, const PointCloudLoadMode mode)
{
    assertOwnerThread(session_);
    if (loading_) {
        return;
    }
    session_.refreshAutomaticMemoryBudget();
    beginLoad(sourcePath,
              !session_.document_->hasPointCloudLayers()
                  ? PointCloudLoadMode::Replace
                  : mode);
}

void PointSceneCoordinator::loadPointClouds(
    std::vector<std::filesystem::path> sourcePaths,
    const PointCloudLoadMode firstMode)
{
    assertOwnerThread(session_);
    if (sourcePaths.empty() || loading_) {
        return;
    }
    session_.refreshAutomaticMemoryBudget();
    if (sourcePaths.size() == 1) {
        beginLoad(sourcePaths.front(), firstMode);
        return;
    }
    const bool replace = !session_.document_->hasPointCloudLayers() ||
                         firstMode == PointCloudLoadMode::Replace;
    if (replace && session_.document_->hasAnyLayer()) {
        auto replacement =
            std::make_shared<SceneDocument>(session_.document_->colorMaps());
        static_cast<void>(
            replacement->copyOverlayLayersFrom(*session_.document_));
        session_.installDocument(std::move(replacement));
        session_.publishDocument();
    }

    loading_ = true;
    batchLoading_ = true;
    batchReplacing_ = replace;
    batchTotal_ = sourcePaths.size();
    batchProgressPercentage_ = 0;
    batchProgressUpdatesEnabled_ = false;
    batchLoadedPoints_ = 0;
    batchSafetySamplesAtStart_ =
        session_.loadController_->metrics().safetySampledSources;
    batchTimeToFirstPointsMilliseconds_.reset();
    batchTimeToAllFirstPointsMilliseconds_.reset();
    batchTimeToAllDisplayReadyMilliseconds_.reset();
    timings_ = {};
    batchBaseLayerCount_ = session_.document_->layerCount();
    batchOrder_.clear();
    batchOrder_.reserve(sourcePaths.size());
    progressTimer_.stop();
    emit session_.loadingChanged(
        true,
        replace,
        QStringLiteral("%1 point clouds").arg(sourcePaths.size()));
    if (!replace) {
        emit session_.showTasksRequested();
    }
    emit session_.batchProgressChanged(
        0, QStringLiteral("Queued %1 point clouds").arg(sourcePaths.size()));

    std::vector<PointCloudLoadRequest> requests;
    requests.reserve(sourcePaths.size());
    for (const std::filesystem::path &sourcePath : sourcePaths) {
        requests.push_back({
            .options =
                {
                    .sourcePath = sourcePath,
                    .maximumPoints = session_.maximumLoadPoints_,
                    .localPaging =
                        {
                            .cache = session_.localPageCache_,
                        },
                },
            .resources =
                {
                    .decodedByteBudget = session_.decodedByteBudget_,
                    .residency = session_.runtime_.residencyCoordinator(),
                    .memoryBudget = session_.memoryBudget_,
                    .flatReservation = {},
                },
            .session = session_.sessionGeneration_,
        });
    }
    const auto dispatchTime = session_.clock_();
    batchOrder_ = session_.loadController_->loadBatch(std::move(requests));
    for (std::size_t index = 0; index < sourcePaths.size(); ++index) {
        trackLoadJob(batchOrder_[index],
                     sourcePaths[index],
                     PointCloudLoadMode::Add,
                     index,
                     dispatchTime);
    }
    QTimer::singleShot(50, &session_, [this] {
        if (batchLoading_) {
            batchProgressUpdatesEnabled_ = true;
            updateBatchProgress();
        }
    });
    emit session_.statusChanged(
        QStringLiteral("Loading %1 point clouds…").arg(sourcePaths.size()));
}

void PointSceneCoordinator::beginLoad(const std::filesystem::path &sourcePath,
                                      const PointCloudLoadMode mode)
{
    loading_ = true;
    batchLoading_ = false;
    batchReplacing_ = false;
    timings_ = {};
    loadingProgress_.reset();
    progressTimer_.stop();
    const bool showOverlay = mode == PointCloudLoadMode::Replace ||
                             !session_.document_->hasPointCloudLayers();
    emit session_.loadingChanged(true, showOverlay, pathToQString(sourcePath));
    if (!showOverlay) {
        emit session_.showTasksRequested();
    }
    emit session_.loadingProgressChanged(loadingProgress_.state());
    startLoadJob(sourcePath, mode, 0);
    emit session_.statusChanged(
        QStringLiteral("Loading %1…").arg(displayPathName(sourcePath)));
}

LoadJobId
PointSceneCoordinator::startLoadJob(const std::filesystem::path &sourcePath,
                                    const PointCloudLoadMode mode,
                                    const std::size_t order)
{
    const auto dispatchTime = session_.clock_();
    const LoadJobId jobId = session_.loadController_->load({
        .options =
            {
                .sourcePath = sourcePath,
                .maximumPoints = session_.maximumLoadPoints_,
                .localPaging =
                    {
                        .cache = session_.localPageCache_,
                    },
            },
        .resources =
            {
                .decodedByteBudget = session_.decodedByteBudget_,
                .residency = session_.runtime_.residencyCoordinator(),
                .memoryBudget = session_.memoryBudget_,
                .flatReservation = {},
            },
        .session = session_.sessionGeneration_,
    });
    trackLoadJob(jobId, sourcePath, mode, order, dispatchTime);
    return jobId;
}

void PointSceneCoordinator::trackLoadJob(
    const LoadJobId jobId,
    const std::filesystem::path &sourcePath,
    const PointCloudLoadMode mode,
    const std::size_t order,
    const std::chrono::steady_clock::time_point dispatchTime)
{
    activeLoads_.emplace(jobId,
                         ActiveLoad{
                             .jobId = jobId,
                             .order = order,
                             .sourcePath = sourcePath,
                             .sourceName = displayPathName(sourcePath),
                             .mode = mode,
                             .scene = {},
                             .previousDocument = {},
                             .previousRuntime = {},
                             .layerId = std::nullopt,
                             .bindingGeneration = {},
                             .admitted = false,
                             .importCompleted = false,
                             .firstFrameCompleted = false,
                             .displayCompleted = false,
                             .renderUploaded = 0,
                             .renderUploadTotal = 0,
                             .importStage = PointCloudImportStage::Reading,
                             .importProcessed = 0,
                             .importTotal = 0,
                             .incrementalImportProgress = false,
                             .loadedPointCountText = {},
                             .dispatchTime = dispatchTime,
                             .timeToFirstPointsMilliseconds = std::nullopt,
                             .displayReadyMilliseconds = std::nullopt,
                         });
}

void PointSceneCoordinator::cancelAll()
{
    assertOwnerThread(session_);
    session_.loadController_->cancel();
}

void PointSceneCoordinator::cancelJob(const LoadJobId jobId)
{
    assertOwnerThread(session_);
    if (const ActiveLoad *load = activeLoad(jobId); load && load->layerId) {
        session_.setLayerVisible(*load->layerId, false);
    }
    session_.loadController_->cancel(jobId);
}

void PointSceneCoordinator::retryJob(const LoadJobId jobId)
{
    assertOwnerThread(session_);
    const auto state = session_.loadController_->jobState(jobId);
    if (!state || !state->canRetry || loading_) {
        return;
    }
    const std::filesystem::path sourcePath = state->sourcePath;
    static_cast<void>(session_.loadController_->dismiss(jobId));
    loadPointCloud(sourcePath,
                   !session_.document_->hasPointCloudLayers()
                       ? PointCloudLoadMode::Replace
                       : PointCloudLoadMode::Add);
}

void PointSceneCoordinator::prioritizeJob(const LoadJobId jobId)
{
    assertOwnerThread(session_);
    if (session_.loadController_->prioritize(jobId)) {
        emit session_.statusChanged(
            QStringLiteral("Point-cloud source prioritized."));
    }
}

void PointSceneCoordinator::dismissJob(const LoadJobId jobId)
{
    assertOwnerThread(session_);
    if (session_.loadController_->dismiss(jobId)) {
        session_.publishTaskRows();
    }
}

void PointSceneCoordinator::showLoadProgress(const LoadJobId jobId,
                                             const PointCloudImportStage stage,
                                             const std::uint64_t processed,
                                             const std::uint64_t total)
{
    assertOwnerThread(session_);
    ActiveLoad *load = activeLoad(jobId);
    if (!loading_ || !load) {
        return;
    }
    load->importStage = stage;
    load->importProcessed = processed;
    load->importTotal = total;
    load->incrementalImportProgress =
        load->incrementalImportProgress ||
        (total > 0 && processed > 0 && processed < total);
    if (batchLoading_) {
        updateBatchProgress();
        return;
    }
    const LoadingProgressPhase previous = loadingProgress_.state().phase;
    const LoadingProgressState state =
        loadingProgress_.updateImport(stage, processed, total);
    emit session_.loadingProgressChanged(state);
    if (state.estimated && state.phase != previous) {
        progressStageTimer_.restart();
        progressTimer_.start();
    } else if (!state.estimated) {
        progressTimer_.stop();
    }
    emit session_.statusChanged(
        QStringLiteral("Loading %1…").arg(load->sourceName));
}

void PointSceneCoordinator::onRenderLoadProgress(
    const RenderLoadProgress &progress)
{
    assertOwnerThread(session_);
    ActiveLoad *load = activeLoadByLayerId(progress.layerId);
    if (!loading_ || !load ||
        load->bindingGeneration != progress.bindingGeneration || !load->scene ||
        !session_.runtime_.point(load->scene->sourceId(),
                                 progress.bindingGeneration)) {
        return;
    }
    if (!batchLoading_) {
        const LoadingProgressPhase previous = loadingProgress_.state().phase;
        const LoadingProgressState state =
            loadingProgress_.updateRender(progress);
        emit session_.loadingProgressChanged(state);
        if (state.estimated && state.phase != previous) {
            progressStageTimer_.restart();
            progressTimer_.start();
        } else if (!state.estimated) {
            progressTimer_.stop();
        }
    }
    if (progress.stage == RenderLoadStage::Uploading) {
        load->renderUploaded = progress.completed;
        load->renderUploadTotal = progress.total;
    } else if (progress.stage == RenderLoadStage::FirstFrameReady) {
        markFirstFrameReady(*load);
    } else if (progress.stage == RenderLoadStage::DisplayReady) {
        markFirstFrameReady(*load);
        markDisplayCompleted(*load);
    }
    if (batchLoading_) {
        updateBatchProgress();
    }
}

void PointSceneCoordinator::handlePointData(const LoadJobId jobId,
                                            const PointDatasetEvent &event)
{
    assertOwnerThread(session_);
    ActiveLoad *load = activeLoad(jobId);
    if (!load || event.token.session != session_.sessionGeneration_ ||
        event.token.attempt != jobId.value()) {
        session_.loadController_->cancel(jobId);
        return;
    }
    if (const auto *seed = std::get_if<PreparedPointDatasetPtr>(&event.data)) {
        if (load->scene || !*seed) {
            throw std::invalid_argument("duplicate or invalid point preview");
        }
        load->importSeed = *seed;
        load->scene =
            createPointDatasetRuntime(*seed, session_.decodedByteBudget_);
        if (batchLoading_) {
            admitBatchLayers();
        } else {
            loadingProgress_.setPagedSource(load->scene->hierarchical());
            handleSceneReadySingle(*load);
        }
    } else {
        const auto &block = std::get<PreparedPointBlock>(event.data);
        if (!load->scene || !load->admitted ||
            block.sourceId != load->scene->sourceId() ||
            session_.runtime_.point(block.sourceId, load->bindingGeneration) !=
                load->scene ||
            block.index != load->scene->blockEntries().size()) {
            throw std::invalid_argument(
                "point block targets a stale binding or index");
        }
        session_.commitPointPublication(
            *load, load->scene->preparePublication(block.block));
    }
}

void PointSceneCoordinator::handlePointPrepared(
    const LoadJobId jobId, const PreparedPointDatasetPtr &dataset)
{
    assertOwnerThread(session_);
    auto *load = activeLoad(jobId);
    if (!load) {
        session_.loadController_->cancel(jobId);
        return;
    }
    if (!load->scene) {
        load->scene =
            createPointDatasetRuntime(dataset, session_.decodedByteBudget_);
        load->importSeed = dataset;
        if (batchLoading_) {
            admitBatchLayers();
        } else {
            handleSceneReadySingle(*load);
        }
    } else {
        if (!load->importSeed || load->importSeed->source != dataset->source ||
            load->importSeed->root != dataset->root ||
            load->importSeed->reservation != dataset->reservation ||
            session_.runtime_.point(load->scene->sourceId(),
                                    load->bindingGeneration) != load->scene) {
            throw std::invalid_argument(
                "point completion targets a stale source");
        }
        validatePointDatasetCompletion(load->scene, dataset);
        session_.commitPointPublication(
            *load, load->scene->preparePublication({}, true));
    }
    if (auto *current = activeLoad(jobId)) {
        current->importSeed.reset();
    }
}

void PointSceneCoordinator::handleSceneReadySingle(ActiveLoad &load)
{
    const bool replacing = load.mode == PointCloudLoadMode::Replace;
    const bool keepRollback = replacing && session_.document_->hasAnyLayer();
    auto previousDocument =
        keepRollback ? session_.document_ : SceneDocumentPtr{};
    auto previousRuntime =
        keepRollback ? session_.runtime_.snapshot() : SceneRuntimeSnapshotPtr{};
    auto candidate =
        replacing
            ? std::make_shared<SceneDocument>(session_.document_->colorMaps())
            : std::make_shared<SceneDocument>(*session_.document_);
    if (replacing) {
        static_cast<void>(
            candidate->copyOverlayLayersFrom(*session_.document_));
    }
    const auto generation = session_.allocateBindingGeneration();
    const auto layer =
        candidate->addLayer(load.scene->datasetView(), generation);
    session_.installDocument(std::move(candidate),
                             {{.descriptor = load.scene->descriptor(),
                               .runtime = load.scene,
                               .generation = generation}},
                             keepRollback);
    load.previousDocument = std::move(previousDocument);
    load.previousRuntime = std::move(previousRuntime);
    load.bindingGeneration = generation;
    load.layerId = layer;
    load.admitted = true;
    session_.publishInstalledPointDocument({
        .sessionGeneration = {},
        .snapshot = {},
        .runtime = {},
        .runtimeBudget = {},
        .viewAdjustment = replacing ? ViewAdjustment::FrameVisibleLayers
                                    : ViewAdjustment::Preserve,
        .rendererPolicy = replacing ? RendererDocumentPolicy::ResetPointView
                                    : RendererDocumentPolicy::Reconcile,
    });
}

void PointSceneCoordinator::admitBatchLayers()
{
    for (std::size_t order = 0; order < batchOrder_.size(); ++order) {
        ActiveLoad *load = activeLoad(batchOrder_[order]);
        if (!load || !load->scene || load->admitted) {
            continue;
        }
        std::size_t earlierAdmissions = 0;
        for (std::size_t earlier = 0; earlier < order; ++earlier) {
            const ActiveLoad *candidate = activeLoad(batchOrder_[earlier]);
            if (candidate && candidate->admitted) {
                ++earlierAdmissions;
            }
        }
        const bool hadNoPointClouds =
            !session_.document_->hasPointCloudLayers();
        const auto generation = session_.allocateBindingGeneration();
        auto candidate = std::make_shared<SceneDocument>(*session_.document_);
        const auto layerId =
            candidate->insertLayer(load->scene->datasetView(),
                                   batchBaseLayerCount_ + earlierAdmissions,
                                   generation);
        session_.installDocument(std::move(candidate),
                                 {{.descriptor = load->scene->descriptor(),
                                   .runtime = load->scene,
                                   .generation = generation}},
                                 false);
        load->bindingGeneration = generation;
        load->layerId = layerId;
        load->admitted = true;
        if (hadNoPointClouds) {
            session_.publishInstalledPointDocument(
                {.sessionGeneration = {},
                 .snapshot = {},
                 .runtime = {},
                 .runtimeBudget = {},
                 .viewAdjustment = ViewAdjustment::FrameVisibleLayers,
                 .rendererPolicy = RendererDocumentPolicy::ResetPointView});
        } else if (batchReplacing_) {
            emit session_.frameVisibleLayersRequested();
        }
    }
    session_.publishInstalledPointDocument();
    updateBatchProgress();
}

void PointSceneCoordinator::handleLoadCompleted(const LoadJobId jobId,
                                                const PreparedPointDatasetPtr &)
{
    assertOwnerThread(session_);
    ActiveLoad *load = activeLoad(jobId);
    if (!load) {
        return;
    }
    const auto scene = load->scene;
    load->importCompleted = true;
    if (!batchLoading_) {
        emit session_.loadingProgressChanged(loadingProgress_.completeImport());
    }
    load->loadedPointCountText = QStringLiteral("Loaded %1 | %2 points")
                                     .arg(load->sourceName)
                                     .arg(scene->totalPointCount());
    if (scene->totalPointCount() == 0) {
        markFirstFrameReady(*load);
        markDisplayCompleted(*load);
        return;
    }
    finishJobIfComplete(*load);
}

void PointSceneCoordinator::markFirstFrameReady(ActiveLoad &load)
{
    if (load.firstFrameCompleted) {
        return;
    }
    load.firstFrameCompleted = true;
    if (load.scene && load.scene->totalPointCount() > 0) {
        load.timeToFirstPointsMilliseconds =
            std::chrono::duration<double, std::milli>(session_.clock_() -
                                                      load.dispatchTime)
                .count();
    }
    if (batchLoading_ && load.timeToFirstPointsMilliseconds) {
        const double value = *load.timeToFirstPointsMilliseconds;
        if (!batchTimeToFirstPointsMilliseconds_ ||
            value < *batchTimeToFirstPointsMilliseconds_) {
            batchTimeToFirstPointsMilliseconds_ = value;
        }
        if (!batchTimeToAllFirstPointsMilliseconds_ ||
            value > *batchTimeToAllFirstPointsMilliseconds_) {
            batchTimeToAllFirstPointsMilliseconds_ = value;
        }
    }
}

void PointSceneCoordinator::markDisplayCompleted(ActiveLoad &load)
{
    if (load.displayCompleted) {
        return;
    }
    load.displayCompleted = true;
    load.displayReadyMilliseconds = std::chrono::duration<double, std::milli>(
                                        session_.clock_() - load.dispatchTime)
                                        .count();
    if (batchLoading_ && (!batchTimeToAllDisplayReadyMilliseconds_ ||
                          *load.displayReadyMilliseconds >
                              *batchTimeToAllDisplayReadyMilliseconds_)) {
        batchTimeToAllDisplayReadyMilliseconds_ = load.displayReadyMilliseconds;
    }
    finishJobIfComplete(load);
}

void PointSceneCoordinator::finishJobIfComplete(ActiveLoad &load)
{
    if (!load.importCompleted || !load.displayCompleted) {
        return;
    }
    QString countText = load.loadedPointCountText;
    const auto first = load.timeToFirstPointsMilliseconds;
    const auto display = load.displayReadyMilliseconds;
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    if (first) {
        countText +=
            QStringLiteral(" | first points %1 ms").arg(*first, 0, 'f', 1);
    }
#endif
    if (load.scene) {
        batchLoadedPoints_ =
            saturatingAdd(batchLoadedPoints_, load.scene->totalPointCount());
    }
    activeLoads_.erase(load.jobId);
    if (!activeLoads_.empty()) {
        updateBatchProgress();
        return;
    }
    timings_ = {
        .timeToFirstPointsMilliseconds =
            batchLoading_ ? batchTimeToFirstPointsMilliseconds_ : first,
        .timeToAllFirstPointsMilliseconds =
            batchLoading_ ? batchTimeToAllFirstPointsMilliseconds_ : first,
        .displayReadyMilliseconds =
            batchLoading_ ? batchTimeToAllDisplayReadyMilliseconds_ : display,
    };
    QString status = batchLoading_
                         ? QStringLiteral("Loaded %1 point clouds | %2 points")
                               .arg(batchTotal_)
                               .arg(batchLoadedPoints_)
                         : countText;
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    if (batchLoading_ && batchTimeToFirstPointsMilliseconds_) {
        status += QStringLiteral(" | first points %1 ms")
                      .arg(*batchTimeToFirstPointsMilliseconds_, 0, 'f', 1);
    }
#endif
    finalizeLoads(status);
}

void PointSceneCoordinator::finalizeLoads(const QString &status)
{
    progressTimer_.stop();
    loading_ = false;
    batchLoading_ = false;
    batchReplacing_ = false;
    batchProgressUpdatesEnabled_ = false;
    batchOrder_.clear();
    batchTotal_ = 0;
    batchSafetySamplesAtStart_ = 0;
    batchTimeToFirstPointsMilliseconds_.reset();
    batchTimeToAllFirstPointsMilliseconds_.reset();
    batchTimeToAllDisplayReadyMilliseconds_.reset();
    emit session_.loadingChanged(false, false, {});
    if (!status.isEmpty()) {
        emit session_.statusChanged(status);
    }
    session_.publishTaskRows();
}

void PointSceneCoordinator::handleLoadCancelled(const LoadJobId jobId)
{
    assertOwnerThread(session_);
    ActiveLoad *load = activeLoad(jobId);
    if (!load) {
        session_.publishTaskRows();
        return;
    }
    rollbackAdmittedLoad(*load);
    activeLoads_.erase(jobId);
    if (batchLoading_) {
        admitBatchLayers();
        if (activeLoads_.empty()) {
            finalizeLoads(QStringLiteral("Loading cancelled"));
        } else {
            updateBatchProgress();
        }
        return;
    }
    activeLoads_.clear();
    finalizeLoads(QStringLiteral("Loading cancelled"));
}

void PointSceneCoordinator::rollbackAdmittedLoad(ActiveLoad &load)
{
    if (!load.admitted) {
        return;
    }
    if (load.previousDocument) {
        auto restored = std::make_shared<SceneDocument>(*load.previousDocument);
        std::vector<PointRuntimeBinding> restoredPoints;
        restoredPoints.reserve(restored->layerCount());
        for (const PointCloudLayer &layer : restored->layers()) {
            const PointDatasetRuntimePtr pointRuntime =
                load.previousRuntime
                    ? load.previousRuntime->point(layer.descriptor.sourceId,
                                                  layer.bindingGeneration)
                    : PointDatasetRuntimePtr{};
            if (!pointRuntime) {
                throw std::logic_error(
                    "restored point runtime binding is unavailable");
            }
            const BindingGeneration binding =
                session_.allocateBindingGeneration();
            const bool updated =
                restored->setLayerBindingGeneration(layer.id, binding);
            if (!updated) {
                throw std::logic_error(
                    "restored point binding generation was not accepted");
            }
            restoredPoints.push_back({.descriptor = layer.descriptor,
                                      .runtime = pointRuntime,
                                      .generation = binding,
                                      .active = layer.visible});
        }
        session_.installDocument(std::move(restored),
                                 std::move(restoredPoints));
        load.previousDocument.reset();
        load.previousRuntime.reset();
        session_.publishInstalledPointDocument(
            {.sessionGeneration = {},
             .snapshot = {},
             .runtime = {},
             .runtimeBudget = {},
             .viewAdjustment = ViewAdjustment::FrameVisibleLayers,
             .rendererPolicy = RendererDocumentPolicy::ResetPointView});
    } else if (load.layerId) {
        auto candidate = std::make_shared<SceneDocument>(*session_.document_);
        static_cast<void>(candidate->removeLayer(*load.layerId));
        session_.installDocument(std::move(candidate), {}, false);
        session_.publishInstalledPointDocument();
    }
    load.layerId.reset();
    load.admitted = false;
}

void PointSceneCoordinator::showLoadFailure(const LoadJobId jobId,
                                            const QString &message)
{
    assertOwnerThread(session_);
    ActiveLoad *load = activeLoad(jobId);
    if (!load) {
        session_.publishTaskRows();
        return;
    }
    if (batchLoading_) {
        rollbackAdmittedLoad(*load);
        activeLoads_.erase(jobId);
        admitBatchLayers();
        emit session_.statusChanged(
            QStringLiteral("Failed to load a point cloud: %1").arg(message));
        if (activeLoads_.empty()) {
            finalizeLoads(QStringLiteral("Loaded %1 point clouds | %2 points")
                              .arg(batchTotal_)
                              .arg(batchLoadedPoints_));
        } else {
            updateBatchProgress();
        }
        return;
    }
    rollbackAdmittedLoad(*load);
    activeLoads_.clear();
    progressTimer_.stop();
    loading_ = false;
    emit session_.loadingChanged(false, false, {});
    emit session_.statusChanged(
        QStringLiteral("Loading failed: %1").arg(message));
    emit session_.failureOccurred(QStringLiteral("Point cloud loading failed"),
                                  message);
    session_.publishTaskRows();
}

void PointSceneCoordinator::updateBatchProgress()
{
    if (!batchLoading_ || batchTotal_ == 0 || !batchProgressUpdatesEnabled_) {
        return;
    }
    std::vector<PointCloudLoadJobState> batchStates;
    batchStates.reserve(batchOrder_.size());
    for (const LoadJobId jobId : batchOrder_) {
        if (const auto state = session_.loadController_->jobState(jobId)) {
            batchStates.push_back(*state);
        }
    }
    const PointCloudBatchProgress batchProgress =
        weightedBatchProgress(batchStates);
    long double aggregate = 0.0L;
    for (const LoadJobId jobId : batchOrder_) {
        const ActiveLoad *load = activeLoad(jobId);
        if (!load) {
            aggregate += 1.0L;
            continue;
        }
        const auto state = session_.loadController_->jobState(jobId);
        const bool paged = (load->scene && load->scene->hierarchical()) ||
                           (state && state->localPaging);
        const long double pagedPreparation =
            load->incrementalImportProgress ? 0.55L : 0.05L;
        long double completion = 0.0L;
        if (paged) {
            if (load->importCompleted) {
                completion = pagedPreparation;
            } else if (load->importTotal > 0) {
                const long double fraction =
                    std::clamp(static_cast<long double>(load->importProcessed) /
                                   static_cast<long double>(load->importTotal),
                               0.0L,
                               1.0L);
                completion =
                    load->incrementalImportProgress
                        ? (load->importStage == PointCloudImportStage::Reading
                               ? 0.45L * fraction
                               : 0.45L + 0.10L * fraction)
                        : 0.05L * fraction;
            } else if (state) {
                completion = std::min(
                    0.05L,
                    static_cast<long double>(state->weightedCompletion()) *
                        0.05L);
            }
        } else {
            completion =
                state ? static_cast<long double>(state->weightedCompletion()) *
                            0.75L
                      : 0.0L;
        }
        if (load->importCompleted && load->renderUploadTotal > 0) {
            const long double fraction = std::clamp(
                static_cast<long double>(load->renderUploaded) /
                    static_cast<long double>(load->renderUploadTotal),
                0.0L,
                1.0L);
            completion = std::max(
                completion,
                paged ? pagedPreparation + (0.99L - pagedPreparation) * fraction
                      : 0.75L + 0.24L * fraction);
        }
        if (!paged && load->importCompleted && load->firstFrameCompleted) {
            completion = std::max(completion, 0.80L);
        }
        aggregate += std::min(completion, 0.99L);
    }
    const long double completion =
        batchOrder_.empty()
            ? 0.0L
            : aggregate / static_cast<long double>(batchOrder_.size());
    batchProgressPercentage_ = std::max(
        batchProgressPercentage_,
        static_cast<int>(std::clamp(completion * 100.0L, 0.0L, 99.0L)));
    const std::size_t finished = batchTotal_ - activeLoads_.size();
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    const PointCloudLoadControllerMetrics metrics =
        session_.loadController_->metrics();
    const std::uint64_t sampled =
        metrics.safetySampledSources - batchSafetySamplesAtStart_;
    QString details =
        QStringLiteral("Loaded %1 of %2 point clouds; %3 active, %4 queued; "
                       "%5 MiB active estimate")
            .arg(finished)
            .arg(batchTotal_)
            .arg(metrics.scheduler.active)
            .arg(metrics.scheduler.pending)
            .arg(metrics.scheduler.activeEstimatedBytes /
                 (std::uint64_t{1024} * 1024));
    if (batchProgress.failedSources > 0 || batchProgress.cancelledSources > 0) {
        details += QStringLiteral("; %1 failed, %2 cancelled")
                       .arg(batchProgress.failedSources)
                       .arg(batchProgress.cancelledSources);
    }
    if (sampled > 0) {
        details +=
            QStringLiteral("; %1 spatial safety previews (detail reduced)")
                .arg(sampled);
    }
#else
    QString details = QStringLiteral("Loading %1 of %2 point clouds…")
                          .arg(finished)
                          .arg(batchTotal_);
    if (batchProgress.failedSources > 0 || batchProgress.cancelledSources > 0) {
        details += QStringLiteral(" %1 failed, %2 cancelled.")
                       .arg(batchProgress.failedSources)
                       .arg(batchProgress.cancelledSources);
    }
#endif
    emit session_.batchProgressChanged(batchProgressPercentage_, details);
}

PointSceneCoordinator::ActiveLoad *
PointSceneCoordinator::activeLoad(const LoadJobId jobId)
{
    const auto found = activeLoads_.find(jobId);
    return found == activeLoads_.end() ? nullptr : &found->second;
}

PointSceneCoordinator::ActiveLoad *
PointSceneCoordinator::activeLoadByLayerId(const PointCloudLayerId layerId)
{
    for (auto &[jobId, load] : activeLoads_) {
        static_cast<void>(jobId);
        if (load.layerId == layerId) {
            return &load;
        }
    }
    return nullptr;
}
} // namespace pci
