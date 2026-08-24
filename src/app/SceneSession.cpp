#include "app/SceneSession.h"

#include "foundation/CheckedArithmetic.h"
#include "import/PointCloudLoadController.h"
#include "import/VectorLoadController.h"
#include "platform/QtPath.h"
#include "platform/SystemMemoryInfo.h"
#include "pointcloud/PointColorPolicy.h"
#include "scene/RasterLayerDisplay.h"

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

[[nodiscard]] QString pointLayerName(const PointCloudLayer &layer)
{
    return layer.scene->metadata().sourcePath.empty()
               ? QStringLiteral("Point cloud %1").arg(layer.id.value())
               : displayPathName(layer.scene->metadata().sourcePath);
}

[[nodiscard]] QString rasterLayerName(const RasterLayer &layer)
{
    return layer.data->metadata().sourcePath.empty()
               ? QStringLiteral("Raster layer %1").arg(layer.id.value())
               : displayPathName(layer.data->metadata().sourcePath);
}

} // namespace

SceneSession::SceneSession(
    ImportServices importServices,
    const std::uint64_t maximumLoadPoints,
    const std::uint64_t decodedByteBudget,
    std::optional<AutomaticMemoryBudgetParameters> automaticMemoryBudget,
    std::filesystem::path localPageCacheDirectory,
    PointColorMapCatalogSnapshotPtr colorMaps,
    QObject *parent)
    : QObject(parent)
    , importServices_(std::move(importServices))
    , decodeAdmission_(std::make_shared<HierarchyDecodeAdmission>())
    , memoryBudget_(std::make_shared<PointMemoryBudget>(decodedByteBudget))
    , document_(std::make_shared<SceneDocument>(
          decodedByteBudget,
          HierarchyResidencyCoordinator::defaultMaximumConcurrentDecodes,
          decodeAdmission_,
          memoryBudget_,
          std::move(colorMaps)))
    , maximumLoadPoints_(maximumLoadPoints)
    , decodedByteBudget_(decodedByteBudget)
    , automaticMemoryBudget_(automaticMemoryBudget)
    , localPageCacheDirectory_(std::move(localPageCacheDirectory))
{
    qRegisterMetaType<SceneDocumentSnapshotPtr>();
    qRegisterMetaType<LoadingProgressState>();
    qRegisterMetaType<LoadJobRows>();
    qRegisterMetaType<VectorImportPreflight>();
    if (!importServices_.valid()) {
        throw std::invalid_argument(
            "scene session requires valid import services");
    }
    if (maximumLoadPoints_ == 0 || decodedByteBudget_ == 0) {
        throw std::invalid_argument(
            "scene session budgets must be greater than zero");
    }
    loadController_ = importServices_.pointCloud.get();
    progressTimer_.setInterval(50);
    connect(&progressTimer_, &QTimer::timeout, this, [this] {
        if (!loading_) {
            progressTimer_.stop();
            return;
        }
        const LoadingProgressState state = loadingProgress_.advance(
            std::chrono::milliseconds(progressStageTimer_.elapsed()));
        emit loadingProgressChanged(state);
        if (!state.estimated) {
            progressTimer_.stop();
        }
    });
    memoryBudgetRefreshTimer_.setInterval(5000);
    connect(&memoryBudgetRefreshTimer_,
            &QTimer::timeout,
            this,
            &SceneSession::refreshAutomaticMemoryBudget);
    memoryBudgetRefreshTimer_.start();
    connectController();
    connectVectorController();
    connectRasterController();
    connectColorizeController();
}

SceneSession::~SceneSession()
{
    loadController_ = nullptr;
    importServices_.shutdown();
}

const SceneDocumentPtr &SceneSession::document() const noexcept
{
    return document_;
}

PointCloudLoadController &SceneSession::pointLoadController() noexcept
{
    return *importServices_.pointCloud;
}

PointCloudLoadControllerMetrics SceneSession::pointLoadMetrics() const
{
    return importServices_.pointCloud->metrics();
}

std::vector<PointCloudLoadJobState> SceneSession::pointLoadJobStates() const
{
    return importServices_.pointCloud->jobStates();
}

VectorLoadController &SceneSession::vectorLoadController() noexcept
{
    return *importServices_.vector;
}

RasterLoadController &SceneSession::rasterLoadController() noexcept
{
    return *importServices_.raster;
}

PointCloudColorizeController &SceneSession::colorizeController() noexcept
{
    return *importServices_.colorize;
}

const std::shared_ptr<const PointCloudStatisticsProvider> &
SceneSession::statisticsProvider() const noexcept
{
    return importServices_.statistics;
}

bool SceneSession::loading() const noexcept
{
    return loading_;
}

bool SceneSession::batchLoading() const noexcept
{
    return batchLoading_;
}

bool SceneSession::hasActiveVectorLoads() const noexcept
{
    return importServices_.vector->hasActiveJobs();
}

bool SceneSession::hasActiveRasterLoads() const noexcept
{
    return importServices_.raster->hasActiveJobs();
}

bool SceneSession::hasActiveColorizeJobs() const noexcept
{
    return importServices_.colorize->hasActiveJobs();
}

bool SceneSession::hasActiveColorizeJob(
    const PointCloudLayerId layerId) const noexcept
{
    return importServices_.colorize->hasActiveJob(layerId);
}

bool SceneSession::colorizeCommitInProgress(
    const PointCloudLayerId layerId) const noexcept
{
    return importServices_.colorize->isCommitInProgress(layerId);
}

PointCloudColorizeControllerMetrics
SceneSession::colorizeMetrics() const noexcept
{
    return importServices_.colorize->metrics();
}

const std::shared_ptr<const SpatialReferenceComparator> &
SceneSession::spatialReferenceComparator() const noexcept
{
    return importServices_.spatialReferences;
}

