#pragma once

#include "pointcloud/PointColorPolicy.h"
#include "scene/PointCloudDataSource.h"
#include "scene/PointCloudScene.h"
#include "scene/SceneDocument.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace pci {

struct FullDetailLayerDescriptor {
    PointCloudLayerId layerId;
    bool hierarchical = false;
    bool loadingComplete = false;
    std::uint64_t sourcePointCount = 0;
    std::uint64_t decodedByteBudget = 0;
    std::optional<PointCloudFullDetailInfo> detail;
    std::function<void(std::span<const PointCloudNodeId>)> setPinnedNodes;
    std::function<PointCloudNodePayloadPtr(PointCloudNodeId)> peekPayload;
    std::function<PointCloudNodePayloadPtr()> rootPayload;
    std::function<Bounds3d(PointCloudNodeId)> nodeBounds;
    std::function<bool()> decodeInFlight;
    std::function<std::string()> hierarchyError;
};

struct FullDetailConfiguration {
    std::uint64_t documentRevision = 0;
    std::uint64_t decodedByteBudget = 0;
    std::uint64_t gpuByteBudget = 0;
    std::vector<FullDetailLayerDescriptor> visibleLayers;
};

struct FullDetailConfigurationChange {
    bool planStarted = false;
    bool planStopped = false;
};

struct FullDetailBlockId {
    PointCloudLayerId layerId;
    PointCloudNodeId nodeId;
    std::uint32_t nodeBlockIndex = 0;

    auto operator<=>(const FullDetailBlockId &) const = default;
};

struct FullDetailBlock {
    FullDetailBlockId id;
    PointBlockPtr block;
    std::uint32_t pointCount = 0;
};

struct FullDetailNodeRequest {
    PointCloudLayerId layerId;
    std::vector<PointCloudNodeId> nodes;
};

struct FullDetailFrameResult {
    std::vector<FullDetailNodeRequest> nodeRequests;
    std::vector<PointCloudNodePayloadPtr> payloadLeases;
    std::vector<FullDetailBlock> uploadRequests;
    std::vector<FullDetailBlockId> protectedBlocks;
    std::vector<FullDetailBlock> drawableBlocks;
    std::uint64_t selectedPoints = 0;
    std::uint64_t visibleBlocks = 0;
    std::uint64_t culledBlocks = 0;
    bool active = false;
    bool requiresContinuation = false;
};

struct FullDetailProgressUpdate {
    PointCloudLayerId layerId;
    std::uint64_t decoded = 0;
    std::uint64_t uploaded = 0;
    std::uint64_t total = 0;
};

struct FullDetailLayerStatus {
    std::uint64_t decodedNodes = 0;
    std::uint64_t totalNodes = 0;
    std::uint64_t pointCount = 0;
    bool decodeInFlight = false;
};

struct FullDetailStatus {
    bool warming = false;
    bool active = false;
    bool decisionPending = false;
    std::uint64_t decodedNodes = 0;
    std::uint64_t totalNodes = 0;
    std::uint64_t decodesInFlight = 0;
};

class FullDetailController {
public:
    static constexpr std::size_t maximumNodeProbesPerFrame = 256;
    static constexpr std::uint8_t maximumRecoveryAttempts = 3;

    FullDetailController() = default;
    ~FullDetailController();

    FullDetailController(const FullDetailController &) = delete;
    FullDetailController &operator=(const FullDetailController &) = delete;

    [[nodiscard]] FullDetailConfigurationChange
    configure(FullDetailConfiguration configuration);
    [[nodiscard]] FullDetailConfigurationChange clear();

    using VisibilityQuery = std::function<bool(const Bounds3d &)>;
    using ResidencyQuery = std::function<bool(const FullDetailBlockId &)>;

    [[nodiscard]] FullDetailFrameResult advance(const VisibilityQuery &visible,
                                                const ResidencyQuery &resident);
    [[nodiscard]] std::vector<FullDetailProgressUpdate>
    progressUpdates(const ResidencyQuery &resident);

    [[nodiscard]] bool hasPlan() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool decisionPending() const noexcept;
    [[nodiscard]] FullDetailStatus status() const;
    [[nodiscard]] std::optional<FullDetailLayerStatus>
    layerStatus(PointCloudLayerId layerId) const;

private:
    struct LayerPlan {
        FullDetailLayerDescriptor descriptor;
        PointCloudFullDetailInfo detail;
        std::unordered_map<PointCloudNodeId,
                           PointCloudNodePayloadPtr,
                           PointCloudNodeIdHash>
            decodedPayloads;
        std::size_t probeCursor = 0;
        std::uint8_t recoveryAttempts = 0;
    };

    struct Plan {
        std::uint64_t documentRevision = 0;
        std::uint64_t decodedByteBudget = 0;
        std::uint64_t gpuByteBudget = 0;
        bool active = false;
        std::vector<LayerPlan> layers;
    };

    struct Progress {
        std::uint64_t decoded = 0;
        std::uint64_t uploaded = 0;

        bool operator==(const Progress &) const = default;
    };

    void resetPlan();

    std::optional<Plan> plan_;
    std::unordered_map<PointCloudLayerId, Progress> publishedProgress_;
    bool decisionPending_ = false;
};

} // namespace pci
