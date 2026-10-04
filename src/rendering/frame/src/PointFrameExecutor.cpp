#include <pci/rendering/PointFrameExecutor.h>

#include <pci/runtime/point/PointDatasetRuntime.h>

#include <algorithm>
#include <ranges>

namespace pci {

PointFrameExecutionOutcome
PointFrameExecutor::execute(const PointFrameResult &frame,
                            const PointFrameExecutionContext &context)
{
    if (frame.execution.serial == 0 ||
        frame.execution.serial <= lastExecutionSerial_) {
        return PointFrameExecutionOutcome::DuplicateOrOutOfOrder;
    }
    // Consume the serial before applying effects. If an unexpected runtime
    // exception escapes after partial application, replaying the same frame
    // must not duplicate metrics or scheduling.
    lastExecutionSerial_ = frame.execution.serial;

    if (!valid(frame, context)) {
        return PointFrameExecutionOutcome::Stale;
    }

    const auto runtime = [&context](const PointFrameRuntimeTarget &target) {
        return context.runtime->point(target.sourceId,
                                      target.bindingGeneration);
    };
    const auto &requests = frame.plan->nodeRequests;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto &request = requests[(requestCursor_ + i) % requests.size()];
        runtime(request.target)->requestNodes(request.nodes);
    }
    if (!requests.empty()) {
        requestCursor_ = (requestCursor_ + 1) % requests.size();
    }
    for (const PointFrameDecodedLookupEffect &effect :
         frame.plan->decodedLookupEffects) {
        runtime(effect.target)
            ->applyDecodedLookupEffect(effect.nodeId, effect.resident);
    }
    for (const PointFrameTrimRequest &request : frame.plan->trimRequests) {
        runtime(request.target)->trimDecodedCache(request.protectedNodes);
    }
    return PointFrameExecutionOutcome::Applied;
}

bool PointFrameExecutor::valid(const PointFrameResult &frame,
                               const PointFrameExecutionContext &context)
{
    if (!frame.plan ||
        frame.execution.sessionGeneration != context.sessionGeneration ||
        frame.execution.documentGeneration != context.documentGeneration ||
        frame.execution.documentRevision != context.documentRevision) {
        return false;
    }

    const auto valid = [&context](const auto &effect) {
        return validTarget(effect.target, context.runtime);
    };
    if (!std::ranges::all_of(frame.plan->nodeRequests, valid) ||
        !std::ranges::all_of(frame.plan->decodedLookupEffects, valid) ||
        !std::ranges::all_of(frame.plan->trimRequests, valid)) {
        return false;
    }

    return true;
}

bool PointFrameExecutor::validTarget(const PointFrameRuntimeTarget &target,
                                     const SceneRuntimeSnapshotPtr &runtime)
{
    if (!runtime) {
        return false;
    }
    const PointDatasetRuntimePtr scene =
        runtime->point(target.sourceId, target.bindingGeneration);
    return scene && scene->residencyContentRevision() == target.contentRevision;
}

} // namespace pci
