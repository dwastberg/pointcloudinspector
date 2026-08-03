#pragma once

#include "foundation/Vec3d.h"
#include "renderer/planning/FramePlanner.h"
#include "scene/PointCloudNode.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_set>
#include <vector>

namespace pci {

struct RenderSelectionNodeState {
    PointCloudNode node;
    bool resident = false;
    std::uint64_t residentPointCount = 0;
};

struct RenderSelectionParameters {
    Vec3d eye;
    double verticalFieldOfViewDegrees = 60.0;
    int viewportHeight = 1;
    std::uint64_t pointBudget = 1;
    // Zero uses pointBudget. This independently bounds decode/upload prefetch.
    std::uint64_t requestPointBudget = 0;
    double refinePixelError = 3.0;
    double coarsenPixelError = 2.0;
};

struct RenderSelectionResult {
    std::vector<PointCloudNodeId> drawNodes;
    std::vector<PointCloudNodeId> requestedNodes;
    std::uint64_t selectedPoints = 0;
};

// Flat point clouds have no hierarchy from which to obtain a stable coarse
// representation. Choose a deterministic set of complete upload blocks, then
// distribute the draw budget across those blocks. Camera distance is
// deliberately absent so navigation cannot swap whole spatial regions merely
// because two blocks cross a near-to-far cutoff.
struct StableFlatBlockCandidate {
    std::uint64_t groupId = 0;
    std::uint64_t blockId = 0;
    std::uint64_t pointCount = 0;
    std::uint64_t gpuBytes = 0;
};

struct StableFlatBlockAllocation {
    std::size_t candidateIndex = 0;
    std::uint64_t pointCount = 0;
};

struct FullDetailResidencyCandidate {
    std::uint64_t sourcePointCount = 0;
    std::uint64_t detailPointCount = 0;
    std::uint64_t decodedBytes = 0;
    std::uint64_t decodedByteBudget = 0;
    std::uint64_t gpuBytes = 0;
};

// Full-detail residency reserves one eighth of both budgets for root/parent
// transition payloads, container overhead, and unrelated renderer resources.
// Every visible source must expose its exact finite leaf set and the combined
// working set must fit the document-owned decoded and GPU caches.
[[nodiscard]] bool fullDetailResidencyFits(
    std::span<const FullDetailResidencyCandidate> candidates,
    std::uint64_t documentDecodedByteBudget,
    std::uint64_t gpuByteBudget) noexcept;

// A narrow camera-space cone through a screen-space pick footprint. Block
// bounds are tested conservatively against this volume before any pick draw
// calls are recorded.
struct ScreenPickVolume {
    Vec3d origin;
    Vec3d direction;
    double tangentHalfAngle = 0.0;
    double nearDistance = 0.0;
    double farDistance = 0.0;

    [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] ScreenPickVolume
makeScreenPickVolume(Vec3d origin,
                     Vec3d forward,
                     Vec3d right,
                     Vec3d up,
                     double verticalFieldOfViewDegrees,
                     int viewportWidth,
                     int viewportHeight,
                     double pixelX,
                     double pixelY,
                     double radiusPixels,
                     double nearDistance,
                     double farDistance) noexcept;

[[nodiscard]] bool intersectsScreenPickVolume(const ScreenPickVolume &volume,
                                              const Bounds3d &bounds,
                                              double expansion = 0.0) noexcept;

[[nodiscard]] double screenPickDistance(const ScreenPickVolume &volume,
                                        const Bounds3d &bounds) noexcept;

[[nodiscard]] std::vector<StableFlatBlockAllocation>
planStableFlatBlocks(std::span<const StableFlatBlockCandidate> candidates,
                     std::uint64_t gpuByteBudget,
                     std::uint64_t pointBudget);

class RenderSelection {
public:
    using NodeLookup =
        std::function<RenderSelectionNodeState(PointCloudNodeId)>;
    using VisibilityTest = std::function<bool(const Bounds3d &)>;

    [[nodiscard]] RenderSelectionResult
    select(std::span<const PointCloudNodeId> roots,
           const NodeLookup &lookup,
           const VisibilityTest &visible,
           const RenderSelectionParameters &parameters);
    void reset();

private:
    std::unordered_set<PointCloudNodeId, PointCloudNodeIdHash>
        refinedLastFrame_;
};

} // namespace pci
