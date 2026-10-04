#include <pci/document/SceneDocument.h>

#include <pci/document/SceneDocumentSnapshot.h>
#include <pci/document/SceneLayerBounds.h>
#include <pci/foundation/CheckedArithmetic.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace pci {
namespace {

[[nodiscard]] PointCloudLayerState *pointState(SceneLayer &layer)
{
    return std::get_if<PointCloudLayerState>(&layer.payload);
}

[[nodiscard]] const PointCloudLayerState *pointState(const SceneLayer &layer)
{
    return std::get_if<PointCloudLayerState>(&layer.payload);
}

[[nodiscard]] VectorLayerState *vectorState(SceneLayer &layer)
{
    return std::get_if<VectorLayerState>(&layer.payload);
}

[[nodiscard]] const VectorLayerState *vectorState(const SceneLayer &layer)
{
    return std::get_if<VectorLayerState>(&layer.payload);
}

[[nodiscard]] RasterLayerState *rasterState(SceneLayer &layer)
{
    return std::get_if<RasterLayerState>(&layer.payload);
}

[[nodiscard]] const RasterLayerState *rasterState(const SceneLayer &layer)
{
    return std::get_if<RasterLayerState>(&layer.payload);
}

[[nodiscard]] RasterLayer rasterProjection(const SceneLayer &layer)
{
    const auto &raster = std::get<RasterLayerState>(layer.payload);
    return {.id = layer.id,
            .visible = layer.visible,
            .style = raster.style,
            .elevationStatus = raster.elevationStatus,
            .exactElevationRange = raster.exactElevationRange,
            .elevationFailure = raster.elevationFailure,
            .elevationGeneration = raster.elevationGeneration,
            .renderGeneration = raster.renderGeneration,
            .bindingGeneration = layer.bindingGeneration,
            .descriptor = raster.descriptor};
}

[[nodiscard]] PointCloudLayer pointProjection(const SceneLayer &layer)
{
    const auto &point = std::get<PointCloudLayerState>(layer.payload);
    return {
        .id = layer.id,
        .visible = layer.visible,
        .colorMode = point.colorMode,
        .classificationFilter = point.classificationFilter,
        .rasterColors = point.rasterColors,
        .colorGeneration = point.colorGeneration,
        .bindingGeneration = layer.bindingGeneration,
        .descriptor = point.descriptor,
        .availability = {.bounds = point.availableBounds,
                         .pointCount = point.availablePointCount,
                         .colorizeAvailability = point.colorizeAvailability,
                         .scalarRanges = point.scalarRanges,
                         .presentClassifications = point.presentClassifications,
                         .storage = point.storage}};
}

[[nodiscard]] VectorLayer vectorProjection(const SceneLayer &layer)
{
    const auto &vector = std::get<VectorLayerState>(layer.payload);
    return {.id = layer.id,
            .data = vector.data,
            .visible = layer.visible,
            .style = vector.style,
            .bindingGeneration = layer.bindingGeneration};
}

[[nodiscard]] SceneSnapshotLayer snapshotProjection(const SceneLayer &layer)
{
    if (const auto *point = pointState(layer)) {
        return {
            .id = layer.id,
            .visible = layer.visible,
            .payload =
                PointCloudLayerSnapshotState{
                    .colorMode = point->colorMode,
                    .classificationFilter = point->classificationFilter,
                    .rasterColors = point->rasterColors,
                    .colorGeneration = point->colorGeneration,
                    .descriptor = point->descriptor,
                    .availableBounds = point->availableBounds,
                    .availablePointCount = point->availablePointCount,
                    .colorizeAvailability = point->colorizeAvailability,
                    .scalarRanges = point->scalarRanges,
                    .presentClassifications = point->presentClassifications,
                    .storage = point->storage,
                },
            .bindingGeneration = layer.bindingGeneration,
        };
    }
    if (const auto *vector = vectorState(layer)) {
        return {
            .id = layer.id,
            .visible = layer.visible,
            .payload =
                VectorLayerSnapshotState{
                    .data = vector->data,
                    .style = vector->style,
                },
            .bindingGeneration = layer.bindingGeneration,
        };
    }
    const auto &raster = std::get<RasterLayerState>(layer.payload);
    return {
        .id = layer.id,
        .visible = layer.visible,
        .payload =
            RasterLayerSnapshotState{
                .style = raster.style,
                .elevationStatus = raster.elevationStatus,
                .exactElevationRange = raster.exactElevationRange,
                .elevationFailure = raster.elevationFailure,
                .elevationGeneration = raster.elevationGeneration,
                .renderGeneration = raster.renderGeneration,
                .descriptor = raster.descriptor,
            },
        .bindingGeneration = layer.bindingGeneration,
    };
}

[[nodiscard]] std::vector<SceneSnapshotLayer>
snapshotLayers(const std::vector<SceneLayer> &layers)
{
    std::vector<SceneSnapshotLayer> result;
    result.reserve(layers.size());
    std::ranges::transform(
        layers, std::back_inserter(result), snapshotProjection);
    return result;
}

struct SnapshotTypedIndices {
    std::vector<std::size_t> point;
    std::vector<std::size_t> vector;
    std::vector<std::size_t> raster;
};

[[nodiscard]] SnapshotTypedIndices
snapshotTypedIndices(const std::vector<SceneSnapshotLayer> &layers)
{
    SnapshotTypedIndices result;
    const auto count = [&layers]<typename LayerState>() {
        return static_cast<std::size_t>(
            std::ranges::count_if(layers, [](const SceneSnapshotLayer &layer) {
                return std::holds_alternative<LayerState>(layer.payload);
            }));
    };
    result.point.reserve(count.operator()<PointCloudLayerSnapshotState>());
    result.vector.reserve(count.operator()<VectorLayerSnapshotState>());
    result.raster.reserve(count.operator()<RasterLayerSnapshotState>());
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const auto &payload = layers[index].payload;
        if (std::holds_alternative<PointCloudLayerSnapshotState>(payload)) {
            result.point.push_back(index);
        } else if (std::holds_alternative<VectorLayerSnapshotState>(payload)) {
            result.vector.push_back(index);
        } else {
            result.raster.push_back(index);
        }
    }
    return result;
}

} // namespace

