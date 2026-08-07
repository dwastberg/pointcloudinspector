#include "import/RasterLoadController.h"

#include "import/QueuedControllerCallback.h"
#include "platform/QtPath.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace pci {
namespace {

// Inspection reads headers and, at most, a bounded window sample. It never
// reads a full-resolution payload, so its reservation stays small.
constexpr std::uint64_t inspectionReservationBytes = std::uint64_t{1024} * 1024;

template <typename Completion>
void post(
    const std::shared_ptr<QueuedControllerTarget<RasterLoadController>> &target,
    Completion completion)
{
    postToObject(target, std::move(completion));
}

JobError rasterCancelledError()
{
    return {
        .code = JobErrorCode::Cancelled,
        .message = "Raster import cancelled",
    };
}

[[nodiscard]] bool terminal(const RasterLoadJobPhase phase) noexcept
{
    return phase == RasterLoadJobPhase::Ready ||
           phase == RasterLoadJobPhase::Failed ||
           phase == RasterLoadJobPhase::Cancelled;
}

} // namespace

RasterLoadController::RasterLoadController(
    std::shared_ptr<const RasterLoader> loader,
    TaskScheduler &scheduler,
    QObject *parent)
    : QObject(parent)
    , loader_(std::move(loader))
    , scheduler_(&scheduler)
{
    if (!loader_) {
        throw std::invalid_argument("raster loader must not be null");
    }
    callbackTarget_ =
        std::make_shared<QueuedControllerTarget<RasterLoadController>>(this);
    qRegisterMetaType<RasterLayerDataPtr>();
    qRegisterMetaType<LoadJobId>();
}

RasterLoadController::~RasterLoadController()
{
    callbackTarget_->invalidate();
    cancelAll();
}

LoadJobId RasterLoadController::createJob(RasterImportRequest request)
{
    const LoadJobId jobId = jobIds_.next("raster job ids are exhausted");
    Job job;
    job.request = std::move(request);
    job.request.stopToken = job.stop.get_token();
    job.phase = RasterLoadJobPhase::Inspecting;
    jobs_.emplace(jobId, std::move(job));
    states_.emplace(jobId,
                    RasterLoadJobState{
                        .jobId = jobId,
                        .phase = RasterLoadJobPhase::Inspecting,
                        .detail = {},
                    });
    emit jobStateChanged(jobId);
    return jobId;
}

LoadJobId RasterLoadController::startImport(RasterImportRequest request)
{
    if (request.sourcePath.empty()) {
        throw std::invalid_argument("raster import requires a source path");
    }
    const LoadJobId jobId = createJob(std::move(request));
    scheduleInspection(jobId);
    return jobId;
}

void RasterLoadController::scheduleInspection(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end())
        return;
    const std::uint64_t generation = found->second.generation;
    const auto loader = loader_;
    const auto callbackTarget = callbackTarget_;

    RasterImportRequest request = found->second.request;
    // The worker reports its phase back through the same queued callback the
    // result uses, so the UI never reads worker-thread state directly.
    request.phase =
        [callbackTarget, jobId, generation](const RasterImportPhase phase) {
            post(callbackTarget,
                 [jobId, generation, phase](RasterLoadController *controller) {
                     controller->observePhase(jobId, generation, phase);
                 });
        };

    const auto completeCancelled = [callbackTarget, jobId, generation] {
        post(
            callbackTarget,
            [jobId, generation](RasterLoadController *controller) {
                controller->finishInspection(
                    jobId, generation, std::unexpected(rasterCancelledError()));
            });
    };

    found->second.tasks.push_back(scheduler_->submit(
        TaskPriority::Inspection,
        inspectionReservationBytes,
        [callbackTarget, jobId, generation, loader, request] {
            InspectionResult result = [&]() -> InspectionResult {
                try {
                    return loader->inspect(request);
                } catch (const RasterImportCancelled &) {
                    return std::unexpected(rasterCancelledError());
                } catch (const RasterImportError &error) {
                    return std::unexpected(JobError{
                        .code = JobErrorCode::Io,
                        .message = error.what(),
                    });
                } catch (const std::exception &error) {
                    return std::unexpected(JobError{
                        .code = JobErrorCode::Internal,
                        .message = error.what(),
                    });
                }
            }();
            post(callbackTarget,
                 [jobId, generation, result = std::move(result)](
                     RasterLoadController *controller) mutable {
                     controller->finishInspection(
                         jobId, generation, std::move(result));
                 });
        },
        completeCancelled,
        jobId.value()));
}

void RasterLoadController::observePhase(const LoadJobId jobId,
                                        const std::uint64_t generation,
                                        const RasterImportPhase phase)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.generation != generation ||
        terminal(found->second.phase)) {
        return;
    }
    setState(jobId,
             phase == RasterImportPhase::SamplingRange
                 ? RasterLoadJobPhase::SamplingRange
                 : RasterLoadJobPhase::Inspecting);
}

