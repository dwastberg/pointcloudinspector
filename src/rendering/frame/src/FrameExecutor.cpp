#include <pci/rendering/FrameExecutor.h>

#include <pci/runtime/point/PointDatasetRuntime.h>

#include <algorithm>

namespace pci {
namespace {
bool sameContext(const FrameContext &left, const FrameContext &right)
{
    return left.session == right.session && left.document == right.document &&
           left.runtime == right.runtime;
}
} // namespace

void FrameExecutor::protectPoints(
    const FrameContext &context,
    const std::span<const PointFrameTrimRequest> requests)
{
    auto &keys = protectionScratch_;
    keys.clear();
    for (const auto &request : requests) {
        auto runtime = context.runtime->point(request.target.sourceId,
                                              request.target.bindingGeneration);
        if (!runtime || runtime->residencyContentRevision() !=
                            request.target.contentRevision) {
            continue;
        }
        for (const auto node : request.protectedNodes) {
            if (keys.size() == keys.capacity())
                ++scratchGrowths_;
            keys.push_back({request.target.sourceId, node});
        }
    }
    context.runtime->pointResidency()->cache()->setFrameProtection(keys);
}

void FrameExecutor::drain(const FrameContext &context)
{
    if (!context.document || !context.runtime) {
        return;
    }
    const auto budget =
        context.runtime->pointResidency()->memoryBudget()->byteBudget();
    if (scratchSession_ != context.session ||
        scratchDocument_ != context.document->generation ||
        budget < scratchBudget_) {
        decltype(protectionScratch_){}.swap(protectionScratch_);
        decltype(requestScratch_){}.swap(requestScratch_);
    }
    scratchSession_ = context.session;
    scratchDocument_ = context.document->generation;
    scratchBudget_ = budget;
    protectPoints(context, previousPoints_);
    context.runtime->pointResidency()->syncCacheBudget();
    // Round-robin layers prevent a continuously completing source from
    // consuming the entire frame's admission allowance on every frame.
    const auto &layers = context.document->layers;
    std::size_t remaining = PointDecodeQueue::capacity;
    for (std::size_t i = 0; i < layers.size() && remaining > 0; ++i) {
        const auto &layer = layers[(drainCursor_ + i) % layers.size()];
        if (const auto *point =
                std::get_if<PointCloudLayerSnapshotState>(&layer.payload)) {
            if (auto runtime = context.runtime->point(
                    point->descriptor.sourceId, layer.bindingGeneration)) {
                remaining -= runtime->drainDecodeCompletions(remaining);
            }
        }
    }
    if (!layers.empty()) {
        drainCursor_ = (drainCursor_ + 1) % layers.size();
    }
    context.runtime->pointResidency()->syncCacheBudget();
    if (indexedDocument_.lock() != context.document ||
        indexedRuntime_.lock() != context.runtime ||
        indexedRevision_ != context.document->revision ||
        indexedSession_ != context.session) {
        ++rasterIdentityBuilds_;
        std::unordered_map<RasterSourceId,
                           std::pair<BindingGeneration, std::uint64_t>>
            live;
        for (const auto &layer : context.document->layers) {
            if (const auto *raster =
                    std::get_if<RasterLayerSnapshotState>(&layer.payload)) {
                live.emplace(raster->descriptor.sourceId,
                             std::pair{layer.bindingGeneration,
                                       raster->renderGeneration});
            }
        }
        for (const auto &[source, identity] : rasterSources_) {
            const auto found = live.find(source);
            if (found == live.end() || found->second != identity) {
                rasters_.releaseSource(source);
            }
        }
        std::erase_if(previousRasters_, [&live](const auto &key) {
            const auto found = live.find(key.sourceId);
            return found == live.end() ||
                   found->second.second != key.renderGeneration;
        });
        rasterSources_ = std::move(live);
        indexedDocument_ = context.document;
        indexedRuntime_ = context.runtime;
        indexedRevision_ = context.document->revision;
        indexedSession_ = context.session;
    }
    static_cast<void>(rasters_.drainCompletions(previousRasters_));
}

void FrameExecutor::run(const FrameBackend &backend)
{
    struct ScratchScope {
        FrameExecutor &owner;
        ~ScratchScope()
        {
            owner.finishScratch();
        }
    } scratchScope{*this};
    const auto context = backend.context();
    drain(context);
    auto input = backend.capture();
    const auto pointFrame = points_.plan(std::move(input.points));
    const auto rasterFrame = rasterPlanner_.buildPlan(input.rasters);
    const PointFrameExecutionContext execution{
        .sessionGeneration = context.session,
        .documentGeneration = context.document ? context.document->generation
                                               : DocumentGeneration{},
        .documentRevision = context.document ? context.document->revision : 0,
        .runtime = context.runtime,
    };
    if (!sameContext(context, backend.context()) || !context.document ||
        !context.runtime || !PointFrameExecutor::valid(pointFrame, execution)) {
        points_.clear();
        backend.complete(true);
        return;
    }
    for (const auto &request : rasterFrame.requests) {
        if (!context.runtime->raster(request.sourceId,
                                     request.bindingGeneration)) {
            backend.complete(true);
            return;
        }
    }
    protectPoints(context, pointFrame.plan->trimRequests);
    backend.protect(*pointFrame.plan, rasterFrame);
    if (pointEffects_.execute(pointFrame, execution) !=
        PointFrameExecutionOutcome::Applied) {
        backend.complete(true);
        return;
    }
    auto &batches = requestScratch_;
    batches.clear();
    if (batches.capacity() < rasterFrame.requests.size())
        ++scratchGrowths_;
    batches.reserve(rasterFrame.requests.size());
    for (const auto &request : rasterFrame.requests) {
        batches.push_back(request.asBatch(context.runtime->raster(
            request.sourceId, request.bindingGeneration)));
    }
    rasters_.reconcile(batches);
    batches
        .clear(); // reconcile copies all values retained by asynchronous work.
    context.runtime->pointResidency()->syncCacheBudget();
    previousPoints_ = pointFrame.plan->trimRequests;
    previousRasters_ = rasterFrame.protectedTiles;
    backend.submit(pointFrame, rasterFrame);
    bool pending = rasters_.hasPendingCompletions();
    for (const auto &layer : context.document->layers) {
        if (const auto *point =
                std::get_if<PointCloudLayerSnapshotState>(&layer.payload)) {
            if (auto runtime = context.runtime->point(
                    point->descriptor.sourceId, layer.bindingGeneration)) {
                pending |= runtime->hasPreparedDecodes();
            }
        }
    }
    backend.complete(pending || pointFrame.plan->requiresContinuation);
}

void FrameExecutor::finishScratch() noexcept
{
    protectionScratch_.clear();
    requestScratch_.clear();
    // Retain only a small, budget-relative amount between synchronous frames.
    const auto limit =
        std::min<std::uint64_t>(1024 * 1024, scratchBudget_ / 64);
    if (protectionScratch_.capacity() * sizeof(DecodedPageKey) > limit / 2)
        decltype(protectionScratch_){}.swap(protectionScratch_);
    if (requestScratch_.capacity() * sizeof(RasterRequestBatch) > limit / 2)
        decltype(requestScratch_){}.swap(requestScratch_);
}

void FrameExecutor::clear()
{
    points_.clear();
    rasterPlanner_.clear();
    decltype(protectionScratch_){}.swap(protectionScratch_);
    decltype(requestScratch_){}.swap(requestScratch_);
    indexedDocument_.reset();
    indexedRuntime_.reset();
    // Keep only identity/key metadata until the next drain filters removed
    // bindings. Point replacement must not drop protection for retained
    // overlays.
}

} // namespace pci
