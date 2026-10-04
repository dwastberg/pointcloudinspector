#include "SceneOperationBindings.h"
#include "ScenePublicationCoordinator.h"
#include <QMetaMethod>
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

[[nodiscard]] QString pointLayerName(const PointCloudLayer &layer)
{
    return layer.descriptor.metadata.sourcePath.empty()
               ? QStringLiteral("Point cloud %1").arg(layer.id.value())
               : displayPathName(layer.descriptor.metadata.sourcePath);
}

[[nodiscard]] QString rasterLayerName(const RasterLayer &layer)
{
    return layer.descriptor.metadata.sourcePath.empty()
               ? QStringLiteral("Raster layer %1").arg(layer.id.value())
               : displayPathName(layer.descriptor.metadata.sourcePath);
}

} // namespace

SceneSession::SceneSession(
    ImportServices importServices,
    const std::uint64_t maximumLoadPoints,
    const std::uint64_t decodedByteBudget,
    std::optional<AutomaticMemoryBudgetParameters> automaticMemoryBudget,
    LocalPageCacheContextPtr localPageCache,
    PointColorMapCatalogSnapshotPtr colorMaps,
    QObject *parent,
    SceneSessionClock clock)
    : QObject(parent)
    , clock_(clock ? std::move(clock) : [] {
        return std::chrono::steady_clock::now();
    })
    , importServices_(std::move(importServices))
    , decodeAdmission_(std::make_shared<HierarchyDecodeAdmission>())
    , memoryBudget_(std::make_shared<PointMemoryBudget>(decodedByteBudget))
    , document_(std::make_shared<SceneDocument>(std::move(colorMaps),
                                                documentGeneration_))
    , runtime_(
          decodedByteBudget,
          HierarchyResidencyCoordinator::defaultMaximumConcurrentDecodes,
          decodeAdmission_,
          memoryBudget_)
    , maximumLoadPoints_(maximumLoadPoints)
    , decodedByteBudget_(decodedByteBudget)
    , automaticMemoryBudget_(automaticMemoryBudget)
    , localPageCache_(std::move(localPageCache))
{
    qRegisterMetaType<SceneDocumentSnapshotPtr>();
    qRegisterMetaType<SceneRuntimeSnapshotPtr>();
    qRegisterMetaType<DocumentUpdate>();
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
    pointLoads_ = std::make_unique<PointSceneCoordinator>(*this);
    memoryBudgetRefreshTimer_.setInterval(5000);
    connect(&memoryBudgetRefreshTimer_,
            &QTimer::timeout,
            this,
            &SceneSession::refreshAutomaticMemoryBudget);
    memoryBudgetRefreshTimer_.start();
    const auto ids = importServices_.jobIds;
    pointLoadController().bindRegistry(
        operationRegistry_, ids, [this](LoadJobId id) {
            return OperationControls{[this, id] {
                                         cancelJob(id);
                                     },
                                     [this, id] {
                                         retryJob(id);
                                     },
                                     [this, id] {
                                         prioritizeJob(id);
                                     },
                                     [this, id] {
                                         dismissJob(id);
                                     }};
        });
    vectorLoadController().bindRegistry(
        operationRegistry_, ids, [this](LoadJobId id) {
            return OperationControls{
                [this, id] {
                    vectorLoadController().cancel(id);
                },
                [this, id] {
                    static_cast<void>(
                        vectorLoadController().retry(id, sessionGeneration_));
                },
                {},
                [this, id] {
                    static_cast<void>(vectorLoadController().dismiss(id));
                }};
        });
    rasterLoadController().bindRegistry(
        operationRegistry_, ids, [this](LoadJobId id) {
            return OperationControls{
                [this, id] {
                    rasterLoadController().cancel(id);
                },
                [this, id] {
                    static_cast<void>(
                        rasterLoadController().retry(id, sessionGeneration_));
                },
                {},
                [this, id] {
                    static_cast<void>(rasterLoadController().dismiss(id));
                }};
        });
    rasterElevationController().bindRegistry(
        operationRegistry_, ids, [this](LoadJobId id) {
            return OperationControls{
                [this, id] {
                    rasterElevationController().cancel(id);
                },
                [this, id] {
                    const auto states = rasterElevationController().jobStates();
                    const auto state = std::ranges::find(
                        states, id, &RasterElevationJobState::jobId);
                    if (state != states.end()) {
                        static_cast<void>(document_->setRasterElevationState(
                            state->layerId, RasterElevationStatus::Scanning));
                        publishDocument();
                    }
                    static_cast<void>(rasterElevationController().retry(id));
                },
                {},
                [this, id] {
                    static_cast<void>(rasterElevationController().dismiss(id));
                }};
        });
    colorizeController().bindRegistry(
        operationRegistry_, ids, [this](LoadJobId id) {
            return OperationControls{[this, id] {
                                         colorizeController().cancel(id);
                                     },
                                     [this, id] {
                                         if (!colorizeController().retry(id))
                                             runtime_.syncResidencyBudgets();
                                     },
                                     {},
                                     [this, id] {
                                         static_cast<void>(
                                             colorizeController().dismiss(id));
                                     }};
        });
    statistics_ = std::make_unique<StatisticsOperation>(
        importServices_.statistics,
        *importServices_.scheduler,
        makeQtCompletionExecutor(this),
        ids,
        operationRegistry_,
        [this](const StatisticsBindingToken &token) {
            if (token.session != sessionGeneration_)
                return false;
            const auto layer = document_->layer(token.layer);
            return layer && layer->descriptor.sourceId == token.source &&
                   layer->bindingGeneration == token.binding &&
                   runtime_.point(token.source, token.binding) != nullptr;
        });
    if (importServices_.storageMaintenance) {
        storageMaintenance_ = std::make_unique<StorageMaintenanceOperation>(
            importServices_.storageMaintenance,
            *importServices_.scheduler,
            makeQtCompletionExecutor(this));
    }
    operationRecoveryTimer_.setInterval(20);
    connect(&operationRecoveryTimer_, &QTimer::timeout, this, [this] {
        statistics_->recoverDelivery();
        if (storageMaintenance_)
            storageMaintenance_->recoverDelivery();
    });
    operationRecoveryTimer_.start();
    operationRegistry_.setObserver(
        [this](OperationChange change, const OperationSnapshot &operation) {
            if (change == OperationChange::Removed)
                emit taskRowRemoved({operation.kind, operation.token.id});
            else
                emit taskRowChanged({{operation.kind, operation.token.id},
                                     QString::fromStdString(operation.title),
                                     QString::fromStdString(operation.detail),
                                     operation.completion,
                                     terminal(operation.state),
                                     operation.capabilities});
            publishTaskRows();
        });
    SceneOperationBindings::connectController(*this);
    SceneOperationBindings::connectVectorController(*this);
    SceneOperationBindings::connectRasterController(*this);
    SceneOperationBindings::connectRasterElevationController(*this);
    SceneOperationBindings::connectColorizeController(*this);
}

