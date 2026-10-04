#include <pci/rendering/FramePickGuard.h>
#include <pci/runtime/point/PointDatasetRuntime.h>

namespace pci {

FramePickGuard FramePickGuard::capture(const FrameContext &context)
{
    FramePickGuard result;
    result.session_ = context.session;
    if (!context.document || !context.runtime) {
        return result;
    }
    result.document_ = context.document->generation;
    result.revision_ = context.document->revision;
    for (const auto &layer : context.document->layers) {
        const auto *point =
            std::get_if<PointCloudLayerSnapshotState>(&layer.payload);
        if (!point) {
            continue;
        }
        const auto runtime = context.runtime->point(point->descriptor.sourceId,
                                                    layer.bindingGeneration);
        result.targets_.push_back({
            .runtime = {.layerId = layer.id,
                        .sourceId = point->descriptor.sourceId,
                        .bindingGeneration = layer.bindingGeneration,
                        .contentRevision =
                            runtime ? runtime->residencyContentRevision() : 0},
            .colorGeneration = point->colorGeneration,
        });
    }
    return result;
}

bool FramePickGuard::valid(const FrameContext &context) const
{
    if (!context.document || !context.runtime || context.session != session_ ||
        context.document->generation != document_ ||
        context.document->revision != revision_) {
        return false;
    }
    for (const auto &target : targets_) {
        const auto layer = context.document->layer(target.runtime.layerId);
        const auto runtime = context.runtime->point(
            target.runtime.sourceId, target.runtime.bindingGeneration);
        if (!layer ||
            layer->bindingGeneration != target.runtime.bindingGeneration ||
            layer->descriptor.sourceId != target.runtime.sourceId ||
            layer->colorGeneration != target.colorGeneration || !runtime ||
            runtime->residencyContentRevision() !=
                target.runtime.contentRevision) {
            return false;
        }
    }
    return true;
}

} // namespace pci