SceneDocument::SceneDocument(PointColorMapCatalogSnapshotPtr colorMaps,
                             const DocumentGeneration generation)
    : colorMaps_(colorMaps ? std::move(colorMaps)
                           : createBuiltInPointColorMapCatalog())
    , generation_(generation)
{
}

DocumentGeneration SceneDocument::generation() const noexcept
{
    return generation_;
}

void SceneDocument::setGeneration(const DocumentGeneration generation) noexcept
{
    if (generation_ == generation) {
        return;
    }
    generation_ = generation;
    snapshotCache_.reset();
}

const PointColorMapCatalogSnapshotPtr &SceneDocument::colorMaps() const noexcept
{
    return colorMaps_;
}

bool SceneDocument::hasPointCloudLayers() const noexcept
{
    return std::ranges::any_of(sceneLayers_, [](const SceneLayer &layer) {
        return pointState(layer) != nullptr;
    });
}

std::size_t SceneDocument::layerCount() const noexcept
{
    return static_cast<std::size_t>(
        std::ranges::count_if(sceneLayers_, [](const SceneLayer &layer) {
            return pointState(layer) != nullptr;
        }));
}

std::size_t SceneDocument::vectorLayerCount() const noexcept
{
    return static_cast<std::size_t>(
        std::ranges::count_if(sceneLayers_, [](const SceneLayer &layer) {
            return vectorState(layer) != nullptr;
        }));
}

PointCloudLayerId SceneDocument::addLayer(PointDatasetView dataset,
                                          const BindingGeneration binding)
{
    if (dataset.descriptor.sourceId == PointCloudSourceId{}) {
        throw std::invalid_argument(
            "point-cloud layers require a source identity");
    }
    if (nextLayerValue_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("point-cloud layer ids are exhausted");
    }
    if (std::ranges::any_of(sceneLayers_, [&dataset](const SceneLayer &layer) {
            const PointCloudLayerState *point = pointState(layer);
            return point &&
                   point->descriptor.sourceId == dataset.descriptor.sourceId;
        })) {
        throw std::invalid_argument(
            "a point-cloud source is already attached to this document");
    }
    const BindingGeneration allocatedBinding =
        allocateBindingGeneration(binding);
    const PointCloudLayerId id{nextLayerValue_};
    const PointColorMode colorMode =
        defaultPointColorMode(*colorMaps_, dataset.descriptor.metadata);
    appendSceneLayer({
        .id = id,
        .visible = true,
        .payload =
            PointCloudLayerState{
                .colorMode = colorMode,
                .classificationFilter = {},
                .rasterColors = std::nullopt,
                .colorGeneration = 0,
                .descriptor = std::move(dataset.descriptor),
                .availableBounds = dataset.availability.bounds,
                .availablePointCount = dataset.availability.pointCount,
                .colorizeAvailability =
                    dataset.availability.colorizeAvailability,
                .scalarRanges = dataset.availability.scalarRanges,
                .presentClassifications =
                    dataset.availability.presentClassifications,
                .storage = dataset.availability.storage,
            },
        .bindingGeneration = allocatedBinding,
    });
    ++nextLayerValue_;
    markPointChanged();
    return id;
}

