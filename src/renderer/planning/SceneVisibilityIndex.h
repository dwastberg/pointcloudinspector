#pragma once

#include "renderer/planning/PointCloudSourceBoundsIndex.h"
#include "scene/SceneDocumentSnapshot.h"

#include <cstdint>
#include <vector>

namespace pci {

// Renderer-owned source-bounds cache. The immutable document revision is its
// complete rebuild key, so render planning never reaches back into the mutable
// application document.
class SceneVisibilityIndex final {
public:
    void update(const SceneDocumentSnapshot &snapshot);
    [[nodiscard]] std::vector<PointCloudLayerId> visibleLayersIntersecting(
        const PointCloudSourceBoundsIndex::VisibilityTest &visible) const;

private:
    std::uint64_t revision_ = 0;
    bool initialized_ = false;
    PointCloudSourceBoundsIndex index_;
    std::vector<PointCloudLayerId> unboundedVisibleLayers_;
};

} // namespace pci
