#pragma once

#include "scene/SceneDocumentSnapshot.h"

#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pci {

class SceneSnapshotCache {
public:
    using WakeCallback = std::function<void()>;
    using Dispatcher = std::function<bool(std::function<void()>)>;

    struct RefreshResult {
        std::vector<PointCloudLayerId> invalidatedRootPayloads;
    };

    struct LayerChanges {
        std::vector<PointCloudLayerId> added;
        std::vector<PointCloudLayerId> removed;
    };

    SceneSnapshotCache();
    ~SceneSnapshotCache();

    SceneSnapshotCache(const SceneSnapshotCache &) = delete;
    SceneSnapshotCache &operator=(const SceneSnapshotCache &) = delete;

    void setCallbacks(WakeCallback wake, Dispatcher dispatcher);
    void setDocument(SceneDocumentSnapshotPtr document, bool reset);
    void clear() noexcept;

    [[nodiscard]] const SceneDocumentSnapshotPtr &document() const noexcept;
    [[nodiscard]] RefreshResult refresh();
    [[nodiscard]] const PointCloudSceneSnapshot *
    snapshot(PointCloudLayerId layerId) const noexcept;
    [[nodiscard]] LayerChanges
    reconcileVisibleLayers(std::span<const PointCloudLayerId> layerIds);
    [[nodiscard]] bool
    containsVisibleLayer(PointCloudLayerId layerId) const noexcept;

private:
    struct SharedInvalidationState;

    struct CachedSceneSnapshot {
        PointCloudScenePtr scene;
        PointCloudSceneSnapshot snapshot;
    };

    struct SceneSubscription {
        PointCloudScenePtr scene;
        PointCloudSceneInvalidationSubscription subscription;
    };

    void refreshSubscriptions();
    void updateObservedRevisions();

    SceneDocumentSnapshotPtr document_;
    std::unordered_map<PointCloudLayerId, CachedSceneSnapshot> snapshots_;
    std::unordered_map<PointCloudLayerId, SceneSubscription> subscriptions_;
    std::unordered_set<PointCloudLayerId> visibleLayerIds_;
    std::shared_ptr<SharedInvalidationState> invalidationState_;
};

} // namespace pci