PointCloudLayerId SceneDocument::insertLayer(PointDatasetView dataset,
                                             const std::size_t position,
                                             const BindingGeneration binding)
{
    std::optional<SceneLayerId> insertionTarget;
    std::size_t pointPosition = 0;
    for (const SceneLayer &layer : sceneLayers_) {
        if (!pointState(layer)) {
            continue;
        }
        if (pointPosition == position) {
            insertionTarget = layer.id;
            break;
        }
        ++pointPosition;
    }

    const PointCloudLayerId id = addLayer(std::move(dataset), binding);
    if (!insertionTarget) {
        return id;
    }
    const auto target = findSceneLayer(*insertionTarget);
    std::rotate(target, sceneLayers_.end() - 1, sceneLayers_.end());
    reindexLayerPositions(
        static_cast<std::size_t>(std::distance(sceneLayers_.begin(), target)));
    markPointChanged();
    return id;
}

SceneLayerId SceneDocument::addVectorLayer(VectorLayerDataPtr data,
                                           const bool initiallyVisible,
                                           const BindingGeneration binding)
{
    if (!data || !data->bounds.valid()) {
        throw std::invalid_argument(
            "vector layers require valid compiled data");
    }
    if (nextLayerValue_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("scene layer ids are exhausted");
    }
    const VectorGeometryKind kind = data->summary.dominantKind();
    const BindingGeneration allocatedBinding =
        allocateBindingGeneration(binding);
    const SceneLayerId id{nextLayerValue_};
    appendSceneLayer({
        .id = id,
        .visible = initiallyVisible,
        .payload =
            VectorLayerState{
                .data = std::move(data),
                .style = defaultVectorLayerStyle(kind),
            },
        .bindingGeneration = allocatedBinding,
    });
    ++nextLayerValue_;
    markVectorChanged();
    return id;
}

SceneLayerId SceneDocument::addRasterLayer(RasterDatasetDescriptor descriptor,
                                           const bool initiallyVisible,
                                           const BindingGeneration binding)
{
    const RasterLayerMetadata &metadata = descriptor.metadata;
    if (descriptor.sourceId == RasterSourceId{}) {
        throw std::invalid_argument("raster layers require a source identity");
    }
    if (metadata.width == 0 || metadata.height == 0 ||
        metadata.levels.empty() ||
        !rasterAffineInvertible(metadata.geoTransform)) {
        throw std::invalid_argument("raster layers require usable metadata");
    }
    // The source identity distinguishes cache ownership, so attaching the same
    // source twice would make two layers share tile-cache entries.
    const bool duplicate = std::ranges::any_of(
        sceneLayers_, [&descriptor](const SceneLayer &layer) {
            const RasterLayerState *raster = rasterState(layer);
            return raster != nullptr &&
                   raster->descriptor.sourceId == descriptor.sourceId;
        });
    if (duplicate) {
        throw std::invalid_argument("raster source is already attached");
    }
    if (nextLayerValue_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("scene layer ids are exhausted");
    }

    RasterLayerStyle style = defaultRasterLayerStyle(metadata);
    const bool elevationAvailable = metadata.elevation.available;
    const bool exactReady =
        elevationAvailable && metadata.elevation.cachedExactRange.has_value();
    const BindingGeneration allocatedBinding =
        allocateBindingGeneration(binding);
    const SceneLayerId id{nextLayerValue_};
    appendSceneLayer({
        .id = id,
        .visible = initiallyVisible,
        .payload =
            RasterLayerState{
                .style = std::move(style),
                .elevationStatus =
                    exactReady ? RasterElevationStatus::Ready
                               : (elevationAvailable
                                      ? RasterElevationStatus::Unknown
                                      : RasterElevationStatus::NotApplicable),
                .exactElevationRange = metadata.elevation.cachedExactRange,
                .elevationFailure = {},
                .elevationGeneration = 0,
                .descriptor = std::move(descriptor),
            },
        .bindingGeneration = allocatedBinding,
    });
    ++nextLayerValue_;
    markRasterChanged();
    return id;
}

std::optional<RasterLayer>
SceneDocument::rasterLayer(const SceneLayerId id) const
{
    const auto found = findSceneLayer(id);
    if (found == sceneLayers_.end() || !rasterState(*found)) {
        return std::nullopt;
    }
    return rasterProjection(*found);
}

std::vector<RasterLayer> SceneDocument::rasterLayers() const
{
    std::vector<RasterLayer> result;
    result.reserve(rasterLayerCount());
    for (const SceneLayer &layer : sceneLayers_) {
        if (rasterState(layer)) {
            result.push_back(rasterProjection(layer));
        }
    }
    return result;
}

std::size_t SceneDocument::rasterLayerCount() const noexcept
{
    return static_cast<std::size_t>(
        std::ranges::count_if(sceneLayers_, [](const SceneLayer &layer) {
            return rasterState(layer) != nullptr;
        }));
}

