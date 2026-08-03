#pragma once

#include "renderer/planning/FrameCamera.h"
#include "renderer/planning/FullDetailController.h"
#include "renderer/planning/RenderSelection.h"
#include "renderer/planning/SceneVisibilityIndex.h"
#include "scene/PointCloudSceneSnapshot.h"
#include "scene/SceneDocumentSnapshot.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pci {

struct PointFrameLayer {
    PointCloudLayer layer;
    const PointCloudSceneSnapshot *snapshot = nullptr;
    std::optional<PointScalarRange> colorRange;
};

struct PointFrameBlockKey {
    PointCloudLayerId layerId;
    PointCloudNodeId nodeId;
    std::uint32_t nodeBlockIndex = 0;
    std::uint64_t sceneBlockId = 0;

    auto operator<=>(const PointFrameBlockKey &) const = default;
};

struct PointFrameBlockKeyHash {
    [[nodiscard]] std::size_t
    operator()(const PointFrameBlockKey &key) const noexcept;
};

struct PointFrameSelectedBlock {
    PointCloudLayerId layerId;
    PointColorMode colorMode;
    PointClassificationFilter classificationFilter;
    std::optional<PointScalarRange> colorRange;
    PointFrameBlockKey key;
    PointBlockPtr block;
    std::uint32_t pointCount = 0;
};

struct PointFrameUpload {
    PointFrameBlockKey key;
    PointBlockPtr block;
};

struct PointFrameNodeRequest {
    PointCloudLayerId layerId;
    std::vector<PointCloudNodeId> nodes;
};

struct PointFramePlan {
    std::vector<PointFrameSelectedBlock> blocks;
    std::vector<PointFrameUpload> uploads;
    std::vector<PointFrameBlockKey> protectedGpuBlocks;
    std::vector<PointCloudNodePayloadPtr> decodedLeases;
    std::unordered_set<PointCloudLayerId> outOfFrustumLayerIds;
    std::vector<PointFrameNodeRequest> nodeRequests;
    std::uint64_t selectedPoints = 0;
    std::uint64_t visibleBlocks = 0;
    std::uint64_t culledBlocks = 0;
    std::uint64_t visibleLayerCount = 0;
    std::uint64_t coveredLayerCount = 0;
    bool requiresContinuation = false;
};

struct PointFrameInput {
    SceneDocumentSnapshotPtr document;
    std::vector<PointFrameLayer> layers;
    FrameCamera camera;
    std::uint64_t cameraRevision = 0;
    std::uint64_t framePointBudget = 0;
    std::uint64_t gpuByteBudget = 0;
    std::function<bool(const PointFrameBlockKey &)> resident;
};

struct PointFrameResult {
    std::shared_ptr<const PointFramePlan> plan;
    bool reused = false;
};

struct PointBudgetUpdate {
    std::uint64_t total = 1;
    std::optional<std::uint64_t> current;
};

class PointFrameCoordinator {
public:
    [[nodiscard]] static FlatFramePlan
    planFlatCandidates(std::span<const FlatFrameBlockCandidate> candidates,
                       std::uint64_t gpuByteBudget,
                       std::uint64_t pointBudget);

    [[nodiscard]] FullDetailConfigurationChange
    configureFullDetail(const SceneDocumentSnapshotPtr &document,
                        const std::vector<PointFrameLayer> &layers,
                        std::uint64_t gpuByteBudget);
    [[nodiscard]] FullDetailConfigurationChange clear();

    [[nodiscard]] PointBudgetUpdate
    pointBudgetUpdate(const std::vector<PointFrameLayer> &layers,
                      std::uint64_t gpuByteBudget,
                      std::uint64_t currentPointBudget);

    [[nodiscard]] PointFrameResult plan(PointFrameInput input);
    [[nodiscard]] std::vector<FullDetailProgressUpdate> fullDetailProgress(
        const std::function<bool(const PointFrameBlockKey &)> &resident);
    [[nodiscard]] bool fullDetailPlanned() const noexcept;
    [[nodiscard]] bool fullDetailActive() const noexcept;
    [[nodiscard]] bool fullDetailDecisionPending() const noexcept;
    [[nodiscard]] FullDetailStatus fullDetailStatus() const;
    [[nodiscard]] std::optional<FullDetailLayerStatus>
    fullDetailLayerStatus(PointCloudLayerId layerId) const;

private:
    struct FlatFramePlanKey {
        std::uint64_t cameraRevision = 0;
        std::uint64_t documentRevision = 0;
        std::uint64_t pointBudget = 0;
        int outputWidth = 0;
        int outputHeight = 0;
        std::vector<std::pair<PointCloudLayerId, std::uint64_t>> sceneRevisions;

        bool operator==(const FlatFramePlanKey &) const = default;
    };

    struct FlatBudgetSettlementKey {
        std::uint64_t gpuByteBudget = 0;
        std::vector<std::pair<PointCloudLayerId, std::uint64_t>>
            retainedLayerPoints;

        bool operator==(const FlatBudgetSettlementKey &) const = default;
    };

    [[nodiscard]] PointFramePlan buildPlan(const PointFrameInput &input);
    [[nodiscard]] static FlatFramePlanKey
    flatPlanKey(const PointFrameInput &input);

    FullDetailController fullDetailController_;
    SceneVisibilityIndex visibilityIndex_;
    std::unordered_map<PointCloudLayerId, RenderSelection> hierarchySelections_;
    std::optional<FlatFramePlanKey> cachedFlatPlanKey_;
    std::shared_ptr<const PointFramePlan> cachedFlatPlan_;
    std::optional<FlatBudgetSettlementKey> flatBudgetSettlementKey_;
};

} // namespace pci