SceneSession::~SceneSession()
{
    operationRegistry_.setObserver({});
    operationRecoveryTimer_.stop();
    storageMaintenance_.reset();
    statistics_.reset();
    loadController_ = nullptr;
    importServices_.shutdown();
}

StorageMaintenanceOperation *SceneSession::storageMaintenance() const noexcept
{
    return storageMaintenance_.get();
}

SceneDocumentSnapshotPtr SceneSession::documentSnapshot() const
{
    return document_->snapshot();
}

const PointColorMapCatalogSnapshotPtr &SceneSession::colorMaps() const noexcept
{
    return document_->colorMaps();
}

SceneRuntimeMetrics SceneSession::documentMetrics() const
{
    return runtime_.metrics();
}

std::optional<PointCloudStorageMetrics>
SceneSession::pointStorageMetrics(const PointCloudLayerId layerId) const
{
    const auto layer = document_->layer(layerId);
    if (!layer) {
        return std::nullopt;
    }
    const PointDatasetRuntimePtr runtime =
        runtime_.point(layer->descriptor.sourceId, layer->bindingGeneration);
    return runtime ? std::optional<PointCloudStorageMetrics>(
                         runtime->storageMetrics())
                   : std::nullopt;
}

RasterColorizeResourceEstimate SceneSession::rasterColorizeResourceEstimate(
    const PointCloudLayerId pointLayerId,
    const SceneLayerId rasterLayerId) const
{
    assertOwnerThread(*this);
    const auto point = document_->layer(pointLayerId);
    const auto raster = document_->rasterLayer(rasterLayerId);
    if (!point || !raster) {
        throw std::invalid_argument(
            "Colorization requires an attached point cloud and raster");
    }
    const PointDatasetRuntimePtr pointRuntime =
        runtime_.point(point->descriptor.sourceId, point->bindingGeneration);
    const RasterTileSourcePtr rasterRuntime =
        runtime_.raster(raster->descriptor.sourceId, raster->bindingGeneration);
    if (!pointRuntime || !rasterRuntime) {
        throw std::logic_error(
            "Colorization requires active point and raster runtime bindings");
    }
    const auto target = pointRuntime->rasterPointColorizeTarget();
    if (!target) {
        throw RasterColorizeError(RasterColorizeFailureCode::SourceChanged,
                                  "Point-cloud colorization target changed");
    }
    RasterColorizeOptions options;
    options.temporaryDirectory = importServices_.workingDirectory.empty()
                                     ? std::filesystem::temp_directory_path()
                                     : importServices_.workingDirectory;
    return pci::rasterColorizeResourceEstimate(preflightRasterPointColorize(
        *target, raster->descriptor.metadata, options));
}

