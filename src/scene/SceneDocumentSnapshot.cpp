#include "scene/SceneDocumentSnapshot.h"

#include <algorithm>
#include <iterator>

namespace pci {
namespace {

[[nodiscard]] Bounds3d vectorBounds(const VectorLayerState &vector)
{
    Bounds3d result = vector.data->bounds;
    result.minimum[2] += vector.style.zOffset;
    result.maximum[2] += vector.style.zOffset;
    for (std::size_t axis = 0; axis < result.minimum.size(); ++axis) {
        if (result.minimum[axis] == result.maximum[axis]) {
            result.minimum[axis] -= 0.5;
            result.maximum[axis] += 0.5;
        }
    }
    return result;
}

} // namespace

bool SceneDocumentSnapshot::hasPointCloudLayers() const noexcept
{
    return std::ranges::any_of(layers, [](const SceneLayer &layer) {
        return std::holds_alternative<PointCloudLayerState>(layer.payload);
    });
}

bool SceneDocumentSnapshot::hasAnyLayer() const noexcept
{
    return !layers.empty();
}

std::size_t SceneDocumentSnapshot::layerCount() const noexcept
{
    return static_cast<std::size_t>(
        std::ranges::count_if(layers, [](const SceneLayer &layer) {
            return std::holds_alternative<PointCloudLayerState>(layer.payload);
        }));
}

std::size_t SceneDocumentSnapshot::vectorLayerCount() const noexcept
{
    // Counted explicitly rather than derived as "everything that is not a
    // point cloud", which silently miscounts once a third payload exists.
    return static_cast<std::size_t>(
        std::ranges::count_if(layers, [](const SceneLayer &layer) {
            return std::holds_alternative<VectorLayerState>(layer.payload);
        }));
}

std::size_t SceneDocumentSnapshot::rasterLayerCount() const noexcept
{
    return static_cast<std::size_t>(
        std::ranges::count_if(layers, [](const SceneLayer &layer) {
            return std::holds_alternative<RasterLayerState>(layer.payload);
        }));
}

std::vector<RasterLayer> SceneDocumentSnapshot::rasterLayers() const
{
    std::vector<RasterLayer> result;
    result.reserve(rasterLayerCount());
    for (const SceneLayer &layer : layers) {
        if (const auto *raster =
                std::get_if<RasterLayerState>(&layer.payload)) {
            result.push_back({
                .id = layer.id,
                .data = raster->data,
                .visible = layer.visible,
                .style = raster->style,
                .elevationStatus = raster->elevationStatus,
                .exactElevationRange = raster->exactElevationRange,
                .elevationFailure = raster->elevationFailure,
                .elevationGeneration = raster->elevationGeneration,
                .renderGeneration = raster->renderGeneration,
            });
        }
    }
    return result;
}

std::optional<RasterLayer>
SceneDocumentSnapshot::rasterLayer(const SceneLayerId id) const
{
    const auto found = std::ranges::find(layers, id, &SceneLayer::id);
    if (found == layers.end()) {
        return std::nullopt;
    }
    const auto *raster = std::get_if<RasterLayerState>(&found->payload);
    if (!raster) {
        return std::nullopt;
    }
    return RasterLayer{
        .id = found->id,
        .data = raster->data,
        .visible = found->visible,
        .style = raster->style,
        .elevationStatus = raster->elevationStatus,
        .exactElevationRange = raster->exactElevationRange,
        .elevationFailure = raster->elevationFailure,
        .elevationGeneration = raster->elevationGeneration,
        .renderGeneration = raster->renderGeneration,
    };
}

std::vector<PointCloudLayer> SceneDocumentSnapshot::pointLayers() const
{
    std::vector<PointCloudLayer> result;
    result.reserve(layers.size());
    for (const SceneLayer &layer : layers) {
        if (const auto *point =
                std::get_if<PointCloudLayerState>(&layer.payload)) {
            result.push_back({
                .id = layer.id,
                .scene = point->scene,
                .visible = layer.visible,
                .colorMode = point->colorMode,
                .classificationFilter = point->classificationFilter,
                .rasterColors = point->rasterColors,
                .colorGeneration = point->colorGeneration,
            });
        }
    }
    return result;
}

std::vector<VectorLayer> SceneDocumentSnapshot::vectorLayers() const
{
    std::vector<VectorLayer> result;
    result.reserve(layers.size());
    for (const SceneLayer &layer : layers) {
        if (const auto *vector =
                std::get_if<VectorLayerState>(&layer.payload)) {
            result.push_back({
                .id = layer.id,
                .data = vector->data,
                .visible = layer.visible,
                .style = vector->style,
            });
        }
    }
    return result;
}

std::vector<SceneLayerId> SceneDocumentSnapshot::layerOrder() const
{
    std::vector<SceneLayerId> result;
    result.reserve(layers.size());
    std::ranges::transform(layers, std::back_inserter(result), &SceneLayer::id);
    return result;
}

std::optional<PointCloudLayer>
SceneDocumentSnapshot::layer(const PointCloudLayerId id) const
{
    const auto found = std::ranges::find(layers, id, &SceneLayer::id);
    if (found == layers.end()) {
        return std::nullopt;
    }
    const auto *point = std::get_if<PointCloudLayerState>(&found->payload);
    if (!point) {
        return std::nullopt;
    }
    return PointCloudLayer{
        .id = found->id,
        .scene = point->scene,
        .visible = found->visible,
        .colorMode = point->colorMode,
        .classificationFilter = point->classificationFilter,
        .rasterColors = point->rasterColors,
        .colorGeneration = point->colorGeneration,
    };
}

std::optional<VectorLayer>
SceneDocumentSnapshot::vectorLayer(const SceneLayerId id) const
{
    const auto found = std::ranges::find(layers, id, &SceneLayer::id);
    if (found == layers.end()) {
        return std::nullopt;
    }
    const auto *vector = std::get_if<VectorLayerState>(&found->payload);
    if (!vector) {
        return std::nullopt;
    }
    return VectorLayer{
        .id = found->id,
        .data = vector->data,
        .visible = found->visible,
        .style = vector->style,
    };
}

std::optional<Bounds3d>
SceneDocumentSnapshot::layerBounds(const SceneLayerId id) const
{
    const auto found = std::ranges::find(layers, id, &SceneLayer::id);
    if (found == layers.end()) {
        return std::nullopt;
    }
    if (const auto *point =
            std::get_if<PointCloudLayerState>(&found->payload)) {
        const Bounds3d source = point->scene->metadata().sourceBounds;
        return source.valid() ? std::optional<Bounds3d>(source)
                              : std::optional<Bounds3d>(point->scene->bounds());
    }
    // An unconditional std::get here is a bad variant access, not a missing
    // feature, the moment a layer is neither a point cloud nor a vector.
    if (const auto *vector = std::get_if<VectorLayerState>(&found->payload)) {
        return vectorBounds(*vector);
    }
    if (const auto *raster = std::get_if<RasterLayerState>(&found->payload)) {
        return rasterSceneBounds(raster->data->metadata(),
                                 raster->style,
                                 raster->elevationStatus,
                                 raster->exactElevationRange);
    }
    return std::nullopt;
}

} // namespace pci
