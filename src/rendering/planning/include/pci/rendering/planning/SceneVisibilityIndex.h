#pragma once

#include <pci/rendering/planning/PointCloudSourceBoundsIndex.h>

#include <pci/foundation/Generation.h>
#include <pci/foundation/LayerIdentity.h>

#include <cstdint>
#include <span>
#include <vector>

namespace pci {

struct SceneVisibilityLayer {
    PointCloudLayerId layerId;
    Bounds3d sourceBounds;
};

// Renderer-owned source-bounds cache. The immutable document revision is its
// complete rebuild key, so render planning never reaches back into the mutable
// application document.
class SceneVisibilityIndex final {
public:
    void clear();
    void update(DocumentGeneration documentGeneration,
                std::uint64_t documentRevision,
                std::span<const SceneVisibilityLayer> layers);
    [[nodiscard]] std::vector<PointCloudLayerId> visibleLayersIntersecting(
        const PointCloudSourceBoundsIndex::VisibilityTest &visible) const;

private:
    DocumentGeneration generation_;
    std::uint64_t revision_ = 0;
    bool initialized_ = false;
    PointCloudSourceBoundsIndex index_;
    std::vector<PointCloudLayerId> unboundedVisibleLayers_;
};

} // namespace pci