bool SceneDocument::setRasterLayerStyle(const SceneLayerId id,
                                        RasterLayerStyle style)
{
    const auto found = findSceneLayer(id);
    RasterLayerState *raster =
        found == sceneLayers_.end() ? nullptr : rasterState(*found);
    if (!raster)
        return false;
    style = clampRasterLayerStyle(std::move(style));
    if (raster->style == style) {
        return true;
    }
    // Only a change that alters decoded pixels invalidates cached tiles.
    // Opacity and elevation are shader uniforms, so bumping the generation for
    // them would discard reusable tiles on every slider movement.
    if (rasterDecodeAffectedBy(raster->style, style)) {
        ++raster->renderGeneration;
    }
    raster->style = std::move(style);
    markRasterChanged();
    return true;
}

bool SceneDocument::setRasterElevationState(
    const SceneLayerId id,
    const RasterElevationStatus status,
    std::optional<RasterElevationRange> exactRange,
    std::string failure)
{
    const auto found = findSceneLayer(id);
    RasterLayerState *raster =
        found == sceneLayers_.end() ? nullptr : rasterState(*found);
    if (!raster || !raster->descriptor.metadata.elevation.available) {
        return false;
    }
    if (status == RasterElevationStatus::Ready) {
        if (!exactRange || !std::isfinite(exactRange->minimum) ||
            !std::isfinite(exactRange->maximum)) {
            return false;
        }
        if (exactRange->minimum > exactRange->maximum) {
            std::swap(exactRange->minimum, exactRange->maximum);
        }
        failure.clear();
    } else {
        exactRange.reset();
        if (status != RasterElevationStatus::Failed) {
            failure.clear();
        }
    }
    if (raster->elevationStatus == status &&
        raster->exactElevationRange == exactRange &&
        raster->elevationFailure == failure) {
        return true;
    }
    raster->elevationStatus = status;
    raster->exactElevationRange = exactRange;
    raster->elevationFailure = std::move(failure);
    ++raster->elevationGeneration;
    markRasterChanged();
    return true;
}

std::uint64_t SceneDocument::rasterRevision() const noexcept
{
    return rasterRevision_;
}

std::string SceneDocument::referenceSpatialReferenceWkt() const
{
    for (const SceneLayer &layer : sceneLayers_) {
        if (const PointCloudLayerState *point = pointState(layer)) {
            const std::string &wkt =
                point->descriptor.metadata.spatialReferenceWkt;
            if (!wkt.empty()) {
                return wkt;
            }
        }
    }
    for (const SceneLayer &layer : sceneLayers_) {
        if (const RasterLayerState *raster = rasterState(layer)) {
            const std::string &wkt =
                raster->descriptor.metadata.spatialReferenceWkt;
            if (!wkt.empty()) {
                return wkt;
            }
        }
    }
    for (const SceneLayer &layer : sceneLayers_) {
        if (const VectorLayerState *vector = vectorState(layer)) {
            const std::string &wkt = vector->data->spatialReferenceWkt;
            if (!wkt.empty()) {
                return wkt;
            }
        }
    }
    return {};
}

bool SceneDocument::removeLayer(const PointCloudLayerId id)
{
    const auto found = findSceneLayer(id);
    if (found == sceneLayers_.end()) {
        return false;
    }
    if (PointCloudLayerState *point = pointState(*found)) {
        static_cast<void>(point);
        const std::size_t removedIndex = static_cast<std::size_t>(
            std::distance(sceneLayers_.begin(), found));
        sceneLayers_.erase(found);
        layerIndices_.erase(id);
        reindexLayerPositions(removedIndex);
        markPointChanged();
        return true;
    }
    const bool removedRaster = rasterState(*found) != nullptr;
    if (removedRaster) {
        static_cast<void>(clearRasterColorsForLayer(id));
    }
    const std::size_t removedIndex =
        static_cast<std::size_t>(std::distance(sceneLayers_.begin(), found));
    sceneLayers_.erase(found);
    layerIndices_.erase(id);
    reindexLayerPositions(removedIndex);
    if (removedRaster) {
        markRasterChanged();
    } else {
        markVectorChanged();
    }
    return true;
}

std::optional<PointCloudLayer>
SceneDocument::layer(const PointCloudLayerId id) const
{
    const auto found = findSceneLayer(id);
    if (found == sceneLayers_.end() || !pointState(*found)) {
        return std::nullopt;
    }
    return pointProjection(*found);
}

std::vector<PointCloudLayer> SceneDocument::layers() const
{
    std::vector<PointCloudLayer> result;
    result.reserve(layerCount());
    for (const SceneLayer &layer : sceneLayers_) {
        if (pointState(layer)) {
            result.push_back(pointProjection(layer));
        }
    }
    return result;
}

