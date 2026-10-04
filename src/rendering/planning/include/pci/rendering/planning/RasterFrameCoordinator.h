#pragma once

#include <pci/foundation/Generation.h>
#include <pci/foundation/LayerIdentity.h>
#include <pci/raster/RasterCacheKey.h>
#include <pci/raster/RasterRequestBatch.h>
#include <pci/rendering/planning/RasterLodPlanner.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace pci {

// One document layer paired with the source and decode state resolved from the
// matching immutable runtime snapshot. Metadata/style references are borrowed
// only for buildPlan(); source/decode ownership is copied into the result.
struct RasterFrameLayerInput {
    SceneLayerId layerId;
    RasterSourceId sourceId;
    BindingGeneration bindingGeneration;
    std::uint64_t renderGeneration = 0;
    bool visible = false;
    RasterLodLayerView layer;
    bool sourceAvailable = false;
    std::shared_ptr<const RasterDecodeParameters> decode;
};

using RasterFrameResidencyQuery = std::function<bool(const RasterCacheKey &)>;

struct RasterFrameInput {
    std::span<const RasterFrameLayerInput> layers;
    FrameCamera camera;
    std::size_t colorTileCapacity = 1;
    std::size_t elevationTileCapacity = 1;
    bool surfaceSupported = false;
    RasterFrameResidencyQuery cpuResident;
    RasterFrameResidencyQuery gpuResident;
    RasterFrameResidencyQuery unavailable;
};

struct RasterFrameLayerPlan {
    std::size_t inputIndex = 0;
    RasterTilePayloadProfile profile = RasterTilePayloadProfile::ColorOnly;
    bool surface = false;
    std::vector<RasterTileKey> selected;
    std::vector<RasterTileKey> draw;
};

// Owns its desired keys. asBatch() borrows them only for the immediate
// reconciliation call, matching RasterRequestBatch's lifetime contract.
struct RasterFrameRequestPlan {
    RasterSourceId sourceId;
    BindingGeneration bindingGeneration;
    std::uint64_t renderGeneration = 0;
    std::shared_ptr<const RasterDecodeParameters> decode;
    RasterTilePayloadProfile profile = RasterTilePayloadProfile::ColorOnly;
    std::vector<RasterTileKey> orderedRequests;

    [[nodiscard]] RasterRequestBatch
    asBatch(RasterTileSourcePtr source) const noexcept;
};

struct RasterFrameUploadTarget {
    SceneLayerId layerId;
    bool nearest = false;
};

struct RasterFrameStatistics {
    std::size_t visibleLayers = 0;
    std::size_t selectedTiles = 0;
    std::uint32_t finestLevel = 0;
    std::uint32_t coarsestLevel = 0;
    bool coverageIncomplete = false;
};

struct RasterFramePlan {
    std::vector<RasterFrameLayerPlan> layers;
    std::vector<RasterFrameRequestPlan> requests;
    std::vector<SceneLayerId> retainedLayers;
    std::vector<RasterSourceId> liveSources;
    std::unordered_map<RasterSourceId, RasterFrameUploadTarget> uploadTargets;
    std::vector<RasterCacheKey> protectedTiles;
    std::vector<RasterCacheKey> decodedUploads;
    RasterFrameStatistics statistics;
};

// Stateful only for LOD hysteresis. Source reads, request reconciliation,
// cache admission/trim, and GPU upload remain explicit executor effects.
class RasterFrameCoordinator {
public:
    [[nodiscard]] RasterFramePlan buildPlan(const RasterFrameInput &input);
    void clear() noexcept;

private:
    struct PreviousSelection {
        RasterSourceId sourceId;
        BindingGeneration bindingGeneration;
        std::vector<RasterTileKey> tiles;
    };
    std::unordered_map<SceneLayerId, PreviousSelection> previousSelections_;
};

} // namespace pci
