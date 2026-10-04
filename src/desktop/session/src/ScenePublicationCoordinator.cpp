#include "ScenePublicationCoordinator.h"
#include <stdexcept>
namespace pci {
void ScenePublicationCoordinator::commitPoints(
    PointCloudLayerId layer,
    const PointDatasetRuntimePtr &scene,
    std::unique_ptr<PointDatasetPublication> publication)
{
    auto candidate = std::make_shared<SceneDocument>(*context_.document);
    if (!layer.value() || !candidate->setPointLayerAvailability(
                              layer, publication->view().availability)) {
        throw std::invalid_argument("point publication lost its layer");
    }
    DocumentUpdate update{
        .sessionGeneration = context_.session,
        .snapshot = candidate->snapshot(),
        .runtime = context_.runtime.snapshot(),
        .runtimeBudget = {.decodedPointBytes = context_.decodedBytes},
    };
    if (context_.beforeCommit) {
        context_.beforeCommit();
    }
    if (!scene->commitPublication(*publication)) {
        throw std::invalid_argument("point publication became stale");
    }
    context_.document.swap(candidate);
    context_.notify(std::move(update));
    scene->publishInvalidation();
}

bool ScenePublicationCoordinator::commitColors(
    SceneDocumentPtr candidate,
    const PointDatasetRuntimePtr &pointRuntime,
    const PointCloudLayerId layerId,
    std::unique_ptr<PointColorPublication> publication)
{
    if (!candidate->setPointLayerAvailability(
            layerId, publication->view().availability)) {
        throw std::logic_error("Color transaction lost its point layer");
    }
    DocumentUpdate update{
        .sessionGeneration = context_.session,
        .snapshot = candidate->snapshot(),
        .runtime = context_.runtime.snapshot(),
        .runtimeBudget = {.decodedPointBytes = context_.decodedBytes}};
    if (context_.beforeCommit) {
        context_.beforeCommit();
    }
    if (!pointRuntime->prepareColorCommit(*publication)) {
        return false;
    }
    pointRuntime->commitPointColors(*publication);
    context_.document.swap(candidate);
    publication.reset();
    context_.notify(std::move(update));
    pointRuntime->notifyPointColors();
    return true;
}

void ScenePublicationCoordinator::install(SceneDocumentPtr document,
                                          SceneRuntime runtime,
                                          DocumentUpdate update)
{
    update.sessionGeneration = context_.session;
    update.snapshot = document->snapshot();
    update.runtime = runtime.snapshot();
    update.runtimeBudget = {.decodedPointBytes = context_.decodedBytes};
    if (context_.beforeCommit) {
        context_.beforeCommit();
    }
    context_.document.swap(document);
    context_.runtime.swap(runtime);
    context_.notify(std::move(update));
}

} // namespace pci