std::uint64_t SceneSession::availablePointMemoryBytes() const noexcept
{
    return memoryBudget_->availableBytes();
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

RasterElevationController &SceneSession::rasterElevationController() noexcept
{
    return *importServices_.rasterElevation;
}

PointCloudColorizeController &SceneSession::colorizeController() noexcept
{
    return *importServices_.colorize;
}

StatisticsSubscription
SceneSession::analyzeStatistics(PointCloudLayerId layerId,
                                StatisticsSubscription::Observer observer)
{
    assertOwnerThread(*this);
    const auto layer = document_->layer(layerId);
    if (!layer)
        return {};
    return statistics_->start(layer->descriptor.metadata,
                              {sessionGeneration_,
                               layerId,
                               layer->descriptor.sourceId,
                               layer->bindingGeneration},
                              std::move(observer));
}

bool SceneSession::loading() const noexcept
{
    return pointLoads_->loading_;
}

bool SceneSession::hasActiveVectorLoads() const noexcept
{
    return !pendingVectorImports_.empty() ||
           importServices_.vector->hasActiveJobs();
}

bool SceneSession::hasActiveRasterLoads() const noexcept
{
    return importServices_.raster->hasActiveJobs() ||
           importServices_.rasterElevation->hasActiveJobs();
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
    return pointLoads_->timings_;
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

SessionGeneration SceneSession::sessionGeneration() const noexcept
{
    return sessionGeneration_;
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
            runtime_.decodedResidentBytes();
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
    runtime_.syncResidencyBudgets();
    publishDocument();
    return byteBudget == requestedByteBudget;
}

void SceneSession::newScene()
{
    assertOwnerThread(*this);
    const SessionGeneration nextSession = nextGeneration(sessionGeneration_);

    // Complete all fallible document/runtime preparation before retiring jobs.
    auto replacement = std::make_shared<SceneDocument>(document_->colorMaps());
    installDocument(std::move(replacement));
    sessionGeneration_ = nextSession;
    pointLoads_->activeLoads_.clear();
    preselectedVectorSublayers_.clear();
    pointLoads_->progressTimer_.stop();
    pointLoads_->loading_ = false;
    pointLoads_->batchLoading_ = false;
    pointLoads_->batchReplacing_ = false;
    pointLoads_->batchProgressUpdatesEnabled_ = false;
    pointLoads_->batchOrder_.clear();
    pointLoads_->batchTotal_ = 0;
    pointLoads_->batchSafetySamplesAtStart_ = 0;
    pointLoads_->batchTimeToFirstPointsMilliseconds_.reset();
    pointLoads_->batchTimeToAllFirstPointsMilliseconds_.reset();
    pointLoads_->batchTimeToAllDisplayReadyMilliseconds_.reset();
    pointLoads_->timings_ = {};

    statistics_->cancelAll();
    cancelAllLoads();
    emit loadingChanged(false, false, {});
    publishInstalledPointDocument(
        {.sessionGeneration = {},
         .snapshot = {},
         .runtime = {},
         .runtimeBudget = {},
         .rendererPolicy = RendererDocumentPolicy::ResetPointView});
    publishTaskRows();
    emit statusChanged(QStringLiteral("New scene"));
}

void SceneSession::publishTaskRows()
{
    // Bulk snapshots are materialized only for explicit snapshot subscribers.
    // The application consumes registry deltas, so progress never projects all
    // rows.
    if (!isSignalConnected(
            QMetaMethod::fromSignal(&SceneSession::taskRowsChanged)))
        return;
    LoadJobRows rows;
    for (const auto &operation : operationRegistry_.snapshots()) {
        rows.push_back({{operation.kind, operation.token.id},
                        QString::fromStdString(operation.title),
                        QString::fromStdString(operation.detail),
                        operation.completion,
                        terminal(operation.state),
                        operation.capabilities});
    }
    emit taskRowsChanged(std::move(rows));
}

void SceneSession::loadPointCloud(const std::filesystem::path &sourcePath,
                                  const PointCloudLoadMode mode)
{
    pointLoads_->loadPointCloud(sourcePath, mode);
}

void SceneSession::loadPointClouds(
    std::vector<std::filesystem::path> sourcePaths,
    const PointCloudLoadMode firstMode)
{
    pointLoads_->loadPointClouds(std::move(sourcePaths), firstMode);
}

void SceneSession::openSources(std::vector<SupportedSource> sources)
{
    assertOwnerThread(*this);
    if (sources.empty()) {
        return;
    }
    if (pointLoads_->loading_) {
        emit statusChanged(
            QStringLiteral("Finish or cancel the active point-cloud load "
                           "before opening more files."));
        return;
    }

    const std::string targetSpatialReferenceWkt =
        document_->referenceSpatialReferenceWkt();
    const std::optional<Bounds3d> targetExtent =
        document_->visibleSceneBounds();
    std::vector<std::filesystem::path> pointClouds;
    std::vector<VectorImportRequest> vectors;
    std::vector<RasterImportRequest> rasters;
    pointClouds.reserve(sources.size());
    vectors.reserve(sources.size());
    rasters.reserve(sources.size());

    for (SupportedSource &source : sources) {
        switch (source.kind) {
        case SupportedSourceKind::PointCloud:
            pointClouds.push_back(std::move(source.path));
            break;
        case SupportedSourceKind::Vector:
            vectors.push_back({
                .sourcePath = std::move(source.path),
                .sublayers = {},
                .origin = std::nullopt,
                .limits = {},
                .targetSpatialReferenceWkt = targetSpatialReferenceWkt,
                .targetExtent = targetExtent,
                .stopToken = {},
                .progress = {},
            });
            break;
        case SupportedSourceKind::Raster:
            rasters.push_back({
                .sourcePath = std::move(source.path),
                .targetSpatialReferenceWkt = targetSpatialReferenceWkt,
                .targetExtent = targetExtent,
                .stopToken = {},
                .phase = {},
            });
            break;
        }
    }

    if (!pointClouds.empty()) {
        loadPointClouds(std::move(pointClouds), PointCloudLoadMode::Add);
    }
    for (RasterImportRequest &request : rasters) {
        static_cast<void>(startRasterImport(std::move(request)));
    }
    if (!rasters.empty() || !vectors.empty()) {
        emit showTasksRequested();
    }

    switch (vectorImportAvailability_) {
    case VectorImportAvailability::Supported:
        for (VectorImportRequest &request : vectors) {
            static_cast<void>(startVectorImport(std::move(request)));
        }
        break;
    case VectorImportAvailability::Unknown:
        pendingVectorImports_.insert(pendingVectorImports_.end(),
                                     std::make_move_iterator(vectors.begin()),
                                     std::make_move_iterator(vectors.end()));
        if (!vectors.empty()) {
            emit statusChanged(QStringLiteral(
                "Waiting for vector-overlay support to initialize…"));
        }
        break;
    case VectorImportAvailability::Unsupported:
        if (!vectors.empty()) {
            emit statusChanged(
                vectorImportUnavailableReason_.isEmpty()
                    ? QStringLiteral(
                          "Vector sources are not supported by the active "
                          "renderer.")
                    : vectorImportUnavailableReason_);
        }
        break;
    }
}

void SceneSession::setVectorImportAvailability(
    const VectorImportAvailability availability, QString reason)
{
    assertOwnerThread(*this);
    vectorImportAvailability_ = availability;
    vectorImportUnavailableReason_ = std::move(reason);
    if (availability == VectorImportAvailability::Unknown ||
        pendingVectorImports_.empty()) {
        return;
    }
    if (availability == VectorImportAvailability::Unsupported) {
        const std::size_t count = pendingVectorImports_.size();
        pendingVectorImports_.clear();
        emit statusChanged(
            vectorImportUnavailableReason_.isEmpty()
                ? QStringLiteral("Skipped %1 vector source(s): vector "
                                 "overlays are unavailable.")
                      .arg(count)
                : vectorImportUnavailableReason_);
        return;
    }

    std::vector<VectorImportRequest> pending = std::move(pendingVectorImports_);
    pendingVectorImports_.clear();
    for (VectorImportRequest &request : pending) {
        static_cast<void>(startVectorImport(std::move(request)));
    }
}

void SceneSession::beginLoad(const std::filesystem::path &sourcePath,
                             const PointCloudLoadMode mode)
{
    pointLoads_->beginLoad(sourcePath, mode);
}

LoadJobId SceneSession::startLoadJob(const std::filesystem::path &sourcePath,
                                     const PointCloudLoadMode mode,
                                     const std::size_t order)
{
    return pointLoads_->startLoadJob(sourcePath, mode, order);
}

void SceneSession::trackLoadJob(
    const LoadJobId jobId,
    const std::filesystem::path &sourcePath,
    const PointCloudLoadMode mode,
    const std::size_t order,
    const std::chrono::steady_clock::time_point dispatchTime)
{
    pointLoads_->trackLoadJob(jobId, sourcePath, mode, order, dispatchTime);
}

void SceneSession::cancelAll()
{
    pointLoads_->cancelAll();
}

void SceneSession::cancelJob(const LoadJobId jobId)
{
    pointLoads_->cancelJob(jobId);
}

void SceneSession::retryJob(const LoadJobId jobId)
{
    pointLoads_->retryJob(jobId);
}

void SceneSession::prioritizeJob(const LoadJobId jobId)
{
    pointLoads_->prioritizeJob(jobId);
}

void SceneSession::dismissJob(const LoadJobId jobId)
{
    pointLoads_->dismissJob(jobId);
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
    request.session = sessionGeneration_;
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
    request.session = sessionGeneration_;
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
    const PointDatasetRuntimePtr pointRuntime =
        runtime_.point(point->descriptor.sourceId, point->bindingGeneration);
    const RasterTileSourcePtr rasterRuntime =
        runtime_.raster(raster->descriptor.sourceId, raster->bindingGeneration);
    if (!pointRuntime || !rasterRuntime) {
        throw std::logic_error(
            "Colorization requires active point and raster runtime bindings");
    }
    if (options.temporaryDirectory.empty()) {
        options.temporaryDirectory =
            importServices_.workingDirectory.empty()
                ? std::filesystem::temp_directory_path()
                : importServices_.workingDirectory;
    }
    std::optional<SpatialReferenceRelation> relation;
    if (importServices_.spatialReferences) {
        relation = importServices_.spatialReferences->compare(
            point->descriptor.metadata.spatialReferenceWkt,
            raster->descriptor.metadata.spatialReferenceWkt);
    }
    const auto decode = resolveRasterDecodeParameters(
        raster->descriptor.metadata, raster->style, *document_->colorMaps());
    const QString targetName = pointLayerName(*point);
    const QString sourceName = rasterLayerName(*raster);
    LoadJobId jobId;
    try {
        jobId = colorizeController().startColorize(
            {
                .token =
                    RasterColorizeCommitToken{
                        .pointLayerId = pointLayerId,
                        .pointSourceId = point->descriptor.sourceId,
                        .pointBindingGeneration = point->bindingGeneration,
                        .pointColorGeneration = point->colorGeneration,
                        .rasterLayerId = rasterLayerId,
                        .rasterSourceId = raster->descriptor.sourceId,
                        .rasterBindingGeneration = raster->bindingGeneration,
                        .rasterRenderGeneration = raster->renderGeneration,
                        .session = sessionGeneration_,
                    },
                .pointRuntime = pointRuntime,
                .raster = rasterRuntime,
                .decode = decode,
                .options = std::move(options),
                .memoryBudget = memoryBudget_,
                .crsRelation = relation,
                .pointLayerName = targetName.toStdString(),
                .rasterLayerName = sourceName.toStdString(),
            },
            [this] {
                runtime_.syncResidencyBudgets();
            });
    } catch (...) {
        runtime_.syncResidencyBudgets();
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
    const PointDatasetRuntimePtr pointRuntime =
        runtime_.point(point->descriptor.sourceId, point->bindingGeneration);
    if (!pointRuntime) {
        return false;
    }
    const QString targetName = pointLayerName(*point);
    auto publication = pointRuntime->preparePointColors();
    if (!publication) {
        return false;
    }
    auto candidate = std::make_shared<SceneDocument>(*document_);
    static_cast<void>(candidate->clearLayerRasterColors(pointLayerId));
    if (!point->descriptor.metadata.hasColor &&
        point->colorMode.source == PointColorSource::Rgb) {
        static_cast<void>(candidate->setLayerColorMode(
            pointLayerId,
            defaultPointColorMode(*document_->colorMaps(),
                                  point->descriptor.metadata)));
    }
    if (!commitColorPublication(std::move(candidate),
                                pointRuntime,
                                pointLayerId,
                                std::move(publication))) {
        return false;
    }
    colorizeController().cancelAndWaitForPointLayer(pointLayerId);
    runtime_.syncResidencyBudgets();
    publishTaskRows();
    emit statusChanged(
        QStringLiteral("Restored source colors for %1.").arg(targetName));
    return true;
}

void SceneSession::cancelAllLoads()
{
    assertOwnerThread(*this);
    pendingVectorImports_.clear();
    cancelAll();
    vectorLoadController().cancelAll();
    rasterLoadController().cancelAll();
    rasterElevationController().cancelAll();
    colorizeController().cancelAll();
}

// Registry endpoints preserve each concrete workflow's control policy.
void SceneSession::cancelJob(const LoadJobKey key)
{
    assertOwnerThread(*this);
    const auto operation = operationRegistry_.find(key.id);
    if (operation && operation->kind == key.kind)
        static_cast<void>(operationRegistry_.cancel(key.id));
}

void SceneSession::retryJob(const LoadJobKey key)
{
    assertOwnerThread(*this);
    const auto operation = operationRegistry_.find(key.id);
    if (operation && operation->kind == key.kind)
        static_cast<void>(operationRegistry_.retry(key.id));
}

void SceneSession::prioritizeJob(const LoadJobKey key)
{
    assertOwnerThread(*this);
    const auto operation = operationRegistry_.find(key.id);
    if (operation && operation->kind == key.kind)
        static_cast<void>(operationRegistry_.prioritize(key.id));
}

void SceneSession::dismissJob(const LoadJobKey key)
{
    assertOwnerThread(*this);
    const auto operation = operationRegistry_.find(key.id);
    if (operation && operation->kind == key.kind)
        static_cast<void>(operationRegistry_.dismiss(key.id));
}

void SceneSession::setLayerVisible(const SceneLayerId layerId,
                                   const bool visible)
{
    assertOwnerThread(*this);
    const auto point = document_->layer(layerId);
    const bool pointVisibilityChanged = point && point->visible != visible;
    if (pointVisibilityChanged) {
        if (!runtime_.setPointActive(point->descriptor.sourceId,
                                     point->bindingGeneration,
                                     visible)) {
            throw std::logic_error(
                "visible point layer has no matching runtime binding");
        }
        if (!document_->setLayerVisible(layerId, visible)) {
            if (!runtime_.setPointActive(point->descriptor.sourceId,
                                         point->bindingGeneration,
                                         point->visible)) {
                std::terminate();
            }
            throw std::logic_error(
                "point layer disappeared during visibility update");
        }
        publishDocument();
        return;
    }
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
    if (!document_->setRasterLayerStyle(layerId, std::move(style))) {
        return;
    }
    const auto layer = document_->rasterLayer(layerId);
    if (layer && layer->style.renderMode == RasterRenderMode::Surface &&
        layer->elevationStatus == RasterElevationStatus::Unknown) {
        static_cast<void>(document_->setRasterElevationState(
            layerId, RasterElevationStatus::Scanning));
        try {
            const RasterTileSourcePtr source = runtime_.raster(
                layer->descriptor.sourceId, layer->bindingGeneration);
            static_cast<void>(rasterElevationController().start(
                {.layerId = layerId,
                 .sourceId = layer->descriptor.sourceId,
                 .bindingGeneration = layer->bindingGeneration},
                source));
        } catch (const std::exception &error) {
            static_cast<void>(document_->setRasterElevationState(
                layerId,
                RasterElevationStatus::Failed,
                std::nullopt,
                error.what()));
        }
    }
    publishDocument();
}

void SceneSession::retryRasterElevation(const SceneLayerId layerId)
{
    assertOwnerThread(*this);
    const auto layer = document_->rasterLayer(layerId);
    if (!layer || !layer->descriptor.metadata.elevation.available) {
        return;
    }
    bool accepted = false;
    const std::vector<RasterElevationJobState> states =
        rasterElevationController().jobStates();
    if (const auto state = std::ranges::find(
            states, layerId, &RasterElevationJobState::layerId);
        state != states.end()) {
        accepted = rasterElevationController().retry(state->jobId);
    }
    if (!accepted) {
        try {
            const RasterTileSourcePtr source = runtime_.raster(
                layer->descriptor.sourceId, layer->bindingGeneration);
            static_cast<void>(rasterElevationController().start(
                {.layerId = layerId,
                 .sourceId = layer->descriptor.sourceId,
                 .bindingGeneration = layer->bindingGeneration},
                source));
            accepted = true;
        } catch (const std::exception &error) {
            static_cast<void>(document_->setRasterElevationState(
                layerId,
                RasterElevationStatus::Failed,
                std::nullopt,
                error.what()));
        }
    }
    if (accepted) {
        static_cast<void>(document_->setRasterElevationState(
            layerId, RasterElevationStatus::Scanning));
    }
    publishDocument();
}

void SceneSession::cancelRasterElevation(const SceneLayerId layerId)
{
    assertOwnerThread(*this);
    rasterElevationController().cancelLayer(layerId);
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
                                               layer.descriptor.metadata,
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
            if (!layer.descriptor.metadata.hasClassification ||
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
    const auto point = document_->layer(layerId);
    const auto raster = document_->rasterLayer(layerId);
    auto candidate = std::make_shared<SceneDocument>(*document_);
    if (!candidate->removeLayer(layerId)) {
        return;
    }
    if (point) {
        installDocument(std::move(candidate), {}, false);
    } else {
        auto registry = runtime_.copyBindings();
        if (raster) {
            static_cast<void>(registry.stageDetach(raster->bindingGeneration));
        }
        // Cache both immutable publications before the paired swap.
        static_cast<void>(candidate->snapshot());
        static_cast<void>(registry.snapshot());
        if (beforePointCommit_) {
            beforePointCommit_();
        }
        document_.swap(candidate);
        runtime_.swap(registry);
    }
    if (point) {
        statistics_->cancelLayer(layerId);
        colorizeController().cancelForPointLayer(layerId);
    }
    if (raster) {
        colorizeController().cancelForRasterLayer(layerId);
        rasterElevationController().cancelLayer(layerId);
    }
    if (ActiveLoad *load = activeLoadByLayerId(layerId)) {
        load->admitted = false;
        load->layerId.reset();
        loadController_->cancel(load->jobId);
    }
    publishInstalledPointDocument();
}

void SceneSession::isolateLayer(const SceneLayerId layerId)
{
    assertOwnerThread(*this);
    if (document_->layerKind(layerId) == SceneLayerKind::None) {
        return;
    }
    const std::vector<PointCloudLayer> pointLayers = document_->layers();
    std::vector<PointRuntimeActivityUpdate> activity;
    activity.reserve(pointLayers.size());
    for (const PointCloudLayer &point : pointLayers) {
        const bool active = point.id == layerId;
        if (point.visible != active) {
            activity.push_back({.sourceId = point.descriptor.sourceId,
                                .generation = point.bindingGeneration,
                                .active = active});
        }
    }
    if (!runtime_.setPointActive(activity)) {
        throw std::logic_error(
            "point-layer isolation has an inconsistent runtime binding");
    }
    if (document_->isolateLayer(layerId)) {
        publishDocument();
    }
}

void SceneSession::showAllLayers()
{
    assertOwnerThread(*this);
    const std::vector<PointCloudLayer> pointLayers = document_->layers();
    std::vector<PointRuntimeActivityUpdate> activity;
    activity.reserve(pointLayers.size());
    for (const PointCloudLayer &point : pointLayers) {
        if (!point.visible) {
            activity.push_back({.sourceId = point.descriptor.sourceId,
                                .generation = point.bindingGeneration,
                                .active = true});
        }
    }
    if (!runtime_.setPointActive(activity)) {
        throw std::logic_error(
            "show-all has an inconsistent point runtime binding");
    }
    if (document_->setAllLayersVisible(true)) {
        publishDocument();
    }
}

void SceneSession::showLoadProgress(const LoadJobId jobId,
                                    const PointCloudImportStage stage,
                                    const std::uint64_t processed,
                                    const std::uint64_t total)
{
    pointLoads_->showLoadProgress(jobId, stage, processed, total);
}

void SceneSession::onRenderLoadProgress(const RenderLoadProgress &progress)
{
    pointLoads_->onRenderLoadProgress(progress);
}

void SceneSession::handlePointData(const LoadJobId jobId,
                                   const PointDatasetEvent &event)
{
    pointLoads_->handlePointData(jobId, event);
}

void SceneSession::handlePointPrepared(const LoadJobId jobId,
                                       const PreparedPointDatasetPtr &dataset)
{
    pointLoads_->handlePointPrepared(jobId, dataset);
}

ScenePublicationCoordinator SceneSession::publicationCoordinator()
{
    return ScenePublicationCoordinator({document_,
                                        runtime_,
                                        sessionGeneration_,
                                        decodedByteBudget_,
                                        beforePointCommit_,
                                        [this](DocumentUpdate update) {
                                            notifyCommittedDocument(
                                                std::move(update));
                                        }});
}
void SceneSession::commitPointPublication(
    ActiveLoad &load, std::unique_ptr<PointDatasetPublication> publication)
{
    publicationCoordinator().commitPoints(
        load.layerId.value_or(PointCloudLayerId{}),
        load.scene,
        std::move(publication));
}
bool SceneSession::commitColorPublication(
    SceneDocumentPtr candidate,
    const PointDatasetRuntimePtr &pointRuntime,
    PointCloudLayerId layerId,
    std::unique_ptr<PointColorPublication> publication)
{
    return publicationCoordinator().commitColors(
        std::move(candidate), pointRuntime, layerId, std::move(publication));
}
void SceneSession::publishPreparedDocument(SceneDocumentPtr document,
                                           SceneRuntime runtime,
                                           DocumentUpdate update)
{
    publicationCoordinator().install(
        std::move(document), std::move(runtime), std::move(update));
}

void SceneSession::notifyCommittedDocument(DocumentUpdate update) noexcept
{
    try {
        if (statistics_)
            statistics_->cancelInvalidTargets();
        emit documentChanged(std::move(update));
    } catch (const std::exception &error) {
        qWarning("Scene data committed; document observer failed: %s",
                 error.what());
    } catch (...) {
        qWarning("Point data committed; document observer failed");
    }
}

void SceneSession::publishInstalledPointDocument(DocumentUpdate update) noexcept
{
    update.sessionGeneration = sessionGeneration_;
    update.snapshot = document_->snapshot();
    update.runtime = runtime_.snapshot();
    update.runtimeBudget = {.decodedPointBytes = decodedByteBudget_};
    notifyCommittedDocument(std::move(update));
}

void SceneSession::handleSceneReadySingle(ActiveLoad &load)
{
    pointLoads_->handleSceneReadySingle(load);
}

void SceneSession::admitBatchLayers()
{
    pointLoads_->admitBatchLayers();
}

void SceneSession::handleLoadCompleted(const LoadJobId jobId,
                                       const PreparedPointDatasetPtr &dataset)
{
    pointLoads_->handleLoadCompleted(jobId, dataset);
}

void SceneSession::markFirstFrameReady(ActiveLoad &load)
{
    pointLoads_->markFirstFrameReady(load);
}

void SceneSession::markDisplayCompleted(ActiveLoad &load)
{
    pointLoads_->markDisplayCompleted(load);
}

void SceneSession::finishJobIfComplete(ActiveLoad &load)
{
    pointLoads_->finishJobIfComplete(load);
}

void SceneSession::finalizeLoads(const QString &status)
{
    pointLoads_->finalizeLoads(status);
}

void SceneSession::handleLoadCancelled(const LoadJobId jobId)
{
    pointLoads_->handleLoadCancelled(jobId);
}

void SceneSession::rollbackAdmittedLoad(ActiveLoad &load)
{
    pointLoads_->rollbackAdmittedLoad(load);
}

void SceneSession::showLoadFailure(const LoadJobId jobId,
                                   const QString &message)
{
    pointLoads_->showLoadFailure(jobId, message);
}

void SceneSession::updateBatchProgress()
{
    pointLoads_->updateBatchProgress();
}

SceneSession::ActiveLoad *SceneSession::activeLoad(const LoadJobId jobId)
{
    return pointLoads_->activeLoad(jobId);
}

SceneSession::ActiveLoad *
SceneSession::activeLoadByLayerId(const PointCloudLayerId layerId)
{
    return pointLoads_->activeLoadByLayerId(layerId);
}

BindingGeneration SceneSession::allocateBindingGeneration()
{
    if (document_->lastBindingGeneration() > bindingGeneration_) {
        bindingGeneration_ = document_->lastBindingGeneration();
    }
    bindingGeneration_ = nextGeneration(bindingGeneration_);
    return bindingGeneration_;
}

void SceneSession::attachRasterRuntime(const RasterLayerDataPtr &data,
                                       const BindingGeneration generation)
{
    if (!data || !runtime_.attachRaster({.descriptor = data->descriptor(),
                                         .source = data->source,
                                         .generation = generation})) {
        throw std::logic_error("raster runtime binding could not be attached");
    }
}

SceneRuntime SceneSession::runtimeForDocument(
    const SceneDocument &document,
    const std::span<const PointRuntimeBinding> preparedPoints) const
{
    auto cache = runtime_.decodedPageCache()->clone();
    for (const auto &old : document_->layers()) {
        const bool retained =
            std::ranges::any_of(document.layers(), [&old](const auto &layer) {
                return layer.descriptor.sourceId == old.descriptor.sourceId;
            });
        if (!retained) {
            cache->removeSource(old.descriptor.sourceId);
        }
    }
    SceneRuntime candidate(
        decodedByteBudget_,
        HierarchyResidencyCoordinator::defaultMaximumConcurrentDecodes,
        decodeAdmission_,
        memoryBudget_,
        std::move(cache));
    for (const PointCloudLayer &layer : document.layers()) {
        PointDatasetRuntimePtr pointRuntime =
            runtime_.point(layer.descriptor.sourceId, layer.bindingGeneration);
        if (!pointRuntime) {
            const auto prepared = std::ranges::find_if(
                preparedPoints, [&layer](const PointRuntimeBinding &binding) {
                    return binding.descriptor.sourceId ==
                               layer.descriptor.sourceId &&
                           binding.generation == layer.bindingGeneration;
                });
            if (prepared != preparedPoints.end()) {
                pointRuntime = prepared->runtime;
            }
        }
        if (!pointRuntime) {
            throw std::logic_error(
                "document point runtime binding is unavailable");
        }
        if (!candidate.stagePoint({.descriptor = layer.descriptor,
                                   .runtime = std::move(pointRuntime),
                                   .generation = layer.bindingGeneration,
                                   .active = layer.visible})) {
            throw std::logic_error(
                "document contains duplicate point runtime bindings");
        }
    }
    for (const RasterLayer &layer : document.rasterLayers()) {
        const RasterTileSourcePtr source =
            runtime_.raster(layer.descriptor.sourceId, layer.bindingGeneration);
        if (!source ||
            !candidate.attachRaster({.descriptor = layer.descriptor,
                                     .source = source,
                                     .generation = layer.bindingGeneration})) {
            throw std::logic_error(
                "document contains duplicate raster runtime bindings");
        }
    }
    return candidate;
}

void SceneSession::installDocument(
    SceneDocumentPtr document,
    std::vector<PointRuntimeBinding> preparedPoints,
    const bool advanceGeneration)
{
    if (!document) {
        throw std::invalid_argument("cannot install a null scene document");
    }
    const DocumentGeneration generation =
        advanceGeneration ? nextGeneration(documentGeneration_)
                          : documentGeneration_;
    document->setGeneration(generation);
    SceneRuntime candidateRuntime =
        runtimeForDocument(*document, preparedPoints);
    // Allocate and validate the immutable publication before changing the
    // visible document. The subsequent pointer and generation assignments are
    // nonthrowing, and publishDocument reuses this cached snapshot.
    static_cast<void>(document->snapshot());
    candidateRuntime.preparePointAttachments();
    candidateRuntime.preparePointDetachments(runtime_);
    static_cast<void>(candidateRuntime.snapshot());
    if (beforePointCommit_) {
        beforePointCommit_();
    }
    candidateRuntime.commitPointAttachments();
    runtime_.swap(candidateRuntime);
    document_ = std::move(document);
    documentGeneration_ = generation;
    runtime_.resumePointRequests();
}

void SceneSession::publishDocument(DocumentUpdate update)
{
    Q_ASSERT(!update.snapshot);
    Q_ASSERT(!update.runtime);
    const std::vector<PointCloudLayer> pointLayers = document_->layers();
    std::vector<PointLayerAvailabilityUpdate> availabilityUpdates;
    availabilityUpdates.reserve(pointLayers.size());
    for (const PointCloudLayer &layer : pointLayers) {
        const PointDatasetRuntimePtr pointRuntime =
            runtime_.point(layer.descriptor.sourceId, layer.bindingGeneration);
        if (!pointRuntime) {
            throw std::logic_error(
                "document point runtime binding is inconsistent");
        }
        availabilityUpdates.push_back(
            {.layerId = layer.id,
             .availability = pointRuntime->datasetView().availability});
    }
    if (!document_->setPointLayerAvailabilities(availabilityUpdates)) {
        throw std::logic_error(
            "document point runtime binding is inconsistent");
    }
    update.sessionGeneration = sessionGeneration_;
    update.snapshot = document_->snapshot();
    update.runtime = runtime_.snapshot();
    update.runtimeBudget = {
        .decodedPointBytes = decodedByteBudget_,
    };
    emit documentChanged(std::move(update));
}

void SceneSession::refreshAutomaticMemoryBudget()
{
    assertOwnerThread(*this);
    if (!automaticMemoryBudget_ || !memoryBudget_) {
        return;
    }
    AutomaticMemoryBudgetParameters parameters = *automaticMemoryBudget_;
    parameters.currentPointBytes = runtime_.decodedResidentBytes();
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
    runtime_.syncResidencyBudgets();
    publishDocument();
}

} // namespace pci
