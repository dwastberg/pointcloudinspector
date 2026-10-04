#include <pci/rendering/planning/SceneVisibilityIndex.h>

namespace pci {

void SceneVisibilityIndex::clear()
{
    generation_ = {};
    revision_ = 0;
    initialized_ = false;
    index_.rebuild({});
    unboundedVisibleLayers_.clear();
}

void SceneVisibilityIndex::update(
    const DocumentGeneration documentGeneration,
    const std::uint64_t documentRevision,
    const std::span<const SceneVisibilityLayer> layers)
{
    // A zero revision is used by direct planner fixtures and deliberately
    // disables reuse because it does not identify an immutable publication.
    if (documentGeneration.value() != 0 && documentRevision != 0 &&
        initialized_ && generation_ == documentGeneration &&
        revision_ == documentRevision) {
        return;
    }
    std::vector<PointCloudSourceBoundsEntry> entries;
    entries.reserve(layers.size());
    unboundedVisibleLayers_.clear();
    for (const SceneVisibilityLayer &layer : layers) {
        if (layer.sourceBounds.valid()) {
            entries.push_back({
                .sourceId = layer.layerId.value(),
                .bounds = layer.sourceBounds,
            });
        } else {
            unboundedVisibleLayers_.push_back(layer.layerId);
        }
    }
    index_.rebuild(entries);
    generation_ = documentGeneration;
    revision_ = documentRevision;
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