std::optional<VectorLayer>
SceneDocument::vectorLayer(const SceneLayerId id) const
{
    const auto found = findSceneLayer(id);
    return found == sceneLayers_.end() || !vectorState(*found)
               ? std::nullopt
               : std::optional<VectorLayer>(vectorProjection(*found));
}

std::vector<VectorLayer> SceneDocument::vectorLayers() const
{
    std::vector<VectorLayer> result;
    result.reserve(vectorLayerCount());
    for (const SceneLayer &layer : sceneLayers_) {
        if (vectorState(layer)) {
            result.push_back(vectorProjection(layer));
        }
    }
    return result;
}

const std::vector<SceneLayer> &SceneDocument::sceneLayers() const noexcept
{
    return sceneLayers_;
}

std::vector<SceneLayerId> SceneDocument::layerOrder() const
{
    std::vector<SceneLayerId> result;
    result.reserve(sceneLayers_.size());
    std::ranges::transform(
        sceneLayers_, std::back_inserter(result), &SceneLayer::id);
    return result;
}

bool SceneDocument::setLayerVisible(const PointCloudLayerId id,
                                    const bool visible)
{
    const auto found = findSceneLayer(id);
    if (found == sceneLayers_.end()) {
        return false;
    }
    if (PointCloudLayerState *point = pointState(*found)) {
        if (found->visible != visible) {
            static_cast<void>(point);
            found->visible = visible;
            markPointChanged();
        }
        return true;
    }
    if (found->visible != visible) {
        found->visible = visible;
        if (rasterState(*found)) {
            markRasterChanged();
        } else {
            markVectorChanged();
        }
    }
    return true;
}

bool SceneDocument::setVectorLayerStyle(const SceneLayerId id,
                                        VectorLayerStyle style)
{
    const auto found = findSceneLayer(id);
    VectorLayerState *vector =
        found == sceneLayers_.end() ? nullptr : vectorState(*found);
    if (!vector)
        return false;
    style = clampVectorLayerStyle(style);
    if (vector->style != style) {
        vector->style = style;
        markVectorChanged();
    }
    return true;
}

std::uint64_t SceneDocument::vectorRevision() const noexcept
{
    return vectorRevision_;
}

std::uint64_t SceneDocument::pointRevision() const noexcept
{
    return pointRevision_;
}

SceneLayerKind SceneDocument::layerKind(const SceneLayerId id) const noexcept
{
    const auto found = findSceneLayer(id);
    if (found == sceneLayers_.end())
        return SceneLayerKind::None;
    if (pointState(*found))
        return SceneLayerKind::PointCloud;
    if (vectorState(*found))
        return SceneLayerKind::Vector;
    if (rasterState(*found))
        return SceneLayerKind::Raster;
    return SceneLayerKind::None;
}

bool SceneDocument::hasAnyLayer() const noexcept
{
    return !sceneLayers_.empty();
}

std::optional<Bounds3d> SceneDocument::layerBounds(const SceneLayerId id) const
{
    const auto found = findSceneLayer(id);
    if (found == sceneLayers_.end()) {
        return std::nullopt;
    }
    if (const PointCloudLayerState *point = pointState(*found)) {
        return pointSourceDomainBounds(*point);
    }
    // Each alternative is named explicitly. Treating "not a point cloud" as
    // "therefore a vector" is a bad variant access rather than a missing
    // feature the moment a third payload exists.
    if (const VectorLayerState *vector = vectorState(*found)) {
        return vectorDisplayBounds(*vector);
    }
    if (const RasterLayerState *raster = rasterState(*found)) {
        return rasterDisplayBounds(*raster);
    }
    return std::nullopt;
}

std::optional<Bounds3d> SceneDocument::sceneBounds() const
{
    std::optional<Bounds3d> result;
    const auto add = [&result](const Bounds3d &bounds) {
        if (result) {
            result->extend(bounds);
        } else {
            result = bounds;
        }
    };
    for (const SceneLayer &layer : sceneLayers_) {
        Bounds3d bounds;
        if (const PointCloudLayerState *point = pointState(layer)) {
            bounds = pointSourceDomainBounds(*point);
        } else if (const VectorLayerState *vector = vectorState(layer)) {
            bounds = vectorDisplayBounds(*vector);
        } else if (const RasterLayerState *raster = rasterState(layer)) {
            bounds = rasterDisplayBounds(*raster);
        }
        if (bounds.valid()) {
            add(bounds);
        }
    }
    return result;
}