SceneSessionTimings SceneSession::timings() const noexcept
{
    return timings_;
}

std::uint64_t SceneSession::maximumLoadPoints() const noexcept
{
    return maximumLoadPoints_;
}

std::uint64_t SceneSession::decodedByteBudget() const noexcept
{
    return decodedByteBudget_;
}

bool SceneSession::automaticMemoryBudgetEnabled() const noexcept
{
    return automaticMemoryBudget_.has_value();
}

void SceneSession::setMaximumLoadPoints(const std::uint64_t maximumPoints)
{
    if (maximumPoints == 0) {
        throw std::invalid_argument("maximum load points must be positive");
    }
    maximumLoadPoints_ = maximumPoints;
}

bool SceneSession::setDecodedByteBudget(
    std::uint64_t byteBudget,
    std::optional<AutomaticMemoryBudgetParameters> automaticParameters)
{
    if (byteBudget == 0) {
        throw std::invalid_argument("decoded byte budget must be positive");
    }
    if (automaticParameters) {
        automaticParameters->currentPointBytes =
            document_->decodedResidentBytes();
        const AutomaticMemoryBudget recommendation =
            automaticMemoryBudget(systemMemoryInfo(), *automaticParameters);
        if (!recommendation.usedFallback) {
            byteBudget = recommendation.pointByteBudget;
        }
    }
    const std::uint64_t requestedByteBudget = byteBudget;
    byteBudget = std::max(requestedByteBudget, memoryBudget_->reservedBytes());
    if (!memoryBudget_->setByteBudget(byteBudget)) {
        return false;
    }
    automaticMemoryBudget_ = automaticParameters;
    decodedByteBudget_ = byteBudget;
    document_->syncResidencyBudgets();
    publishDocument();
    return byteBudget == requestedByteBudget;
}

void SceneSession::connectController()
{
    connect(loadController_,
            &PointCloudLoadController::progressChanged,
            this,
            &SceneSession::showLoadProgress);
    connect(loadController_,
            &PointCloudLoadController::sceneReady,
            this,
            &SceneSession::handleSceneReady);
    connect(loadController_,
            &PointCloudLoadController::loaded,
            this,
            &SceneSession::handleLoadCompleted);
    connect(loadController_,
            &PointCloudLoadController::failed,
            this,
            &SceneSession::showLoadFailure);
    connect(loadController_,
            &PointCloudLoadController::cancelled,
            this,
            &SceneSession::handleLoadCancelled);
    connect(loadController_,
            &PointCloudLoadController::schedulingChanged,
            this,
            [this] {
                assertOwnerThread(*this);
                document_->syncResidencyBudgets();
                publishDocument();
                if (batchLoading_) {
                    updateBatchProgress();
                }
                publishTaskRows();
            });
    connect(loadController_,
            &PointCloudLoadController::jobStateChanged,
            this,
            [this](const LoadJobId) {
                assertOwnerThread(*this);
                publishTaskRows();
                if (batchLoading_) {
                    updateBatchProgress();
                }
            });
}

void SceneSession::connectVectorController()
{
    VectorLoadController &controller = vectorLoadController();
    connect(&controller,
            &VectorLoadController::sublayerLoaded,
            this,
            [this](const LoadJobId,
                   const VectorSublayerKey &,
                   VectorLayerDataPtr data) {
                assertOwnerThread(*this);
                const bool visible = !data->extentDisjointXY;
                static_cast<void>(
                    document_->addVectorLayer(std::move(data), visible));
                publishDocument();
            });
    connect(&controller,
            &VectorLoadController::sublayerFailed,
            this,
            [this](const LoadJobId,
                   const VectorSublayerKey &,
                   const QString &message) {
                assertOwnerThread(*this);
                emit statusChanged(
                    QStringLiteral("Vector sublayer failed: %1").arg(message));
            });
    connect(&controller,
            &VectorLoadController::failed,
            this,
            [this](const LoadJobId jobId, const QString &message) {
                assertOwnerThread(*this);
                preselectedVectorSublayers_.erase(jobId);
                emit statusChanged(
                    QStringLiteral("Vector import failed: %1").arg(message));
                publishTaskRows();
                emit vectorJobFinished(jobId);
            });
    connect(&controller,
            &VectorLoadController::finished,
            this,
            [this](const LoadJobId jobId, const VectorLoadSummary &) {
                assertOwnerThread(*this);
                preselectedVectorSublayers_.erase(jobId);
                publishTaskRows();
                emit vectorJobFinished(jobId);
            });
    connect(&controller,
            &VectorLoadController::cancelled,
            this,
            [this](const LoadJobId jobId, const VectorLoadSummary &) {
                assertOwnerThread(*this);
                preselectedVectorSublayers_.erase(jobId);
                publishTaskRows();
                emit vectorJobFinished(jobId);
            });
    connect(
        &controller,
        &VectorLoadController::inspected,
        this,
        [this](const LoadJobId jobId, const VectorImportPreflight &preflight) {
            assertOwnerThread(*this);
            if (auto selected = preselectedVectorSublayers_.find(jobId);
                selected != preselectedVectorSublayers_.end()) {
                std::vector<VectorSublayerKey> sublayers =
                    std::move(selected->second);
                preselectedVectorSublayers_.erase(selected);
                static_cast<void>(
                    continueVectorImport(jobId, std::move(sublayers)));
                return;
            }
            if (preflight.sublayers.size() == 1) {
                static_cast<void>(continueVectorImport(
                    jobId, {preflight.sublayers.front().key}));
                return;
            }
            emit vectorSelectionRequired(jobId, preflight);
        });
    connect(&controller,
            &VectorLoadController::jobStateChanged,
            this,
            [this](const LoadJobId jobId) {
                assertOwnerThread(*this);
                publishTaskRows();
                static_cast<void>(jobId);
            });
}