void RasterLoadController::finishInspection(const LoadJobId jobId,
                                            const std::uint64_t generation,
                                            InspectionResult result)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.generation != generation)
        return;
    Job &job = found->second;
    if (job.cancelled ||
        (!result && result.error().code == JobErrorCode::Cancelled)) {
        setState(jobId, RasterLoadJobPhase::Cancelled);
        emitTerminal(jobId, {});
        return;
    }
    if (!result) {
        const QString message = QString::fromStdString(result.error().message);
        setState(jobId, RasterLoadJobPhase::Failed, message);
        emitTerminal(jobId, message);
        return;
    }
    if (!result->data || !result->data->source) {
        const QString message =
            QStringLiteral("Raster inspection produced no source");
        setState(jobId, RasterLoadJobPhase::Failed, message);
        emitTerminal(jobId, message);
        return;
    }

    const RasterLayerMetadata &metadata = result->data->metadata();
    // A raster that shares no XY extent with the scene starts hidden, reusing
    // the "Show anyway" affordance vector layers already provide.
    const bool initiallyVisible = !metadata.extentDisjointXY;
    setState(jobId, RasterLoadJobPhase::Ready);
    job.terminalEmitted = true;
    emit loaded(jobId, result->data, initiallyVisible);
    emit jobStateChanged(jobId);
}

bool RasterLoadController::retry(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.phase != RasterLoadJobPhase::Failed) {
        return false;
    }
    Job &job = found->second;
    ++job.generation;
    job.cancelled = false;
    job.terminalEmitted = false;
    job.tasks.clear();
    job.stop = std::stop_source{};
    job.request.stopToken = job.stop.get_token();
    setState(jobId, RasterLoadJobPhase::Inspecting);
    scheduleInspection(jobId);
    return true;
}

void RasterLoadController::cancel(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || terminal(found->second.phase)) {
        return;
    }
    Job &job = found->second;
    job.cancelled = true;
    job.stop.request_stop();
    for (const TaskScheduler::TaskId task : job.tasks) {
        static_cast<void>(scheduler_->cancel(task));
    }
}

void RasterLoadController::cancelAll()
{
    std::vector<LoadJobId> active;
    active.reserve(jobs_.size());
    for (const auto &[jobId, job] : jobs_) {
        if (!terminal(job.phase)) {
            active.push_back(jobId);
        }
    }
    for (const LoadJobId jobId : active) {
        cancel(jobId);
    }
}

bool RasterLoadController::hasActiveJobs() const noexcept
{
    return std::ranges::any_of(jobs_, [](const auto &entry) {
        return !terminal(entry.second.phase);
    });
}

std::optional<RasterLoadJobState>
RasterLoadController::jobState(const LoadJobId jobId) const
{
    const auto found = states_.find(jobId);
    if (found == states_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<RasterLoadJobState> RasterLoadController::jobStates() const
{
    return sortedJobStates<RasterLoadJobState>(states_);
}

std::vector<LoadJobRow> RasterLoadController::jobRows() const
{
    std::vector<LoadJobRow> rows;
    rows.reserve(states_.size());
    for (const RasterLoadJobState &state : jobStates()) {
        QString title = QStringLiteral("Raster import");
        if (const auto job = jobs_.find(state.jobId); job != jobs_.end()) {
            const std::filesystem::path source =
                job->second.request.sourcePath.filename();
            if (!source.empty()) {
                title = pathToQString(source);
            }
        }

        QString detail = state.detail;
        double completion = 0.0;
        switch (state.phase) {
        case RasterLoadJobPhase::Queued:
            detail = QStringLiteral("Queued");
            break;
        case RasterLoadJobPhase::Inspecting:
            detail = QStringLiteral("Reading raster metadata");
            break;
        case RasterLoadJobPhase::SamplingRange:
            detail = QStringLiteral("Sampling display range");
            completion = 0.5;
            break;
        case RasterLoadJobPhase::Ready:
            detail = QStringLiteral("Loaded");
            completion = 1.0;
            break;
        case RasterLoadJobPhase::Failed:
            break;
        case RasterLoadJobPhase::Cancelled:
            detail = QStringLiteral("Cancelled");
            break;
        }

        const bool isTerminal = terminal(state.phase);
        rows.push_back(LoadJobRow{
            .key = {.kind = LoadJobKind::Raster, .id = state.jobId},
            .title = std::move(title),
            .detail = std::move(detail),
            .completion = completion,
            .terminal = isTerminal,
            .capabilities = isTerminal
                                ? terminalLoadJobCapabilities(
                                      state.phase == RasterLoadJobPhase::Failed)
                                : activeLoadJobCapabilities(),
        });
    }
    return rows;
}

bool RasterLoadController::dismiss(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || !terminal(found->second.phase)) {
        return false;
    }
    jobs_.erase(found);
    states_.erase(jobId);
    emit jobStateChanged(jobId);
    return true;
}

const TaskScheduler *RasterLoadController::schedulerIdentity() const noexcept
{
    return scheduler_;
}

void RasterLoadController::setState(const LoadJobId jobId,
                                    const RasterLoadJobPhase phase,
                                    QString detail)
{
    const auto job = jobs_.find(jobId);
    if (job != jobs_.end()) {
        job->second.phase = phase;
    }
    const auto state = states_.find(jobId);
    if (state == states_.end()) {
        return;
    }
    state->second.phase = phase;
    if (!detail.isEmpty()) {
        state->second.detail = std::move(detail);
    }
    emit jobStateChanged(jobId);
}

void RasterLoadController::emitTerminal(const LoadJobId jobId, QString message)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.terminalEmitted) {
        return;
    }
    found->second.terminalEmitted = true;
    if (found->second.phase == RasterLoadJobPhase::Cancelled) {
        emit cancelled(jobId);
    } else {
        emit failed(jobId, std::move(message));
    }
}

} // namespace pci