std::optional<Bounds3d> SceneDocument::visibleSceneBounds() const
{
    std::optional<Bounds3d> result;
    const auto add = [&result](const Bounds3d &bounds) {
        if (result) {
            result->extend(bounds);
        } else {
            result = bounds;
        }
    };
    for (const SceneLayer &layer : sceneLayers_) {
        if (!layer.visible) {
            continue;
        }
        if (const PointCloudLayerState *point = pointState(layer)) {
            const Bounds3d bounds = pointAvailableBounds(*point);
            if (bounds.valid())
                add(bounds);
        } else if (const VectorLayerState *vector = vectorState(layer)) {
            const Bounds3d bounds = vectorDisplayBounds(*vector);
            if (bounds.valid()) {
                add(bounds);
            }
        } else if (const RasterLayerState *raster = rasterState(layer)) {
            const Bounds3d bounds = rasterDisplayBounds(*raster);
            if (bounds.valid()) {
                add(bounds);
            }
        }
    }
    return result;
}

bool SceneDocument::isolateLayer(const SceneLayerId id)
{
    if (layerKind(id) == SceneLayerKind::None)
        return false;
    bool changed = false;
    const std::vector<SceneLayerId> ids = layerOrder();
    for (const SceneLayerId layerId : ids) {
        const auto layer = findSceneLayer(layerId);
        const bool visible = layerId == id;
        changed = changed || layer->visible != visible;
        static_cast<void>(setLayerVisible(layerId, visible));
    }
    return changed;
}

bool SceneDocument::setAllLayersVisible(const bool visible)
{
    bool changed = false;
    const std::vector<SceneLayerId> ids = layerOrder();
    for (const SceneLayerId id : ids) {
        const auto layer = findSceneLayer(id);
        changed = changed || layer->visible != visible;
        static_cast<void>(setLayerVisible(id, visible));
    }
    return changed;
}

BindingGeneration SceneDocument::lastBindingGeneration() const noexcept
{
    return lastBindingGeneration_;
}

bool SceneDocument::setLayerBindingGeneration(
    const SceneLayerId id, const BindingGeneration generation)
{
    const auto found = findSceneLayer(id);
    if (found == sceneLayers_.end() || generation == BindingGeneration{} ||
        generation <= lastBindingGeneration_) {
        return false;
    }
    found->bindingGeneration = generation;
    lastBindingGeneration_ = generation;
    snapshotCache_.reset();
    return true;
}

bool SceneDocument::copyOverlayLayersFrom(const SceneDocument &source)
{
    std::vector<SceneLayer> candidateLayers = sceneLayers_;
    SceneLayerIndexMap candidateIndices = layerIndices_;
    std::uint64_t candidateNextLayerValue = nextLayerValue_;
    BindingGeneration candidateLastBindingGeneration = lastBindingGeneration_;
    bool copiedVector = false;
    bool copiedRaster = false;
    for (const SceneLayer &sourceLayer : source.sceneLayers_) {
        const bool vector = vectorState(sourceLayer) != nullptr;
        const bool raster = rasterState(sourceLayer) != nullptr;
        if (!vector && !raster) {
            continue;
        }
        if (candidateIndices.contains(sourceLayer.id))
            continue;
        // The layer is pushed whole, keeping its id and style. Renderer caches
        // are keyed by that id, so reassigning it would evict and re-upload
        // every overlay on each point-cloud replacement.
        candidateLayers.push_back(sourceLayer);
        const bool inserted =
            candidateIndices
                .emplace(sourceLayer.id, candidateLayers.size() - 1U)
                .second;
        if (!inserted) {
            throw std::logic_error("duplicate scene layer identifier");
        }
        candidateNextLayerValue =
            std::max(candidateNextLayerValue, sourceLayer.id.value() + 1U);
        if (sourceLayer.bindingGeneration > candidateLastBindingGeneration) {
            candidateLastBindingGeneration = sourceLayer.bindingGeneration;
        }
        copiedVector = copiedVector || vector;
        copiedRaster = copiedRaster || raster;
    }
    if (!copiedVector && !copiedRaster) {
        return false;
    }
    sceneLayers_.swap(candidateLayers);
    layerIndices_.swap(candidateIndices);
    nextLayerValue_ = candidateNextLayerValue;
    lastBindingGeneration_ = candidateLastBindingGeneration;
    if (copiedVector)
        markVectorChanged();
    if (copiedRaster)
        markRasterChanged();
    return true;
}

bool SceneDocument::setLayerColorMode(const PointCloudLayerId id,
                                      const PointColorMode colorMode)
{
    const auto found = findSceneLayer(id);
    PointCloudLayerState *point =
        found == sceneLayers_.end() ? nullptr : pointState(*found);
    if (!point || !pointColorModeAvailable(*colorMaps_,
                                           point->descriptor.metadata,
                                           colorMode,
                                           point->rasterColors.has_value())) {
        return false;
    }
    if (point->colorMode != colorMode) {
        point->colorMode = colorMode;
        markPointChanged();
    }
    return true;
}