void SceneSession::connectRasterController()
{
    RasterLoadController &controller = rasterLoadController();
    connect(
        &controller,
        &RasterLoadController::loaded,
        this,
        [this](const LoadJobId,
               RasterLayerDataPtr data,
               const bool initiallyVisible) {
            assertOwnerThread(*this);
            const bool wasEmpty = !document_->hasAnyLayer();
            try {
                static_cast<void>(document_->addRasterLayer(std::move(data),
                                                            initiallyVisible));
            } catch (const std::exception &error) {
                emit statusChanged(QStringLiteral("Raster layer rejected: %1")
                                       .arg(QString::fromUtf8(error.what())));
                return;
            }
            // Only the first layer in an empty document frames the view;
            // an additive import must not move the camera.
            publishDocument(wasEmpty, false);
        });
    connect(&controller,
            &RasterLoadController::failed,
            this,
            [this](const LoadJobId, const QString &message) {
                assertOwnerThread(*this);
                emit statusChanged(
                    QStringLiteral("Raster import failed: %1").arg(message));
                publishTaskRows();
            });
    connect(&controller,
            &RasterLoadController::cancelled,
            this,
            [this](const LoadJobId) {
                assertOwnerThread(*this);
                publishTaskRows();
            });
    connect(&controller,
            &RasterLoadController::jobStateChanged,
            this,
            [this](const LoadJobId) {
                assertOwnerThread(*this);
                publishTaskRows();
            });
}

void SceneSession::connectColorizeController()
{
    PointCloudColorizeController &controller = colorizeController();
    connect(&controller,
            &PointCloudColorizeController::prepared,
            this,
            [this](const LoadJobId jobId,
                   const RasterColorizeCommitToken token,
                   RasterColorizePreparedPtr prepared,
                   RasterPointColorBinding binding) {
                assertOwnerThread(*this);
                const auto point = document_->layer(token.pointLayerId);
                const auto raster = document_->rasterLayer(token.rasterLayerId);
                if (!point || !raster || point->scene != token.scene ||
                    point->colorGeneration != token.pointColorGeneration ||
                    raster->data->sourceId != token.rasterSourceId ||
                    raster->renderGeneration != token.rasterRenderGeneration) {
                    colorizeController().finishCommit(
                        jobId,
                        false,
                        QStringLiteral("Layer state changed while colors were "
                                       "being prepared; try again"));
                    document_->syncResidencyBudgets();
                    publishTaskRows();
                    return;
                }
                RasterPointColorApplyOutcome outcome;
                try {
                    outcome = point->scene->applyRasterPointColors(
                        std::move(prepared));
                } catch (const std::exception &error) {
                    colorizeController().finishCommit(
                        jobId, false, QString::fromUtf8(error.what()));
                    document_->syncResidencyBudgets();
                    publishTaskRows();
                    return;
                }
                if (outcome != RasterPointColorApplyOutcome::Applied) {
                    colorizeController().finishCommit(jobId, false);
                    document_->syncResidencyBudgets();
                    publishTaskRows();
                    return;
                }
                if (!document_->setLayerRasterColors(token.pointLayerId,
                                                     std::move(binding))) {
                    throw std::logic_error("Applied point colors could not be "
                                           "recorded in the document");
                }
                static_cast<void>(document_->setLayerColorMode(
                    token.pointLayerId,
                    PointColorMode{.source = PointColorSource::Rgb,
                                   .colorMap = PointColorMap::Rgb,
                                   .manualRange = std::nullopt}));
                colorizeController().finishCommit(jobId, true);
                document_->syncResidencyBudgets();
                publishDocument();
                publishTaskRows();
                const auto completed = colorizeController().jobState(jobId);
                const QString targetName =
                    completed && !completed->pointLayerName.isEmpty()
                        ? completed->pointLayerName
                        : pointLayerName(*point);
                const QString sourceName =
                    completed && !completed->rasterLayerName.isEmpty()
                        ? completed->rasterLayerName
                        : rasterLayerName(*raster);
                emit statusChanged(
                    QStringLiteral("Colorized %1 from %2. %3")
                        .arg(targetName,
                             sourceName,
                             completed ? completed->detail : QString{}));
            });
    connect(&controller,
            &PointCloudColorizeController::failed,
            this,
            [this](const LoadJobId, const QString &message) {
                document_->syncResidencyBudgets();
                emit statusChanged(
                    QStringLiteral("Point-cloud colorization failed: %1")
                        .arg(message));
                publishTaskRows();
            });
    connect(
        &controller,
        &PointCloudColorizeController::cancelled,
        this,
        [this](const LoadJobId jobId) {
            document_->syncResidencyBudgets();
            const auto state = colorizeController().jobState(jobId);
            emit statusChanged(
                state && !state->pointLayerName.isEmpty()
                    ? QStringLiteral("Colorization of %1 cancelled.")
                          .arg(state->pointLayerName)
                    : QStringLiteral("Point-cloud colorization cancelled."));
            publishTaskRows();
        });
    connect(&controller,
            &PointCloudColorizeController::jobStateChanged,
            this,
            [this](const LoadJobId) {
                publishTaskRows();
            });
}

void SceneSession::publishTaskRows()
{
    LoadJobRows rows = pointLoadController().jobRows();
    LoadJobRows vectorRows = vectorLoadController().jobRows();
    rows.insert(rows.end(),
                std::make_move_iterator(vectorRows.begin()),
                std::make_move_iterator(vectorRows.end()));
    LoadJobRows rasterRows = rasterLoadController().jobRows();
    rows.insert(rows.end(),
                std::make_move_iterator(rasterRows.begin()),
                std::make_move_iterator(rasterRows.end()));
    LoadJobRows colorizeRows = colorizeController().jobRows();
    rows.insert(rows.end(),
                std::make_move_iterator(colorizeRows.begin()),
                std::make_move_iterator(colorizeRows.end()));
    emit taskRowsChanged(std::move(rows));
}

