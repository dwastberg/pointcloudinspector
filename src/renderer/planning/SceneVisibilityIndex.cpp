#include "renderer/planning/SceneVisibilityIndex.h"

namespace pci {

void SceneVisibilityIndex::update(const SceneDocumentSnapshot &snapshot)
{
    if (initialized_ && revision_ == snapshot.revision) {
        return;
    }
    std::vector<PointCloudSourceBoundsEntry> entries;
    entries.reserve(snapshot.layers.size());
    unboundedVisibleLayers_.clear();
    for (const SceneLayer &layer : snapshot.layers) {
        if (!layer.visible ||
            !std::holds_alternative<PointCloudLayerState>(layer.payload)) {
            continue;
        }
        const std::optional<Bounds3d> bounds = snapshot.layerBounds(layer.id);
        if (bounds && bounds->valid()) {
            entries.push_back({
                .sourceId = layer.id.value(),
                .bounds = *bounds,
            });
        } else {
            unboundedVisibleLayers_.push_back(layer.id);
        }
    }
    index_.rebuild(entries);
    revision_ = snapshot.revision;
    initialized_ = true;
}

std::vector<PointCloudLayerId> SceneVisibilityIndex::visibleLayersIntersecting(
    const PointCloudSourceBoundsIndex::VisibilityTest &visible) const
{
    const std::vector<std::uint64_t> values = index_.query(visible);
    std::vector<PointCloudLayerId> result;
    result.reserve(values.size() + unboundedVisibleLayers_.size());
    for (const std::uint64_t value : values) {
        result.emplace_back(value);
    }
    result.insert(result.end(),
                  unboundedVisibleLayers_.begin(),
                  unboundedVisibleLayers_.end());
    return result;
}

} // namespace pci