bool SceneDocument::setLayerRasterColors(const PointCloudLayerId id,
                                         RasterPointColorBinding binding)
{
    const auto found = findSceneLayer(id);
    PointCloudLayerState *point =
        found == sceneLayers_.end() ? nullptr : pointState(*found);
    if (!point || !binding.decode ||
        binding.rasterSourceId == RasterSourceId{}) {
        return false;
    }
    point->rasterColors = std::move(binding);
    ++point->colorGeneration;
    markPointChanged();
    return true;
}

bool SceneDocument::clearLayerRasterColors(const PointCloudLayerId id)
{
    const auto found = findSceneLayer(id);
    PointCloudLayerState *point =
        found == sceneLayers_.end() ? nullptr : pointState(*found);
    if (!point || !point->rasterColors) {
        return false;
    }
    point->rasterColors.reset();
    ++point->colorGeneration;
    markPointChanged();
    return true;
}

std::vector<PointCloudLayerId>
SceneDocument::clearRasterColorsForLayer(const SceneLayerId rasterLayerId)
{
    std::vector<PointCloudLayerId> changed;
    for (SceneLayer &layer : sceneLayers_) {
        PointCloudLayerState *point = pointState(layer);
        if (!point || !point->rasterColors ||
            point->rasterColors->rasterLayerId != rasterLayerId) {
            continue;
        }
        point->rasterColors->rasterLayerId.reset();
        changed.push_back(layer.id);
    }
    if (!changed.empty()) {
        markPointChanged();
    }
    return changed;
}

bool SceneDocument::setLayerClassificationFilter(
    const PointCloudLayerId id, const PointClassificationFilter filter)
{
    const auto found = findSceneLayer(id);
    PointCloudLayerState *point =
        found == sceneLayers_.end() ? nullptr : pointState(*found);
    if (!point || !point->descriptor.metadata.hasClassification) {
        return false;
    }
    if (point->classificationFilter != filter) {
        point->classificationFilter = filter;
        markPointChanged();
    }
    return true;
}

std::uint64_t SceneDocument::revision() const noexcept
{
    return revision_;
}

std::uint64_t SceneDocument::visiblePointCount() const
{
    std::uint64_t total = 0;
    for (const SceneLayer &layer : sceneLayers_) {
        const PointCloudLayerState *point = pointState(layer);
        if (point && layer.visible) {
            total = saturatingAdd(total, point->availablePointCount);
        }
    }
    return total;
}

std::uint64_t SceneDocument::visibleExpectedPointCount() const
{
    std::uint64_t total = 0;
    for (const SceneLayer &layer : sceneLayers_) {
        const PointCloudLayerState *point = pointState(layer);
        if (point && layer.visible) {
            total = saturatingAdd(
                total,
                std::max(point->availablePointCount,
                         point->descriptor.metadata.sourcePointCount));
        }
    }
    return total;
}

std::optional<Bounds3d> SceneDocument::bounds() const
{
    std::optional<Bounds3d> combined;
    for (const SceneLayer &layer : sceneLayers_) {
        const PointCloudLayerState *point = pointState(layer);
        if (!point) {
            continue;
        }
        const Bounds3d current = pointSourceDomainBounds(*point);
        if (!current.valid()) {
            continue;
        }
        if (combined) {
            combined->extend(current);
        } else {
            combined = current;
        }
    }
    return combined;
}

std::optional<Bounds3d> SceneDocument::visibleBounds() const
{
    std::optional<Bounds3d> combined;
    for (const SceneLayer &layer : sceneLayers_) {
        const PointCloudLayerState *point = pointState(layer);
        if (!point || !layer.visible) {
            continue;
        }
        const Bounds3d bounds = pointAvailableBounds(*point);
        if (!bounds.valid()) {
            continue;
        }
        if (combined) {
            combined->extend(bounds);
        } else {
            combined = bounds;
        }
    }
    return combined;
}

BindingGeneration
SceneDocument::allocateBindingGeneration(const BindingGeneration requested)
{
    if (requested != BindingGeneration{}) {
        if (requested <= lastBindingGeneration_) {
            throw std::invalid_argument(
                "binding generations must advance monotonically");
        }
        lastBindingGeneration_ = requested;
        return requested;
    }
    lastBindingGeneration_ = nextGeneration(lastBindingGeneration_);
    return lastBindingGeneration_;
}