void SceneSession::loadPointCloud(const std::filesystem::path &sourcePath,
                                  const PointCloudLoadMode mode)
{
    assertOwnerThread(*this);
    if (loading_) {
        return;
    }
    refreshAutomaticMemoryBudget();
    beginLoad(sourcePath,
              !document_->hasPointCloudLayers() ? PointCloudLoadMode::Replace
                                                : mode);
}

void SceneSession::loadPointClouds(
    std::vector<std::filesystem::path> sourcePaths,
    const PointCloudLoadMode firstMode)
{
    assertOwnerThread(*this);
    if (sourcePaths.empty() || loading_) {
        return;
    }
    refreshAutomaticMemoryBudget();
    if (sourcePaths.size() == 1) {
        beginLoad(sourcePaths.front(), firstMode);
        return;
    }
    const bool replace = !document_->hasPointCloudLayers() ||
                         firstMode == PointCloudLoadMode::Replace;
    if (replace && document_->hasAnyLayer()) {
        auto replacement = std::make_shared<SceneDocument>(
            decodedByteBudget_,
            HierarchyResidencyCoordinator::defaultMaximumConcurrentDecodes,
            decodeAdmission_,
            memoryBudget_,
            document_->colorMaps());
        static_cast<void>(replacement->copyOverlayLayersFrom(*document_));
        document_ = std::move(replacement);
        emit documentChanged(document_->snapshot(), false, false);
    }

    loading_ = true;
    batchLoading_ = true;
    batchReplacing_ = replace;
    batchTotal_ = sourcePaths.size();
    batchProgressPercentage_ = 0;
    batchProgressUpdatesEnabled_ = false;
    batchLoadedPoints_ = 0;
    batchSafetySamplesAtStart_ =
        loadController_->metrics().safetySampledSources;
    batchTimeToFirstPointsMilliseconds_.reset();
    batchTimeToAllFirstPointsMilliseconds_.reset();
    batchTimeToAllDisplayReadyMilliseconds_.reset();
    timings_ = {};
    batchBaseLayerCount_ = document_->layerCount();
    batchOrder_.clear();
    batchOrder_.reserve(sourcePaths.size());
    progressTimer_.stop();
    emit loadingChanged(
        true,
        replace,
        QStringLiteral("%1 point clouds").arg(sourcePaths.size()));
    if (!replace) {
        emit showTasksRequested();
    }
    emit batchProgressChanged(
        0, QStringLiteral("Queued %1 point clouds").arg(sourcePaths.size()));

    std::vector<PointCloudLoadRequest> requests;
    requests.reserve(sourcePaths.size());
    for (const std::filesystem::path &sourcePath : sourcePaths) {
        requests.push_back({
            .options =
                {
                    .sourcePath = sourcePath,
                    .maximumPoints = maximumLoadPoints_,
                    .localPaging =
                        {
                            .cacheDirectory = localPageCacheDirectory_,
                        },
                },
            .resources =
                {
                    .decodedByteBudget = decodedByteBudget_,
                    .residency = document_->residencyCoordinator(),
                    .memoryBudget = memoryBudget_,
                    .flatReservation = {},
                },
        });
    }
    const auto dispatchTime = std::chrono::steady_clock::now();
    batchOrder_ = loadController_->loadBatch(std::move(requests));
    for (std::size_t index = 0; index < sourcePaths.size(); ++index) {
        trackLoadJob(batchOrder_[index],
                     sourcePaths[index],
                     PointCloudLoadMode::Add,
                     index,
                     dispatchTime);
    }
    QTimer::singleShot(50, this, [this] {
        if (batchLoading_) {
            batchProgressUpdatesEnabled_ = true;
            updateBatchProgress();
        }
    });
    emit statusChanged(
        QStringLiteral("Loading %1 point clouds…").arg(sourcePaths.size()));
}

void SceneSession::beginLoad(const std::filesystem::path &sourcePath,
                             const PointCloudLoadMode mode)
{
    loading_ = true;
    batchLoading_ = false;
    batchReplacing_ = false;
    timings_ = {};
    loadingProgress_.reset();
    progressTimer_.stop();
    const bool showOverlay = mode == PointCloudLoadMode::Replace ||
                             !document_->hasPointCloudLayers();
    emit loadingChanged(true, showOverlay, pathToQString(sourcePath));
    if (!showOverlay) {
        emit showTasksRequested();
    }
    emit loadingProgressChanged(loadingProgress_.state());
    startLoadJob(sourcePath, mode, 0);
    emit statusChanged(
        QStringLiteral("Loading %1…").arg(displayPathName(sourcePath)));
}

LoadJobId SceneSession::startLoadJob(const std::filesystem::path &sourcePath,
                                     const PointCloudLoadMode mode,
                                     const std::size_t order)
{
    const auto dispatchTime = std::chrono::steady_clock::now();
    const LoadJobId jobId = loadController_->load({
        .options =
            {
                .sourcePath = sourcePath,
                .maximumPoints = maximumLoadPoints_,
                .localPaging =
                    {
                        .cacheDirectory = localPageCacheDirectory_,
                    },
            },
        .resources =
            {
                .decodedByteBudget = decodedByteBudget_,
                .residency = document_->residencyCoordinator(),
                .memoryBudget = memoryBudget_,
                .flatReservation = {},
            },
    });
    trackLoadJob(jobId, sourcePath, mode, order, dispatchTime);
    return jobId;
}

