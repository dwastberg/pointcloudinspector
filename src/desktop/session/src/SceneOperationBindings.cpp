#include "SceneOperationBindings.h"
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

void SceneOperationBindings::connectController(SceneSession &session)
{
    QObject::connect(session.loadController_,
                     &PointCloudLoadController::progressChanged,
                     &session,
                     &SceneSession::showLoadProgress);
    QObject::connect(session.loadController_,
                     &PointCloudLoadController::dataReady,
                     &session,
                     &SceneSession::handlePointData);
    QObject::connect(session.loadController_,
                     &PointCloudLoadController::prepared,
                     &session,
                     &SceneSession::handlePointPrepared);
    QObject::connect(session.loadController_,
                     &PointCloudLoadController::loaded,
                     &session,
                     &SceneSession::handleLoadCompleted);
    QObject::connect(session.loadController_,
                     &PointCloudLoadController::failed,
                     &session,
                     &SceneSession::showLoadFailure);
    QObject::connect(session.loadController_,
                     &PointCloudLoadController::cancelled,
                     &session,
                     &SceneSession::handleLoadCancelled);
    QObject::connect(session.loadController_,
                     &PointCloudLoadController::schedulingChanged,
                     &session,
                     [&session] {
                         assertOwnerThread(session);
                         session.runtime_.syncResidencyBudgets();
                         session.publishDocument();
                         if (session.pointLoads_->batchLoading_) {
                             session.updateBatchProgress();
                         }
                         session.publishTaskRows();
                     });
    QObject::connect(session.loadController_,
                     &PointCloudLoadController::jobStateChanged,
                     &session,
                     [&session](const LoadJobId) {
                         assertOwnerThread(session);
                         session.publishTaskRows();
                         if (session.pointLoads_->batchLoading_) {
                             session.updateBatchProgress();
                         }
                     });
}

void SceneOperationBindings::connectVectorController(SceneSession &session)
{
    VectorLoadController &controller = session.vectorLoadController();
    controller.setInstaller(
        [&session](LoadJobId,
                   SessionGeneration generation,
                   AttemptGeneration,
                   const VectorSublayerKey &,
                   const VectorLayerDataPtr &data) -> JobResult<void> {
            assertOwnerThread(session);
            if (generation != session.sessionGeneration_) {
                return std::unexpected(
                    JobError{JobErrorCode::Cancelled, "retired vector import"});
            }
            const bool wasEmpty = !session.document_->hasAnyLayer();
            auto candidate =
                std::make_shared<SceneDocument>(*session.document_);
            static_cast<void>(
                candidate->addVectorLayer(data,
                                          !data->extentDisjointXY,
                                          session.allocateBindingGeneration()));
            session.publishPreparedDocument(
                std::move(candidate),
                session.runtime_.copyBindings(),
                {.sessionGeneration = {},
                 .snapshot = {},
                 .runtime = {},
                 .runtimeBudget = {},
                 .viewAdjustment = wasEmpty ? ViewAdjustment::FrameVisibleLayers
                                            : ViewAdjustment::Preserve});
            return {};
        });
    QObject::connect(
        &controller,
        &VectorLoadController::sublayerFailed,
        &session,
        [&session](const LoadJobId,
                   const VectorSublayerKey &,
                   const QString &message) {
            assertOwnerThread(session);
            emit session.statusChanged(
                QStringLiteral("Vector sublayer failed: %1").arg(message));
        });
    QObject::connect(
        &controller,
        &VectorLoadController::failed,
        &session,
        [&session](const LoadJobId jobId, const QString &message) {
            assertOwnerThread(session);
            session.preselectedVectorSublayers_.erase(jobId);
            emit session.statusChanged(
                QStringLiteral("Vector import failed: %1").arg(message));
            session.publishTaskRows();
            emit session.vectorJobFinished(jobId);
        });
    QObject::connect(
        &controller,
        &VectorLoadController::finished,
        &session,
        [&session](const LoadJobId jobId, const VectorLoadSummary &) {
            assertOwnerThread(session);
            session.preselectedVectorSublayers_.erase(jobId);
            session.publishTaskRows();
            emit session.vectorJobFinished(jobId);
        });
    QObject::connect(
        &controller,
        &VectorLoadController::cancelled,
        &session,
        [&session](const LoadJobId jobId, const VectorLoadSummary &) {
            assertOwnerThread(session);
            session.preselectedVectorSublayers_.erase(jobId);
            session.publishTaskRows();
            emit session.vectorJobFinished(jobId);
        });
    QObject::connect(
        &controller,
        &VectorLoadController::inspected,
        &session,
        [&session](const LoadJobId jobId,
                   const VectorImportPreflight &preflight) {
            assertOwnerThread(session);
            if (auto selected = session.preselectedVectorSublayers_.find(jobId);
                selected != session.preselectedVectorSublayers_.end()) {
                std::vector<VectorSublayerKey> sublayers =
                    std::move(selected->second);
                session.preselectedVectorSublayers_.erase(selected);
                static_cast<void>(
                    session.continueVectorImport(jobId, std::move(sublayers)));
                return;
            }
            if (preflight.sublayers.size() == 1) {
                static_cast<void>(session.continueVectorImport(
                    jobId, {preflight.sublayers.front().key}));
                return;
            }
            emit session.vectorSelectionRequired(jobId, preflight);
        });
    QObject::connect(&controller,
                     &VectorLoadController::jobStateChanged,
                     &session,
                     [&session](const LoadJobId jobId) {
                         assertOwnerThread(session);
                         session.publishTaskRows();
                         static_cast<void>(jobId);
                     });
}