SceneDocumentSnapshotPtr SceneDocument::snapshot() const
{
    if (!snapshotCache_) {
        std::vector<SceneSnapshotLayer> layers = snapshotLayers(sceneLayers_);
        SnapshotTypedIndices typedIndices = snapshotTypedIndices(layers);
        snapshotCache_ =
            std::make_shared<SceneDocumentSnapshot>(SceneDocumentSnapshot{
                .generation = generation_,
                .revision = revision_,
                .pointRevision = pointRevision_,
                .vectorRevision = vectorRevision_,
                .rasterRevision = rasterRevision_,
                .layers = std::move(layers),
                .layerIndices = layerIndices_,
                .pointLayerIndices = std::move(typedIndices.point),
                .vectorLayerIndices = std::move(typedIndices.vector),
                .rasterLayerIndices = std::move(typedIndices.raster),
                .bounds = bounds(),
                .visibleBounds = visibleSceneBounds(),
                .visibleExpectedPointCount = visibleExpectedPointCount(),
            });
    }
    return snapshotCache_;
}

bool SceneDocument::setPointLayerAvailability(
    const PointCloudLayerId id, PointDatasetAvailability availability)
{
    const PointLayerAvailabilityUpdate update{
        .layerId = id,
        .availability = availability,
    };
    return setPointLayerAvailabilities({&update, 1});
}

bool SceneDocument::setPointLayerAvailabilities(
    const std::span<const PointLayerAvailabilityUpdate> updates)
{
    std::unordered_set<PointCloudLayerId> uniqueIds;
    uniqueIds.reserve(updates.size());
    for (const PointLayerAvailabilityUpdate &update : updates) {
        const auto found = findSceneLayer(update.layerId);
        if (found == sceneLayers_.end() || !pointState(*found) ||
            !uniqueIds.insert(update.layerId).second) {
            return false;
        }
    }

    bool changed = false;
    for (const PointLayerAvailabilityUpdate &update : updates) {
        PointCloudLayerState &point =
            *pointState(*findSceneLayer(update.layerId));
        const PointDatasetAvailability current{
            .bounds = point.availableBounds,
            .pointCount = point.availablePointCount,
            .colorizeAvailability = point.colorizeAvailability,
            .scalarRanges = point.scalarRanges,
            .presentClassifications = point.presentClassifications,
            .storage = point.storage,
        };
        if (current == update.availability) {
            continue;
        }
        point.availableBounds = update.availability.bounds;
        point.availablePointCount = update.availability.pointCount;
        point.colorizeAvailability = update.availability.colorizeAvailability;
        point.scalarRanges = update.availability.scalarRanges;
        point.presentClassifications =
            update.availability.presentClassifications;
        point.storage = update.availability.storage;
        changed = true;
    }
    if (changed) {
        markPointChanged();
    }
    return true;
}

void SceneDocument::markPointChanged()
{
    ++revision_;
    ++pointRevision_;
    snapshotCache_.reset();
}

void SceneDocument::markVectorChanged()
{
    ++revision_;
    ++vectorRevision_;
    snapshotCache_.reset();
}

void SceneDocument::markRasterChanged()
{
    ++revision_;
    ++rasterRevision_;
    snapshotCache_.reset();
}

void SceneDocument::appendSceneLayer(SceneLayer layer)
{
    sceneLayers_.push_back(std::move(layer));
    try {
        const bool inserted =
            layerIndices_
                .emplace(sceneLayers_.back().id, sceneLayers_.size() - 1U)
                .second;
        if (!inserted) {
            throw std::logic_error("duplicate scene layer identifier");
        }
    } catch (...) {
        sceneLayers_.pop_back();
        throw;
    }
}

void SceneDocument::reindexLayerPositions(const std::size_t first) noexcept
{
    for (std::size_t index = first; index < sceneLayers_.size(); ++index) {
        const auto found = layerIndices_.find(sceneLayers_[index].id);
        if (found == layerIndices_.end()) {
            std::terminate();
        }
        found->second = index;
    }
}

std::vector<SceneLayer>::iterator
SceneDocument::findSceneLayer(const SceneLayerId id)
{
    ++layerLookupInspections_;
    const auto found = layerIndices_.find(id);
    if (found == layerIndices_.end() || found->second >= sceneLayers_.size() ||
        sceneLayers_[found->second].id != id) {
        return sceneLayers_.end();
    }
    return sceneLayers_.begin() + static_cast<std::ptrdiff_t>(found->second);
}

std::vector<SceneLayer>::const_iterator
SceneDocument::findSceneLayer(const SceneLayerId id) const
{
    ++layerLookupInspections_;
    const auto found = layerIndices_.find(id);
    if (found == layerIndices_.end() || found->second >= sceneLayers_.size() ||
        sceneLayers_[found->second].id != id) {
        return sceneLayers_.cend();
    }
    return sceneLayers_.cbegin() + static_cast<std::ptrdiff_t>(found->second);
}

} // namespace pci