void SceneSession::trackLoadJob(
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
                             .layerId = std::nullopt,
                             .admitted = false,
                             .importCompleted = false,
                             .firstFrameCompleted = false,
                             .displayCompleted = false,
                             .fullDetailWarming = false,
                             .fullDetailDecoded = 0,
                             .fullDetailUploaded = 0,
                             .fullDetailTotal = 0,
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

void SceneSession::cancelAll()
{
    assertOwnerThread(*this);
    loadController_->cancel();
}

void SceneSession::cancelJob(const LoadJobId jobId)
{
    assertOwnerThread(*this);
    if (const ActiveLoad *load = activeLoad(jobId); load && load->layerId) {
        static_cast<void>(document_->setLayerVisible(*load->layerId, false));
        publishDocument();
    }
    loadController_->cancel(jobId);
}

void SceneSession::retryJob(const LoadJobId jobId)
{
    assertOwnerThread(*this);
    const auto state = loadController_->jobState(jobId);
    if (!state || !state->canRetry || loading_) {
        return;
    }
    const std::filesystem::path sourcePath = state->sourcePath;
    static_cast<void>(loadController_->dismiss(jobId));
    loadPointCloud(sourcePath,
                   !document_->hasPointCloudLayers()
                       ? PointCloudLoadMode::Replace
                       : PointCloudLoadMode::Add);
}

void SceneSession::prioritizeJob(const LoadJobId jobId)
{
    assertOwnerThread(*this);
    if (loadController_->prioritize(jobId)) {
        emit statusChanged(QStringLiteral("Point-cloud source prioritized."));
    }
}

void SceneSession::dismissJob(const LoadJobId jobId)
{
    assertOwnerThread(*this);
    if (loadController_->dismiss(jobId)) {
        publishTaskRows();
    }
}

LoadJobId SceneSession::loadVectorLayers(VectorImportRequest request)
{
    assertOwnerThread(*this);
    std::vector<VectorSublayerKey> selected = request.sublayers;
    if (selected.empty()) {
        throw std::invalid_argument(
            "a programmatic vector load requires selected sublayers");
    }
    const LoadJobId jobId = startVectorImport(std::move(request));
    preselectedVectorSublayers_.emplace(jobId, std::move(selected));
    return jobId;
}

LoadJobId SceneSession::startVectorImport(VectorImportRequest request)
{
    assertOwnerThread(*this);
    return vectorLoadController().startInspection(std::move(request));
}

bool SceneSession::continueVectorImport(const LoadJobId jobId,
                                        std::vector<VectorSublayerKey> selected)
{
    assertOwnerThread(*this);
    return vectorLoadController().continueLoad(jobId, std::move(selected));
}

LoadJobId SceneSession::startRasterImport(RasterImportRequest request)
{
    assertOwnerThread(*this);
    return rasterLoadController().startImport(std::move(request));
}

LoadJobId
SceneSession::colorizePointCloudFromRaster(const PointCloudLayerId pointLayerId,
                                           const SceneLayerId rasterLayerId,
                                           RasterColorizeOptions options)
{
    assertOwnerThread(*this);
    const auto point = document_->layer(pointLayerId);
    const auto raster = document_->rasterLayer(rasterLayerId);
    if (!point || !raster) {
        throw std::invalid_argument(
            "Colorization requires an attached point cloud and raster");
    }
    if (options.temporaryDirectory.empty()) {
        options.temporaryDirectory = std::filesystem::temp_directory_path();
    }
    std::optional<SpatialReferenceRelation> relation;
    if (importServices_.spatialReferences) {
        relation = importServices_.spatialReferences->compare(
            point->scene->metadata().spatialReferenceWkt,
            raster->data->metadata().spatialReferenceWkt);
    }
    const auto decode =
        resolveRasterDecodeParameters(*raster, *document_->colorMaps());
    const QString targetName = pointLayerName(*point);
    const QString sourceName = rasterLayerName(*raster);
    LoadJobId jobId;
    try {
        jobId = colorizeController().startColorize(
            {
                .token =
                    RasterColorizeCommitToken{
                        .pointLayerId = pointLayerId,
                        .scene = point->scene,
                        .pointColorGeneration = point->colorGeneration,
                        .rasterLayerId = rasterLayerId,
                        .rasterSourceId = raster->data->sourceId,
                        .rasterRenderGeneration = raster->renderGeneration,
                    },
                .raster = raster->data->source,
                .decode = decode,
                .options = std::move(options),
                .memoryBudget = memoryBudget_,
                .crsRelation = relation,
                .pointLayerName = targetName,
                .rasterLayerName = sourceName,
            },
            [this] {
                document_->syncResidencyBudgets();
            });
    } catch (...) {
        document_->syncResidencyBudgets();
        throw;
    }
    publishTaskRows();
    emit showTasksRequested();
    emit statusChanged(
        QStringLiteral("Colorizing %1 from %2…").arg(targetName, sourceName));
    return jobId;
}

bool SceneSession::revertPointCloudColors(const PointCloudLayerId pointLayerId)
{
    assertOwnerThread(*this);
    const auto point = document_->layer(pointLayerId);
    if (!point || !point->rasterColors) {
        return false;
    }
    const QString targetName = pointLayerName(*point);
    colorizeController().cancelAndWaitForPointLayer(pointLayerId);
    if (point->scene->revertPointColors() !=
        RasterPointColorApplyOutcome::Applied) {
        return false;
    }
    static_cast<void>(document_->clearLayerRasterColors(pointLayerId));
    if (!point->scene->metadata().hasColor &&
        point->colorMode.source == PointColorSource::Rgb) {
        static_cast<void>(document_->setLayerColorMode(
            pointLayerId,
            defaultPointColorMode(*document_->colorMaps(),
                                  point->scene->metadata())));
    }
    document_->syncResidencyBudgets();
    publishDocument();
    publishTaskRows();
    emit statusChanged(
        QStringLiteral("Restored source colors for %1.").arg(targetName));
    return true;
}

void SceneSession::cancelAllLoads()
{
    assertOwnerThread(*this);
    cancelAll();
    vectorLoadController().cancelAll();
    rasterLoadController().cancelAll();
    colorizeController().cancelAll();
}

// Each keyed operation below names every job kind. Treating "not a point
// cloud" as "therefore a vector" would silently route raster jobs to the wrong
// controller, where they would be ignored rather than reported.
void SceneSession::cancelJob(const LoadJobKey key)
{
    assertOwnerThread(*this);
    switch (key.kind) {
    case LoadJobKind::PointCloud:
        cancelJob(key.id);
        break;
    case LoadJobKind::Vector:
        vectorLoadController().cancel(key.id);
        break;
    case LoadJobKind::Raster:
        rasterLoadController().cancel(key.id);
        break;
    case LoadJobKind::Colorize:
        colorizeController().cancel(key.id);
        break;
    }
}

void SceneSession::retryJob(const LoadJobKey key)
{
    assertOwnerThread(*this);
    switch (key.kind) {
    case LoadJobKind::PointCloud:
        retryJob(key.id);
        break;
    case LoadJobKind::Vector:
        static_cast<void>(vectorLoadController().retry(key.id));
        break;
    case LoadJobKind::Raster:
        static_cast<void>(rasterLoadController().retry(key.id));
        break;
    case LoadJobKind::Colorize:
        if (!colorizeController().retry(key.id)) {
            document_->syncResidencyBudgets();
        }
        break;
    }
}

void SceneSession::prioritizeJob(const LoadJobKey key)
{
    assertOwnerThread(*this);
    // Only point-cloud jobs are reprioritizable; overlay imports are short and
    // already run at inspection priority.
    if (key.kind == LoadJobKind::PointCloud) {
        prioritizeJob(key.id);
    }
}

void SceneSession::dismissJob(const LoadJobKey key)
{
    assertOwnerThread(*this);
    switch (key.kind) {
    case LoadJobKind::PointCloud:
        dismissJob(key.id);
        break;
    case LoadJobKind::Vector:
        if (vectorLoadController().dismiss(key.id)) {
            publishTaskRows();
        }
        break;
    case LoadJobKind::Raster:
        if (rasterLoadController().dismiss(key.id)) {
            publishTaskRows();
        }
        break;
    case LoadJobKind::Colorize:
        if (colorizeController().dismiss(key.id)) {
            publishTaskRows();
        }
        break;
    }
}

void SceneSession::setLayerVisible(const SceneLayerId layerId,
                                   const bool visible)
{
    assertOwnerThread(*this);
    if (document_->setLayerVisible(layerId, visible)) {
        publishDocument();
    }
}

void SceneSession::setVectorLayerStyle(const SceneLayerId layerId,
                                       VectorLayerStyle style)
{
    assertOwnerThread(*this);
    if (document_->setVectorLayerStyle(layerId, style)) {
        publishDocument();
    }
}

void SceneSession::setRasterLayerStyle(const SceneLayerId layerId,
                                       RasterLayerStyle style)
{
    assertOwnerThread(*this);
    if (document_->setRasterLayerStyle(layerId, std::move(style))) {
        publishDocument();
    }
}

void SceneSession::setLayerColorMode(const PointCloudLayerId layerId,
                                     const PointColorMode mode)
{
    assertOwnerThread(*this);
    if (document_->setLayerColorMode(layerId, mode)) {
        publishDocument();
    }
}

void SceneSession::setAllLayerColors(PointColorMode mode)
{
    assertOwnerThread(*this);
    mode.manualRange.reset();
    const std::vector<PointCloudLayer> layers = document_->layers();
    if (layers.empty() ||
        !std::ranges::all_of(
            layers, [this, &mode](const PointCloudLayer &layer) {
                return pointColorModeAvailable(*document_->colorMaps(),
                                               layer.scene->metadata(),
                                               mode,
                                               layer.rasterColors.has_value());
            })) {
        return;
    }

    bool changed = false;
    for (const PointCloudLayer &layer : layers) {
        PointColorMode updated = layer.colorMode;
        if (updated.source != mode.source) {
            updated.manualRange.reset();
        }
        updated.source = mode.source;
        updated.colorMap = mode.colorMap;
        if (updated != layer.colorMode) {
            changed =
                document_->setLayerColorMode(layer.id, updated) || changed;
        }
    }
    if (changed) {
        publishDocument();
    }
}

void SceneSession::setLayerClassificationFilter(
    const PointCloudLayerId layerId,
    const PointClassificationFilter filter,
    const bool applyToAllLayers)
{
    assertOwnerThread(*this);
    bool changed = false;
    if (applyToAllLayers) {
        for (const PointCloudLayer &layer : document_->layers()) {
            if (!layer.scene->metadata().hasClassification ||
                layer.classificationFilter == filter) {
                continue;
            }
            changed =
                document_->setLayerClassificationFilter(layer.id, filter) ||
                changed;
        }
    } else if (const auto layer = document_->layer(layerId);
               layer && layer->classificationFilter != filter) {
        changed = document_->setLayerClassificationFilter(layerId, filter);
    }
    if (changed) {
        publishDocument();
    }
}

void SceneSession::removeLayer(const SceneLayerId layerId)
{
    assertOwnerThread(*this);
    if (document_->layer(layerId)) {
        colorizeController().cancelForPointLayer(layerId);
    }
    if (document_->rasterLayer(layerId)) {
        colorizeController().cancelForRasterLayer(layerId);
    }
    if (ActiveLoad *load = activeLoadByLayerId(layerId)) {
        load->admitted = false;
        load->layerId.reset();
        loadController_->cancel(load->jobId);
    }
    if (document_->removeLayer(layerId)) {
        publishDocument();
    }
}

void SceneSession::isolateLayer(const SceneLayerId layerId)
{
    assertOwnerThread(*this);
    if (document_->isolateLayer(layerId)) {
        publishDocument();
    }
}

void SceneSession::showAllLayers()
{
    assertOwnerThread(*this);
    if (document_->setAllLayersVisible(true)) {
        publishDocument();
    }
}

void SceneSession::showLoadProgress(const LoadJobId jobId,
                                    const PointCloudImportStage stage,
                                    const std::uint64_t processed,
                                    const std::uint64_t total)
{
    assertOwnerThread(*this);
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
    emit loadingProgressChanged(state);
    if (state.estimated && state.phase != previous) {
        progressStageTimer_.restart();
        progressTimer_.start();
    } else if (!state.estimated) {
        progressTimer_.stop();
    }
    emit statusChanged(QStringLiteral("Loading %1…").arg(load->sourceName));
}

void SceneSession::onRenderLoadProgress(const RenderLoadProgress &progress)
{
    assertOwnerThread(*this);
    ActiveLoad *load = activeLoadByLayerId(progress.layerId);
    if (!loading_ || !load) {
        return;
    }
    if (!batchLoading_) {
        const LoadingProgressPhase previous = loadingProgress_.state().phase;
        const LoadingProgressState state =
            loadingProgress_.updateRender(progress);
        emit loadingProgressChanged(state);
        if (state.estimated && state.phase != previous) {
            progressStageTimer_.restart();
            progressTimer_.start();
        } else if (!state.estimated) {
            progressTimer_.stop();
        }
    }
    if (progress.stage == RenderLoadStage::FullDetailWarming) {
        load->fullDetailWarming = true;
        load->fullDetailDecoded = progress.decoded;
        load->fullDetailUploaded = progress.uploaded;
        load->fullDetailTotal = progress.total;
    } else if (progress.stage == RenderLoadStage::Uploading) {
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

void SceneSession::handleSceneReady(const LoadJobId jobId,
                                    const PointCloudScenePtr &scene)
{
    assertOwnerThread(*this);
    ActiveLoad *load = activeLoad(jobId);
    if (!load) {
        return;
    }
    try {
        load->scene = scene;
        if (batchLoading_) {
            admitBatchLayers();
        } else {
            loadingProgress_.setPagedSource(scene && scene->hierarchical());
            handleSceneReadySingle(*load);
        }
    } catch (const std::exception &error) {
        showLoadFailure(jobId, QString::fromUtf8(error.what()));
    }
}

void SceneSession::handleSceneReadySingle(ActiveLoad &load)
{
    if (load.mode == PointCloudLoadMode::Add) {
        load.layerId = document_->addLayer(load.scene);
        load.admitted = true;
        publishDocument();
    } else if (!document_->hasAnyLayer()) {
        load.layerId = document_->addLayer(load.scene);
        load.admitted = true;
        publishDocument(true, true);
    } else {
        load.previousDocument = document_;
        auto replacement = std::make_shared<SceneDocument>(
            decodedByteBudget_,
            HierarchyResidencyCoordinator::defaultMaximumConcurrentDecodes,
            decodeAdmission_,
            memoryBudget_,
            document_->colorMaps());
        static_cast<void>(replacement->copyOverlayLayersFrom(*document_));
        load.layerId = replacement->addLayer(load.scene);
        load.admitted = true;
        document_ = std::move(replacement);
        publishDocument(true, true);
    }
}

void SceneSession::admitBatchLayers()
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
        const bool hadNoPointClouds = !document_->hasPointCloudLayers();
        load->layerId = document_->insertLayer(
            load->scene, batchBaseLayerCount_ + earlierAdmissions);
        load->admitted = true;
        if (hadNoPointClouds) {
            publishDocument(true, true);
        } else if (batchReplacing_) {
            emit frameVisibleLayersRequested();
        }
    }
    publishDocument();
    updateBatchProgress();
}

void SceneSession::handleLoadCompleted(const LoadJobId jobId,
                                       const PointCloudScenePtr &scene)
{
    assertOwnerThread(*this);
    ActiveLoad *load = activeLoad(jobId);
    if (!load) {
        return;
    }
    load->scene = scene;
    if (batchLoading_) {
        admitBatchLayers();
    } else {
        handleLoadCompletedSingle(*load, scene);
    }
    load->importCompleted = true;
    if (!batchLoading_) {
        emit loadingProgressChanged(loadingProgress_.completeImport());
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

void SceneSession::handleLoadCompletedSingle(ActiveLoad &load,
                                             const PointCloudScenePtr &scene)
{
    if (!load.admitted) {
        load.scene = scene;
        handleSceneReadySingle(load);
    }
}

void SceneSession::markFirstFrameReady(ActiveLoad &load)
{
    if (load.firstFrameCompleted) {
        return;
    }
    load.firstFrameCompleted = true;
    if (load.scene && load.scene->totalPointCount() > 0) {
        load.timeToFirstPointsMilliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - load.dispatchTime)
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

void SceneSession::markDisplayCompleted(ActiveLoad &load)
{
    if (load.displayCompleted) {
        return;
    }
    load.displayCompleted = true;
    load.displayReadyMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - load.dispatchTime)
            .count();
    if (batchLoading_ && (!batchTimeToAllDisplayReadyMilliseconds_ ||
                          *load.displayReadyMilliseconds >
                              *batchTimeToAllDisplayReadyMilliseconds_)) {
        batchTimeToAllDisplayReadyMilliseconds_ = load.displayReadyMilliseconds;
    }
    finishJobIfComplete(load);
}

void SceneSession::finishJobIfComplete(ActiveLoad &load)
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

void SceneSession::finalizeLoads(const QString &status)
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
    emit loadingChanged(false, false, {});
    if (!status.isEmpty()) {
        emit statusChanged(status);
    }
    publishTaskRows();
}

void SceneSession::handleLoadCancelled(const LoadJobId jobId)
{
    assertOwnerThread(*this);
    ActiveLoad *load = activeLoad(jobId);
    if (load) {
        rollbackAdmittedLoad(*load);
        activeLoads_.erase(jobId);
    }
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

void SceneSession::rollbackAdmittedLoad(ActiveLoad &load)
{
    if (!load.admitted) {
        return;
    }
    if (load.previousDocument) {
        document_ = std::move(load.previousDocument);
        publishDocument(true, true);
    } else if (load.layerId) {
        static_cast<void>(document_->removeLayer(*load.layerId));
        publishDocument();
    }
    load.layerId.reset();
    load.admitted = false;
}

void SceneSession::showLoadFailure(const LoadJobId jobId,
                                   const QString &message)
{
    assertOwnerThread(*this);
    ActiveLoad *load = activeLoad(jobId);
    if (batchLoading_) {
        if (load) {
            rollbackAdmittedLoad(*load);
            activeLoads_.erase(jobId);
        }
        admitBatchLayers();
        emit statusChanged(
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
    if (load) {
        rollbackAdmittedLoad(*load);
    }
    activeLoads_.clear();
    progressTimer_.stop();
    loading_ = false;
    emit loadingChanged(false, false, {});
    emit statusChanged(QStringLiteral("Loading failed: %1").arg(message));
    emit failureOccurred(QStringLiteral("Point cloud loading failed"), message);
    publishTaskRows();
}

void SceneSession::updateBatchProgress()
{
    if (!batchLoading_ || batchTotal_ == 0 || !batchProgressUpdatesEnabled_) {
        return;
    }
    std::vector<PointCloudLoadJobState> batchStates;
    batchStates.reserve(batchOrder_.size());
    for (const LoadJobId jobId : batchOrder_) {
        if (const auto state = loadController_->jobState(jobId)) {
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
        const auto state = loadController_->jobState(jobId);
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
            completion =
                std::max(completion,
                         paged ? pagedPreparation + (0.99L - pagedPreparation) *
                                                        0.5L * fraction
                               : 0.75L + 0.24L * fraction);
        }
        if (!paged && load->importCompleted && load->firstFrameCompleted) {
            completion = std::max(completion, 0.80L);
        }
        if (load->fullDetailWarming && load->fullDetailTotal > 0) {
            const long double fraction =
                paged ? std::clamp((static_cast<long double>(
                                        std::min(load->fullDetailDecoded,
                                                 load->fullDetailTotal)) +
                                    static_cast<long double>(
                                        std::min(load->fullDetailUploaded,
                                                 load->fullDetailTotal))) /
                                       (2.0L * static_cast<long double>(
                                                   load->fullDetailTotal)),
                                   0.0L,
                                   1.0L)
                      : std::clamp(
                            static_cast<long double>(
                                std::min(load->fullDetailDecoded,
                                         load->fullDetailUploaded)) /
                                static_cast<long double>(load->fullDetailTotal),
                            0.0L,
                            1.0L);
            completion = std::max(
                completion,
                paged ? pagedPreparation + (0.99L - pagedPreparation) * fraction
                      : 0.75L + 0.24L * fraction);
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
    const PointCloudLoadControllerMetrics metrics = loadController_->metrics();
    const std::uint64_t sampled =
        metrics.safetySampledSources - batchSafetySamplesAtStart_;
    std::uint64_t fullDetailDecoded = 0;
    std::uint64_t fullDetailUploaded = 0;
    std::uint64_t fullDetailTotal = 0;
    for (const auto &[jobId, load] : activeLoads_) {
        static_cast<void>(jobId);
        if (!load.fullDetailWarming) {
            continue;
        }
        fullDetailDecoded =
            saturatingAdd(fullDetailDecoded, load.fullDetailDecoded);
        fullDetailUploaded =
            saturatingAdd(fullDetailUploaded, load.fullDetailUploaded);
        fullDetailTotal = saturatingAdd(fullDetailTotal, load.fullDetailTotal);
    }
    if (fullDetailTotal > 0) {
        emit batchProgressChanged(
            batchProgressPercentage_,
            QStringLiteral(
                "Preparing full detail: %1 / %2 decoded, %3 / %2 uploaded")
                .arg(fullDetailDecoded)
                .arg(fullDetailTotal)
                .arg(fullDetailUploaded));
        return;
    }
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
    emit batchProgressChanged(batchProgressPercentage_, details);
}

SceneSession::ActiveLoad *SceneSession::activeLoad(const LoadJobId jobId)
{
    const auto found = activeLoads_.find(jobId);
    return found == activeLoads_.end() ? nullptr : &found->second;
}

SceneSession::ActiveLoad *
SceneSession::activeLoadByLayerId(const PointCloudLayerId layerId)
{
    for (auto &[jobId, load] : activeLoads_) {
        static_cast<void>(jobId);
        if (load.layerId == layerId) {
            return &load;
        }
    }
    return nullptr;
}

void SceneSession::publishDocument(const bool frameVisibleLayers,
                                   const bool replaceRendererDocument)
{
    emit documentChanged(
        document_->snapshot(), frameVisibleLayers, replaceRendererDocument);
    publishTaskRows();
}

void SceneSession::refreshAutomaticMemoryBudget()
{
    assertOwnerThread(*this);
    if (!automaticMemoryBudget_ || !memoryBudget_) {
        return;
    }
    AutomaticMemoryBudgetParameters parameters = *automaticMemoryBudget_;
    parameters.currentPointBytes = document_->decodedResidentBytes();
    const AutomaticMemoryBudget recommendation =
        automaticMemoryBudget(systemMemoryInfo(), parameters);
    if (recommendation.usedFallback) {
        return;
    }
    const std::uint64_t target = std::max(recommendation.pointByteBudget,
                                          memoryBudget_->reservedBytes());
    if (target == memoryBudget_->byteBudget() ||
        !memoryBudget_->setByteBudget(target)) {
        return;
    }
    decodedByteBudget_ = target;
    document_->syncResidencyBudgets();
    publishDocument();
}

} // namespace pci