void SceneOperationBindings::connectRasterController(SceneSession &session)
{
    RasterLoadController &controller = session.rasterLoadController();
    controller.setInstaller([&session](
                                LoadJobId,
                                SessionGeneration generation,
                                AttemptGeneration,
                                const RasterLayerDataPtr &data,
                                bool initiallyVisible) -> JobResult<void> {
        assertOwnerThread(session);
        if (generation != session.sessionGeneration_) {
            return std::unexpected(
                JobError{JobErrorCode::Cancelled, "retired raster import"});
        }
        const bool wasEmpty = !session.document_->hasAnyLayer();
        const auto binding = session.allocateBindingGeneration();
        auto candidate = std::make_shared<SceneDocument>(*session.document_);
        auto registry = session.runtime_.copyBindings();
        if (!registry.attachRaster({.descriptor = data->descriptor(),
                                    .source = data->source,
                                    .generation = binding})) {
            throw std::invalid_argument("duplicate raster runtime binding");
        }
        static_cast<void>(candidate->addRasterLayer(
            data->descriptor(), initiallyVisible, binding));
        session.publishPreparedDocument(
            std::move(candidate),
            std::move(registry),
            {.sessionGeneration = {},
             .snapshot = {},
             .runtime = {},
             .runtimeBudget = {},
             .viewAdjustment = wasEmpty ? ViewAdjustment::FrameVisibleLayers
                                        : ViewAdjustment::Preserve});
        return {};
    });
    QObject::connect(
        &controller,
        &RasterLoadController::failed,
        &session,
        [&session](const LoadJobId, const QString &message) {
            assertOwnerThread(session);
            emit session.statusChanged(
                QStringLiteral("Raster import failed: %1").arg(message));
            session.publishTaskRows();
        });
    QObject::connect(&controller,
                     &RasterLoadController::cancelled,
                     &session,
                     [&session](const LoadJobId) {
                         assertOwnerThread(session);
                         session.publishTaskRows();
                     });
    QObject::connect(&controller,
                     &RasterLoadController::jobStateChanged,
                     &session,
                     [&session](const LoadJobId) {
                         assertOwnerThread(session);
                         session.publishTaskRows();
                     });
}

void SceneOperationBindings::connectRasterElevationController(
    SceneSession &session)
{
    RasterElevationController &controller = session.rasterElevationController();
    QObject::connect(
        &controller,
        &RasterElevationController::completed,
        &session,
        [&session](const LoadJobId,
                   const RasterElevationBindingToken token,
                   const RasterElevationRange range) {
            assertOwnerThread(session);
            const auto layer = session.document_->rasterLayer(token.layerId);
            if (!layer || layer->descriptor.sourceId != token.sourceId ||
                layer->bindingGeneration != token.bindingGeneration ||
                !session.runtime_.raster(token.sourceId,
                                         token.bindingGeneration)) {
                return;
            }
            if (session.document_->setRasterElevationState(
                    token.layerId, RasterElevationStatus::Ready, range)) {
                session.publishDocument();
            }
            session.publishTaskRows();
        });
    QObject::connect(
        &controller,
        &RasterElevationController::failed,
        &session,
        [&session](const LoadJobId,
                   const RasterElevationBindingToken token,
                   const QString &message) {
            assertOwnerThread(session);
            const auto layer = session.document_->rasterLayer(token.layerId);
            if (!layer || layer->descriptor.sourceId != token.sourceId ||
                layer->bindingGeneration != token.bindingGeneration ||
                !session.runtime_.raster(token.sourceId,
                                         token.bindingGeneration)) {
                return;
            }
            if (session.document_->setRasterElevationState(
                    token.layerId,
                    RasterElevationStatus::Failed,
                    std::nullopt,
                    message.toStdString())) {
                session.publishDocument();
            }
            session.publishTaskRows();
        });
    QObject::connect(
        &controller,
        &RasterElevationController::cancelled,
        &session,
        [&session](const LoadJobId, const RasterElevationBindingToken token) {
            assertOwnerThread(session);
            const auto layer = session.document_->rasterLayer(token.layerId);
            if (!layer || layer->descriptor.sourceId != token.sourceId ||
                layer->bindingGeneration != token.bindingGeneration ||
                !session.runtime_.raster(token.sourceId,
                                         token.bindingGeneration)) {
                return;
            }
            if (session.document_->setRasterElevationState(
                    token.layerId,
                    RasterElevationStatus::Failed,
                    std::nullopt,
                    "Elevation analysis was cancelled")) {
                session.publishDocument();
            }
            session.publishTaskRows();
        });
    QObject::connect(&controller,
                     &RasterElevationController::jobStateChanged,
                     &session,
                     [&session](const LoadJobId) {
                         session.publishTaskRows();
                     });
}

