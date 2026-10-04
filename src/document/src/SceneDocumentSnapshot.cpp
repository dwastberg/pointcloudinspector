#include <pci/document/SceneDocumentSnapshot.h>

#include <algorithm>
#include <iterator>

namespace pci {
namespace {

[[nodiscard]] std::vector<SceneSnapshotLayer>::const_iterator
findSceneLayer(const SceneDocumentSnapshot &snapshot, const SceneLayerId id)
{
    if (snapshot.layerIndices.size() == snapshot.layers.size()) {
        const auto indexed = snapshot.layerIndices.find(id);
        if (indexed == snapshot.layerIndices.end() ||
            indexed->second >= snapshot.layers.size() ||
            snapshot.layers[indexed->second].id != id) {
            return snapshot.layers.end();
        }
        return snapshot.layers.begin() +
               static_cast<std::ptrdiff_t>(indexed->second);
    }
    // A few UI test fixtures construct snapshots field-by-field. Keep those
    // fixtures valid while production snapshots always carry the complete
    // immutable index built by SceneDocument.
    return std::ranges::find(snapshot.layers, id, &SceneSnapshotLayer::id);
}

[[nodiscard]] Bounds3d
pointSourceDomainBounds(const PointCloudLayerSnapshotState &point)
{
    const Bounds3d source = point.descriptor.metadata.sourceBounds;
    return source.valid() ? source : point.availableBounds;
}

[[nodiscard]] Bounds3d
vectorDisplayBounds(const VectorLayerSnapshotState &vector)
{
    if (!vector.data) {
        return {};
    }
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

[[nodiscard]] Bounds3d
rasterDisplayBounds(const RasterLayerSnapshotState &raster)
{
    return rasterSceneBounds(raster.descriptor.metadata,
                             raster.style,
                             raster.elevationStatus,
                             raster.exactElevationRange);
}

[[nodiscard]] PointCloudLayerSnapshot
pointSnapshotProjection(const SceneSnapshotLayer &layer)
{
    const auto &point = std::get<PointCloudLayerSnapshotState>(layer.payload);
    return {
        .id = layer.id,
        .visible = layer.visible,
        .colorMode = point.colorMode,
        .classificationFilter = point.classificationFilter,
        .rasterColors = point.rasterColors,
        .colorGeneration = point.colorGeneration,
        .bindingGeneration = layer.bindingGeneration,
        .descriptor = point.descriptor,
        .availableBounds = point.availableBounds,
        .availablePointCount = point.availablePointCount,
        .colorizeAvailability = point.colorizeAvailability,
        .scalarRanges = point.scalarRanges,
        .presentClassifications = point.presentClassifications,
        .storage = point.storage,
    };
}

[[nodiscard]] VectorLayerSnapshot
vectorSnapshotProjection(const SceneSnapshotLayer &layer)
{
    const auto &vector = std::get<VectorLayerSnapshotState>(layer.payload);
    return {
        .id = layer.id,
        .data = vector.data,
        .visible = layer.visible,
        .style = vector.style,
        .bindingGeneration = layer.bindingGeneration,
    };
}

[[nodiscard]] RasterLayerSnapshot
rasterSnapshotProjection(const SceneSnapshotLayer &layer)
{
    const auto &raster = std::get<RasterLayerSnapshotState>(layer.payload);
    return {
        .id = layer.id,
        .visible = layer.visible,
        .style = raster.style,
        .elevationStatus = raster.elevationStatus,
        .exactElevationRange = raster.exactElevationRange,
        .elevationFailure = raster.elevationFailure,
        .elevationGeneration = raster.elevationGeneration,
        .renderGeneration = raster.renderGeneration,
        .bindingGeneration = layer.bindingGeneration,
        .descriptor = raster.descriptor,
    };
}

[[nodiscard]] bool
typedLayerIndicesComplete(const SceneDocumentSnapshot &snapshot) noexcept
{
    return snapshot.pointLayerIndices.size() <= snapshot.layers.size() &&
           snapshot.vectorLayerIndices.size() <= snapshot.layers.size() &&
           snapshot.rasterLayerIndices.size() <= snapshot.layers.size() &&
           snapshot.pointLayerIndices.size() +
                   snapshot.vectorLayerIndices.size() +
                   snapshot.rasterLayerIndices.size() ==
               snapshot.layers.size();
}

} // namespace

bool SceneDocumentSnapshot::hasPointCloudLayers() const noexcept
{
    return !pointLayers().empty();
}

bool SceneDocumentSnapshot::hasAnyLayer() const noexcept
{
    return !layers.empty();
}

std::size_t SceneDocumentSnapshot::layerCount() const noexcept
{
    return pointLayers().size();
}

std::size_t SceneDocumentSnapshot::vectorLayerCount() const noexcept
{
    return vectorLayers().size();
}

std::size_t SceneDocumentSnapshot::rasterLayerCount() const noexcept
{
    return rasterLayers().size();
}

RasterLayerSnapshotView SceneDocumentSnapshot::rasterLayers() const noexcept
{
    return {layers,
            rasterLayerIndices,
            rasterSnapshotProjection,
            typedLayerIndicesComplete(*this)};
}

std::optional<RasterLayerSnapshot>
SceneDocumentSnapshot::rasterLayer(const SceneLayerId id) const
{
    const auto found = findSceneLayer(*this, id);
    if (found == layers.end()) {
        return std::nullopt;
    }
    const auto *raster = std::get_if<RasterLayerSnapshotState>(&found->payload);
    if (!raster) {
        return std::nullopt;
    }
    return RasterLayerSnapshot{
        .id = found->id,
        .visible = found->visible,
        .style = raster->style,
        .elevationStatus = raster->elevationStatus,
        .exactElevationRange = raster->exactElevationRange,
        .elevationFailure = raster->elevationFailure,
        .elevationGeneration = raster->elevationGeneration,
        .renderGeneration = raster->renderGeneration,
        .bindingGeneration = found->bindingGeneration,
        .descriptor = raster->descriptor,
    };
}

PointCloudLayerSnapshotView SceneDocumentSnapshot::pointLayers() const noexcept
{
    return {layers,
            pointLayerIndices,
            pointSnapshotProjection,
            typedLayerIndicesComplete(*this)};
}

VectorLayerSnapshotView SceneDocumentSnapshot::vectorLayers() const noexcept
{
    return {layers,
            vectorLayerIndices,
            vectorSnapshotProjection,
            typedLayerIndicesComplete(*this)};
}

std::vector<SceneLayerId> SceneDocumentSnapshot::layerOrder() const
{
    std::vector<SceneLayerId> result;
    result.reserve(layers.size());
    std::ranges::transform(
        layers, std::back_inserter(result), &SceneSnapshotLayer::id);
    return result;
}

std::optional<PointCloudLayerSnapshot>
SceneDocumentSnapshot::layer(const PointCloudLayerId id) const
{
    const auto found = findSceneLayer(*this, id);
    if (found == layers.end()) {
        return std::nullopt;
    }
    const auto *point =
        std::get_if<PointCloudLayerSnapshotState>(&found->payload);
    if (!point) {
        return std::nullopt;
    }
    return pointSnapshotProjection(*found);
}

std::optional<VectorLayerSnapshot>
SceneDocumentSnapshot::vectorLayer(const SceneLayerId id) const
{
    const auto found = findSceneLayer(*this, id);
    if (found == layers.end()) {
        return std::nullopt;
    }
    const auto *vector = std::get_if<VectorLayerSnapshotState>(&found->payload);
    if (!vector) {
        return std::nullopt;
    }
    return vectorSnapshotProjection(*found);
}

std::optional<Bounds3d>
SceneDocumentSnapshot::layerBounds(const SceneLayerId id) const
{
    const auto found = findSceneLayer(*this, id);
    if (found == layers.end()) {
        return std::nullopt;
    }
    if (const auto *point =
            std::get_if<PointCloudLayerSnapshotState>(&found->payload)) {
        return pointSourceDomainBounds(*point);
    }
    // An unconditional std::get here is a bad variant access, not a missing
    // feature, the moment a layer is neither a point cloud nor a vector.
    if (const auto *vector =
            std::get_if<VectorLayerSnapshotState>(&found->payload)) {
        return vectorDisplayBounds(*vector);
    }
    if (const auto *raster =
            std::get_if<RasterLayerSnapshotState>(&found->payload)) {
        return rasterDisplayBounds(*raster);
    }
    return std::nullopt;
}

} // namespace pci
