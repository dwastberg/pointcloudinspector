#include <pci/desktop/models/LoadingProgressModel.h>

#include <algorithm>

namespace pci {
namespace {

// Import and renderer run concurrently, but display readiness may require
// several seconds of page decode and GPU upload after a fast persistent-index
// reopen. Keep a meaningful portion of the bar for that measured work so a
// load cannot appear to begin at 90+ percent or move backwards when renderer
// work starts.
constexpr int readingEndPercent = 70;
constexpr int optimizingStartPercent = 70;
constexpr int optimizingEndPercent = 75;
constexpr int rendererStartPercent = 75;
constexpr int rendererEndPercent = 80;
constexpr int rendererUploadEndPercent = 99;
constexpr int firstFrameReadyPercent = 80;
constexpr int displayReadyPercent = 100;

// Reopening a persistent hierarchy does not read every source point during
// import. Its expensive work is the later page decode plus GPU upload, so a
// cache hit only earns a small setup allowance. A first-time local index build
// does scan the source and receives a larger, measured preparation range.
constexpr int pagedOpenPercent = 5;
constexpr int pagedBuildReadingEndPercent = 45;
constexpr int pagedBuildEndPercent = 55;

constexpr auto optimizingEstimate = std::chrono::milliseconds(4200);
constexpr auto rendererEstimate = std::chrono::milliseconds(1600);

int scaledPercent(const std::uint64_t processed,
                  const std::uint64_t total,
                  const int startPercent,
                  const int endPercent) noexcept
{
    if (total == 0) {
        return startPercent;
    }
    const auto span = static_cast<std::uint64_t>(endPercent - startPercent);
    const auto clamped = std::min(processed, total);
    return startPercent + static_cast<int>((clamped * span) / total);
}

} // namespace

void LoadingProgressModel::reset() noexcept
{
    state_ = {};
    importReadingComplete_ = false;
    pagedSource_ = false;
    incrementalImportProgress_ = false;
    deferredRenderProgress_.reset();
    estimateStartPercent_ = 0;
    estimateEndPercent_ = 0;
    estimateDuration_ = std::chrono::milliseconds(1);
}

void LoadingProgressModel::setPagedSource(const bool paged) noexcept
{
    pagedSource_ = paged;
}

LoadingProgressState LoadingProgressModel::state() const noexcept
{
    return state_;
}

LoadingProgressState LoadingProgressModel::completeImport() noexcept
{
    importReadingComplete_ = true;
    if (pagedSource_) {
        setPercentage(incrementalImportProgress_ ? pagedBuildEndPercent
                                                 : pagedOpenPercent);
    }
    if (deferredRenderProgress_) {
        const RenderLoadProgress deferred = *deferredRenderProgress_;
        deferredRenderProgress_.reset();
        return updateRender(deferred);
    }
    return state_;
}

LoadingProgressState
LoadingProgressModel::updateImport(const PointCloudImportStage stage,
                                   const std::uint64_t processed,
                                   const std::uint64_t total) noexcept
{
    if (stage == PointCloudImportStage::Reading) {
        incrementalImportProgress_ =
            incrementalImportProgress_ ||
            (total > 0 && processed > 0 && processed < total);
        state_.phase = LoadingProgressPhase::Reading;
        state_.completed = processed;
        state_.total = total;
        state_.estimated = false;
        if (pagedSource_) {
            setPercentage(
                incrementalImportProgress_
                    ? scaledPercent(
                          processed, total, 0, pagedBuildReadingEndPercent)
                    : (total > 0 && processed >= total ? pagedOpenPercent : 0));
        } else {
            setPercentage(
                scaledPercent(processed, total, 0, readingEndPercent));
        }
        importReadingComplete_ = total > 0 && processed >= total;
        if (importReadingComplete_ && deferredRenderProgress_) {
            const RenderLoadProgress deferred = *deferredRenderProgress_;
            deferredRenderProgress_.reset();
            return updateRender(deferred);
        }
        return state_;
    }

    importReadingComplete_ = true;
    if (pagedSource_ && incrementalImportProgress_) {
        if (processed >= total && total > 0) {
            state_.phase = LoadingProgressPhase::Optimizing;
            state_.completed = processed;
            state_.total = total;
            state_.estimated = false;
            setPercentage(pagedBuildEndPercent);
            return state_;
        }
        if (state_.phase != LoadingProgressPhase::Optimizing ||
            !state_.estimated) {
            beginEstimate(LoadingProgressPhase::Optimizing,
                          pagedBuildReadingEndPercent,
                          pagedBuildEndPercent,
                          optimizingEstimate);
        }
        state_.completed = processed;
        state_.total = total;
        return state_;
    }
    if (state_.phase != LoadingProgressPhase::Optimizing || !state_.estimated) {
        beginEstimate(LoadingProgressPhase::Optimizing,
                      optimizingStartPercent,
                      optimizingEndPercent,
                      optimizingEstimate);
    }
    state_.completed = processed;
    state_.total = total;
    return state_;
}

LoadingProgressState
LoadingProgressModel::updateRender(const RenderLoadProgress progress) noexcept
{
    if (progress.stage != RenderLoadStage::DisplayReady &&
        !importReadingComplete_) {
        // A scene shell makes the renderer available before PDAL has begun or
        // completed streaming. Even a rendered preview must not replace real
        // read progress; MainWindow records its timing independently.
        deferredRenderProgress_ = progress;
        return state_;
    }

    deferredRenderProgress_.reset();

    const int pagedStartPercent =
        incrementalImportProgress_ ? pagedBuildEndPercent : pagedOpenPercent;

    if (progress.stage == RenderLoadStage::Preparing) {
        if (pagedSource_) {
            state_.phase = LoadingProgressPhase::PreparingRenderer;
            state_.completed = progress.completed;
            state_.total = progress.total;
            state_.estimated = false;
            setPercentage(pagedStartPercent);
            return state_;
        }
        if (state_.phase != LoadingProgressPhase::PreparingRenderer ||
            !state_.estimated) {
            beginEstimate(LoadingProgressPhase::PreparingRenderer,
                          rendererStartPercent,
                          rendererEndPercent,
                          rendererEstimate);
        }
        state_.completed = progress.completed;
        state_.total = progress.total;
        return state_;
    }

    if (progress.stage == RenderLoadStage::Uploading) {
        state_.phase = LoadingProgressPhase::Uploading;
        state_.completed = progress.completed;
        state_.total = progress.total;
        state_.estimated = false;
        setPercentage(scaledPercent(progress.completed,
                                    progress.total,
                                    pagedSource_ ? pagedStartPercent
                                                 : rendererStartPercent,
                                    rendererUploadEndPercent));
        return state_;
    }

    if (progress.stage == RenderLoadStage::FirstFrameReady) {
        state_.phase = LoadingProgressPhase::FirstFrameReady;
        state_.completed = progress.completed;
        state_.total = progress.total;
        state_.estimated = false;
        setPercentage(std::max(state_.percentage,
                               pagedSource_ ? pagedStartPercent
                                            : firstFrameReadyPercent));
        return state_;
    }

    state_.phase = LoadingProgressPhase::DisplayReady;
    state_.completed = progress.completed;
    state_.total = progress.total;
    state_.estimated = false;
    setPercentage(displayReadyPercent);
    return state_;
}

LoadingProgressState
LoadingProgressModel::advance(const std::chrono::milliseconds elapsed) noexcept
{
    if (!state_.estimated) {
        return state_;
    }

    const int cappedEnd =
        std::max(estimateStartPercent_, estimateEndPercent_ - 1);
    const auto boundedElapsed = std::clamp(
        elapsed, std::chrono::milliseconds::zero(), estimateDuration_);
    const double fraction = static_cast<double>(boundedElapsed.count()) /
                            static_cast<double>(estimateDuration_.count());
    setPercentage(
        estimateStartPercent_ +
        static_cast<int>((cappedEnd - estimateStartPercent_) * fraction));
    return state_;
}

void LoadingProgressModel::beginEstimate(
    const LoadingProgressPhase phase,
    const int startPercent,
    const int endPercent,
    const std::chrono::milliseconds estimate) noexcept
{
    state_.phase = phase;
    state_.completed = 0;
    state_.total = 0;
    state_.estimated = true;
    estimateStartPercent_ = std::max(state_.percentage, startPercent);
    estimateEndPercent_ = std::max(estimateStartPercent_, endPercent);
    estimateDuration_ = std::max(estimate, std::chrono::milliseconds(1));
    setPercentage(estimateStartPercent_);
}

void LoadingProgressModel::setPercentage(const int percentage) noexcept
{
    state_.percentage =
        std::max(state_.percentage, std::clamp(percentage, 0, 100));
}

} // namespace pci