void SceneOperationBindings::connectColorizeController(SceneSession &session)
{
    PointCloudColorizeController &controller = session.colorizeController();
    QObject::connect(
        &controller,
        &PointCloudColorizeController::prepared,
        &session,
        [&session](const LoadJobId jobId,
                   const RasterColorizeCommitToken token,
                   PointColorInstallationPtr installation,
                   RasterPointColorBinding binding) {
            assertOwnerThread(session);
            const auto point = session.document_->layer(token.pointLayerId);
            const auto raster =
                session.document_->rasterLayer(token.rasterLayerId);
            const PointDatasetRuntimePtr pointRuntime = session.runtime_.point(
                token.pointSourceId, token.pointBindingGeneration);
            if (token.session != session.sessionGeneration_ || !point ||
                !raster || !pointRuntime ||
                point->descriptor.sourceId != token.pointSourceId ||
                point->bindingGeneration != token.pointBindingGeneration ||
                point->colorGeneration != token.pointColorGeneration ||
                raster->descriptor.sourceId != token.rasterSourceId ||
                raster->bindingGeneration != token.rasterBindingGeneration ||
                !session.runtime_.raster(token.rasterSourceId,
                                         token.rasterBindingGeneration) ||
                raster->renderGeneration != token.rasterRenderGeneration) {
                session.colorizeController().finishCommit(
                    jobId,
                    false,
                    QStringLiteral("Layer state changed while colors were "
                                   "being prepared; try again"));
                session.runtime_.syncResidencyBudgets();
                session.publishTaskRows();
                return;
            }
            try {
                auto publication =
                    pointRuntime->preparePointColors(std::move(installation));
                if (!publication) {
                    session.colorizeController().finishCommit(jobId, false);
                    return;
                }
                auto candidate =
                    std::make_shared<SceneDocument>(*session.document_);
                if (!candidate->setLayerRasterColors(token.pointLayerId,
                                                     std::move(binding))) {
                    throw std::logic_error(
                        "Color transaction lost its point layer");
                }
                static_cast<void>(candidate->setLayerColorMode(
                    token.pointLayerId,
                    PointColorMode{.source = PointColorSource::Rgb,
                                   .colorMap = PointColorMap::Rgb,
                                   .manualRange = std::nullopt}));
                if (!session.commitColorPublication(std::move(candidate),
                                                    pointRuntime,
                                                    token.pointLayerId,
                                                    std::move(publication))) {
                    session.colorizeController().finishCommit(jobId, false);
                    return;
                }
            } catch (const std::exception &error) {
                session.colorizeController().finishCommit(
                    jobId, false, QString::fromUtf8(error.what()));
                session.runtime_.syncResidencyBudgets();
                session.publishTaskRows();
                return;
            }
            session.colorizeController().finishCommit(jobId, true);
            session.runtime_.syncResidencyBudgets();
            session.publishTaskRows();
            const auto completed = session.colorizeController().jobState(jobId);
            const QString targetName =
                completed && !completed->pointLayerName.empty()
                    ? QString::fromStdString(completed->pointLayerName)
                    : pointLayerName(*point);
            const QString sourceName =
                completed && !completed->rasterLayerName.empty()
                    ? QString::fromStdString(completed->rasterLayerName)
                    : rasterLayerName(*raster);
            emit session.statusChanged(
                QStringLiteral("Colorized %1 from %2. %3")
                    .arg(targetName,
                         sourceName,
                         completed ? QString::fromStdString(completed->detail)
                                   : QString{}));
        });
    QObject::connect(
        &controller,
        &PointCloudColorizeController::failed,
        &session,
        [&session](const LoadJobId, const QString &message) {
            session.runtime_.syncResidencyBudgets();
            emit session.statusChanged(
                QStringLiteral("Point-cloud colorization failed: %1")
                    .arg(message));
            session.publishTaskRows();
        });
    QObject::connect(
        &controller,
        &PointCloudColorizeController::cancelled,
        &session,
        [&session](const LoadJobId jobId) {
            session.runtime_.syncResidencyBudgets();
            const auto state = session.colorizeController().jobState(jobId);
            emit session.statusChanged(
                state && !state->pointLayerName.empty()
                    ? QStringLiteral("Colorization of %1 cancelled.")
                          .arg(QString::fromStdString(state->pointLayerName))
                    : QStringLiteral("Point-cloud colorization cancelled."));
            session.publishTaskRows();
        });
    QObject::connect(&controller,
                     &PointCloudColorizeController::jobStateChanged,
                     &session,
                     [&session](const LoadJobId) {
                         session.publishTaskRows();
                     });
}

} // namespace pci
