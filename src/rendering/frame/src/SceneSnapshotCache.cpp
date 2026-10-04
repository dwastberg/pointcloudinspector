#include <pci/rendering/SceneSnapshotCache.h>

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace pci {

struct SceneSnapshotCache::SharedInvalidationState {
    struct ObservedScene {
        PointDatasetRuntimePtr scene;
        std::uint64_t revision = 0;
    };

    std::mutex mutex;
    std::unordered_map<PointCloudLayerId, ObservedScene> observedScenes;
    std::atomic_bool wakeQueued = false;
    WakeCallback wake;
    Dispatcher dispatcher;
};

SceneSnapshotCache::SceneSnapshotCache()
    : invalidationState_(std::make_shared<SharedInvalidationState>())
{
}

SceneSnapshotCache::~SceneSnapshotCache()
{
    clear();
    invalidationState_.reset();
}

void SceneSnapshotCache::setCallbacks(WakeCallback wake, Dispatcher dispatcher)
{
    const std::scoped_lock lock(invalidationState_->mutex);
    invalidationState_->wake = std::move(wake);
    invalidationState_->dispatcher = std::move(dispatcher);
}

void SceneSnapshotCache::setDocument(SceneDocumentSnapshotPtr document,
                                     SceneRuntimeSnapshotPtr runtime,
                                     const bool reset)
{
    if (!document || !runtime) {
        throw std::invalid_argument(
            "document and runtime snapshots must not be null");
    }
    for (const SceneSnapshotLayer &layer : document->layers) {
        const auto *point =
            std::get_if<PointCloudLayerSnapshotState>(&layer.payload);
        if (point && !runtime->point(point->descriptor.sourceId,
                                     layer.bindingGeneration)) {
            throw std::invalid_argument(
                "document point layer has no matching runtime binding");
        }
    }
    if (reset) {
        clear();
    }
    document_ = std::move(document);
    runtime_ = std::move(runtime);
    refreshSubscriptions();
    updateObservedRevisions();
}

void SceneSnapshotCache::clear() noexcept
{
    subscriptions_.clear();
    snapshots_.clear();
    visibleLayerIds_.clear();
    document_.reset();
    runtime_.reset();
    if (invalidationState_) {
        const std::scoped_lock lock(invalidationState_->mutex);
        invalidationState_->observedScenes.clear();
        invalidationState_->wakeQueued = false;
    }
}

const SceneDocumentSnapshotPtr &SceneSnapshotCache::document() const noexcept
{
    return document_;
}

SceneSnapshotCache::RefreshResult SceneSnapshotCache::refresh()
{
    RefreshResult result;
    if (!document_) {
        return result;
    }

    std::unordered_set<PointCloudLayerId> retained;
    retained.reserve(document_->layers.size());
    for (const SceneSnapshotLayer &layer : document_->layers) {
        const auto *point =
            std::get_if<PointCloudLayerSnapshotState>(&layer.payload);
        if (!point) {
            continue;
        }
        const PointDatasetRuntimePtr scene = pointRuntime(layer);
        if (!scene) {
            continue;
        }
        retained.insert(layer.id);
        const std::uint64_t revision = scene->revision();
        const auto current = snapshots_.find(layer.id);
        if (current != snapshots_.end() && current->second.scene == scene &&
            current->second.colorGeneration != point->colorGeneration) {
            result.invalidatedColors.push_back(layer.id);
        }
        if (current != snapshots_.end() && current->second.scene == scene &&
            current->second.snapshot.revision == revision &&
            current->second.colorGeneration == point->colorGeneration) {
            continue;
        }
        PointDatasetRuntimeSnapshot sceneSnapshot = scene->snapshot();
        if (current != snapshots_.end() && current->second.scene == scene &&
            current->second.snapshot.rootPayloadRevision !=
                sceneSnapshot.rootPayloadRevision) {
            result.invalidatedRootPayloads.push_back(layer.id);
        }
        snapshots_.insert_or_assign(
            layer.id,
            CachedSceneSnapshot{
                .scene = scene,
                .snapshot = std::move(sceneSnapshot),
                .colorGeneration = point->colorGeneration,
            });
    }
    for (auto current = snapshots_.begin(); current != snapshots_.end();) {
        if (!retained.contains(current->first)) {
            current = snapshots_.erase(current);
        } else {
            ++current;
        }
    }
    updateObservedRevisions();
    return result;
}

const PointDatasetRuntimeSnapshot *
SceneSnapshotCache::snapshot(const PointCloudLayerId layerId) const noexcept
{
    const auto found = snapshots_.find(layerId);
    return found == snapshots_.end() ? nullptr : &found->second.snapshot;
}

SceneSnapshotCache::LayerChanges SceneSnapshotCache::reconcileVisibleLayers(
    const std::span<const PointCloudLayerId> layerIds)
{
    std::unordered_set<PointCloudLayerId> current(layerIds.begin(),
                                                  layerIds.end());
    LayerChanges changes;
    changes.added.reserve(current.size());
    changes.removed.reserve(visibleLayerIds_.size());
    for (const PointCloudLayerId layerId : current) {
        if (!visibleLayerIds_.contains(layerId)) {
            changes.added.push_back(layerId);
        }
    }
    for (const PointCloudLayerId layerId : visibleLayerIds_) {
        if (!current.contains(layerId)) {
            changes.removed.push_back(layerId);
        }
    }
    visibleLayerIds_ = std::move(current);
    return changes;
}

bool SceneSnapshotCache::containsVisibleLayer(
    const PointCloudLayerId layerId) const noexcept
{
    return visibleLayerIds_.contains(layerId);
}

void SceneSnapshotCache::refreshSubscriptions()
{
    std::unordered_set<PointCloudLayerId> retained;
    retained.reserve(document_->layers.size());
    for (const SceneSnapshotLayer &layer : document_->layers) {
        const auto *point =
            std::get_if<PointCloudLayerSnapshotState>(&layer.payload);
        if (!point) {
            continue;
        }
        const PointDatasetRuntimePtr scene = pointRuntime(layer);
        if (!scene) {
            continue;
        }
        retained.insert(layer.id);
        const auto current = subscriptions_.find(layer.id);
        if (current != subscriptions_.end() && current->second.scene == scene) {
            continue;
        }
        if (current != subscriptions_.end()) {
            subscriptions_.erase(current);
        }

        const std::weak_ptr<SharedInvalidationState> weakState =
            invalidationState_;
        auto subscription = scene->subscribeInvalidation([weakState] {
            const auto state = weakState.lock();
            if (!state || state->wakeQueued.exchange(true)) {
                return;
            }

            Dispatcher dispatcher;
            {
                const std::scoped_lock lock(state->mutex);
                dispatcher = state->dispatcher;
            }
            if (!dispatcher || !dispatcher([weakState] {
                    const auto delivered = weakState.lock();
                    if (!delivered) {
                        return;
                    }
                    delivered->wakeQueued = false;
                    WakeCallback wake;
                    bool changed = false;
                    {
                        const std::scoped_lock lock(delivered->mutex);
                        for (const auto &[id, observed] :
                             delivered->observedScenes) {
                            static_cast<void>(id);
                            if (observed.scene->revision() !=
                                observed.revision) {
                                changed = true;
                                break;
                            }
                        }
                        wake = delivered->wake;
                    }
                    if (changed && wake) {
                        wake();
                    }
                })) {
                state->wakeQueued = false;
            }
        });
        subscriptions_.emplace(layer.id,
                               SceneSubscription{
                                   .scene = scene,
                                   .subscription = std::move(subscription),
                               });
    }

    for (auto current = subscriptions_.begin();
         current != subscriptions_.end();) {
        if (!retained.contains(current->first)) {
            current = subscriptions_.erase(current);
        } else {
            ++current;
        }
    }
}

void SceneSnapshotCache::updateObservedRevisions()
{
    std::unordered_map<PointCloudLayerId,
                       SharedInvalidationState::ObservedScene>
        observed;
    if (document_) {
        observed.reserve(document_->layers.size());
        for (const SceneSnapshotLayer &layer : document_->layers) {
            const auto *point =
                std::get_if<PointCloudLayerSnapshotState>(&layer.payload);
            if (!point) {
                continue;
            }
            const PointDatasetRuntimePtr scene = pointRuntime(layer);
            if (!scene) {
                continue;
            }
            const auto cached = snapshots_.find(layer.id);
            const bool cacheMatches =
                cached != snapshots_.end() && cached->second.scene == scene;
            observed.emplace(
                layer.id,
                SharedInvalidationState::ObservedScene{
                    .scene = scene,
                    .revision = cacheMatches ? cached->second.snapshot.revision
                                             : scene->revision(),
                });
        }
    }
    const std::scoped_lock lock(invalidationState_->mutex);
    invalidationState_->observedScenes = std::move(observed);
}

PointDatasetRuntimePtr
SceneSnapshotCache::pointRuntime(const SceneSnapshotLayer &layer) const
{
    const auto *point =
        std::get_if<PointCloudLayerSnapshotState>(&layer.payload);
    return point && runtime_ ? runtime_->point(point->descriptor.sourceId,
                                               layer.bindingGeneration)
                             : PointDatasetRuntimePtr{};
}

} // namespace pci
