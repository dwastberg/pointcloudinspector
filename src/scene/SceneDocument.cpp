#include "scene/SceneDocument.h"

#include "foundation/CheckedArithmetic.h"
#include "scene/SceneDocumentSnapshot.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
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
            .data = raster.data,
            .visible = layer.visible,
            .style = raster.style,
            .renderGeneration = raster.renderGeneration};
}

[[nodiscard]] PointCloudLayer pointProjection(const SceneLayer &layer)
{
    const auto &point = std::get<PointCloudLayerState>(layer.payload);
    return {.id = layer.id,
            .scene = point.scene,
            .visible = layer.visible,
            .colorMode = point.colorMode,
            .classificationFilter = point.classificationFilter,
            .rasterColors = point.rasterColors,
            .colorGeneration = point.colorGeneration};
}

[[nodiscard]] VectorLayer vectorProjection(const SceneLayer &layer)
{
    const auto &vector = std::get<VectorLayerState>(layer.payload);
    return {.id = layer.id,
            .data = vector.data,
            .visible = layer.visible,
            .style = vector.style};
}

[[nodiscard]] Bounds3d vectorLayerBounds(const VectorLayer &layer)
{
    Bounds3d result = layer.data->bounds;
    result.minimum[2] += layer.style.zOffset;
    result.maximum[2] += layer.style.zOffset;
    for (std::size_t axis = 0; axis < result.minimum.size(); ++axis) {
        if (result.minimum[axis] == result.maximum[axis]) {
            result.minimum[axis] -= 0.5;
            result.maximum[axis] += 0.5;
        }
    }
    return result;
}

} // namespace

SceneDocument::SceneDocument(const std::uint64_t decodedByteBudget,
                             const std::size_t maximumConcurrentDecodes,
                             HierarchyDecodeAdmissionPtr decodeAdmission,
                             PointMemoryBudgetPtr memoryBudget,
                             PointColorMapCatalogSnapshotPtr colorMaps)
    : memoryBudget_(
          memoryBudget ? std::move(memoryBudget)
                       : std::make_shared<PointMemoryBudget>(decodedByteBudget))
    , colorMaps_(colorMaps ? std::move(colorMaps)
                           : createBuiltInPointColorMapCatalog())
    , residencyCoordinator_(std::make_shared<HierarchyResidencyCoordinator>(
          decodedByteBudget,
          maximumConcurrentDecodes,
          std::move(decodeAdmission),
          memoryBudget_))
    , decodedPageCache_(
          std::make_shared<DecodedPageCache>(memoryBudget_->availableBytes()))
    , hierarchyScheduler_(std::make_shared<TaskScheduler>(
          maximumConcurrentDecodes,
          std::max<std::uint64_t>(
              1,
              std::min<std::uint64_t>(TaskScheduler::defaultActiveByteBudget,
                                      decodedByteBudget))))
{
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

PointCloudLayerId SceneDocument::addLayer(PointCloudScenePtr scene)
{
    if (!scene) {
        throw std::invalid_argument("document layers require a scene");
    }
    if (nextLayerValue_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("point-cloud layer ids are exhausted");
    }
    if (std::ranges::any_of(sceneLayers_, [&scene](const SceneLayer &layer) {
            const PointCloudLayerState *point = pointState(layer);
            return point && point->scene == scene;
        })) {
        throw std::invalid_argument(
            "a point-cloud scene is already attached to this document");
    }
    if (scene->hierarchical()) {
        fitHierarchyRoots(scene);
    }

    const PointCloudLayerId id{nextLayerValue_++};
    const PointColorMode colorMode =
        defaultPointColorMode(*colorMaps_, scene->metadata());
    scene->setDocumentHierarchyResources(
        residencyCoordinator_, decodedPageCache_, hierarchyScheduler_, true);
    try {
        sceneLayers_.push_back({
            .id = id,
            .visible = true,
            .payload =
                PointCloudLayerState{
                    .scene = scene,
                    .colorMode = colorMode,
                    .classificationFilter = {},
                    .rasterColors = std::nullopt,
                    .colorGeneration = 0,
                },
        });
    } catch (...) {
        scene->useStandaloneHierarchyResidency();
        throw;
    }
    rebalanceHierarchyResidency();
    markPointChanged();
    return id;
}

PointCloudLayerId SceneDocument::insertLayer(PointCloudScenePtr scene,
                                             const std::size_t position)
{
    const PointCloudLayerId id = addLayer(std::move(scene));
    const std::vector<PointCloudLayer> pointLayers = layers();
    if (position >= pointLayers.size() - 1U) {
        return id;
    }
    SceneLayer inserted = std::move(sceneLayers_.back());
    sceneLayers_.pop_back();
    sceneLayers_.insert(findSceneLayer(pointLayers[position].id),
                        std::move(inserted));
    markPointChanged();
    return id;
}

SceneLayerId SceneDocument::addVectorLayer(VectorLayerDataPtr data,
                                           const bool initiallyVisible)
{
    if (!data || !data->bounds.valid()) {
        throw std::invalid_argument(
            "vector layers require valid compiled data");
    }
    if (nextLayerValue_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("scene layer ids are exhausted");
    }
    const VectorGeometryKind kind = data->summary.dominantKind();
    const SceneLayerId id{nextLayerValue_++};
    sceneLayers_.push_back({
        .id = id,
        .visible = initiallyVisible,
        .payload =
            VectorLayerState{
                .data = std::move(data),
                .style = defaultVectorLayerStyle(kind),
            },
    });
    markVectorChanged();
    return id;
}

SceneLayerId SceneDocument::addRasterLayer(RasterLayerDataPtr data,
                                           const bool initiallyVisible)
{
    if (!data || !data->source) {
        throw std::invalid_argument("raster layers require an attached source");
    }
    const RasterLayerMetadata &metadata = data->metadata();
    if (metadata.width == 0 || metadata.height == 0 ||
        metadata.levels.empty() ||
        !rasterAffineInvertible(metadata.geoTransform)) {
        throw std::invalid_argument("raster layers require usable metadata");
    }
    // The source identity distinguishes cache ownership, so attaching the same
    // source twice would make two layers share tile-cache entries.
    const bool duplicate =
        std::ranges::any_of(sceneLayers_, [&data](const SceneLayer &layer) {
            const RasterLayerState *raster = rasterState(layer);
            return raster != nullptr &&
                   raster->data->sourceId == data->sourceId;
        });
    if (duplicate) {
        throw std::invalid_argument("raster source is already attached");
    }
    if (nextLayerValue_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("scene layer ids are exhausted");
    }

    RasterLayerStyle style = defaultRasterLayerStyle(metadata);
    const SceneLayerId id{nextLayerValue_++};
    sceneLayers_.push_back({
        .id = id,
        .visible = initiallyVisible,
        .payload =
            RasterLayerState{
                .data = std::move(data),
                .style = std::move(style),
            },
    });
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

std::uint64_t SceneDocument::rasterRevision() const noexcept
{
    return rasterRevision_;
}

std::string SceneDocument::referenceSpatialReferenceWkt() const
{
    for (const SceneLayer &layer : sceneLayers_) {
        if (const PointCloudLayerState *point = pointState(layer)) {
            const std::string &wkt =
                point->scene->metadata().spatialReferenceWkt;
            if (!wkt.empty()) {
                return wkt;
            }
        }
    }
    for (const SceneLayer &layer : sceneLayers_) {
        if (const RasterLayerState *raster = rasterState(layer)) {
            const std::string &wkt =
                raster->data->metadata().spatialReferenceWkt;
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
        PointCloudScenePtr removedScene = point->scene;
        removedScene->requestNodes({});
        removedScene->useStandaloneHierarchyResidency();
        sceneLayers_.erase(found);
        removedScene.reset();
        rebalanceHierarchyResidency();
        markPointChanged();
        return true;
    }
    const bool removedRaster = rasterState(*found) != nullptr;
    if (removedRaster) {
        static_cast<void>(clearRasterColorsForLayer(id));
    }
    sceneLayers_.erase(found);
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
            point->scene->setHierarchyResidencyActive(visible);
            found->visible = visible;
            rebalanceHierarchyResidency();
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
        const Bounds3d source = point->scene->metadata().sourceBounds;
        return source.valid() ? std::optional<Bounds3d>(source)
                              : std::optional<Bounds3d>(point->scene->bounds());
    }
    // Each alternative is named explicitly. Treating "not a point cloud" as
    // "therefore a vector" is a bad variant access rather than a missing
    // feature the moment a third payload exists.
    if (vectorState(*found)) {
        return vectorLayerBounds(vectorProjection(*found));
    }
    if (const RasterLayerState *raster = rasterState(*found)) {
        return rasterSceneBounds(raster->data->metadata(), raster->style);
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
        if (const auto bounds = layerBounds(layer.id);
            bounds && bounds->valid()) {
            add(*bounds);
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
            const Bounds3d bounds = point->scene->bounds();
            if (bounds.valid())
                add(bounds);
        } else if (vectorState(layer)) {
            const VectorLayer projected = vectorProjection(layer);
            if (projected.data->bounds.valid()) {
                add(vectorLayerBounds(projected));
            }
        } else if (const RasterLayerState *raster = rasterState(layer)) {
            const Bounds3d bounds =
                rasterSceneBounds(raster->data->metadata(), raster->style);
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

bool SceneDocument::copyOverlayLayersFrom(const SceneDocument &source)
{
    bool copiedVector = false;
    bool copiedRaster = false;
    for (const SceneLayer &sourceLayer : source.sceneLayers_) {
        const bool vector = vectorState(sourceLayer) != nullptr;
        const bool raster = rasterState(sourceLayer) != nullptr;
        if (!vector && !raster) {
            continue;
        }
        if (findSceneLayer(sourceLayer.id) != sceneLayers_.end())
            continue;
        // The layer is pushed whole, keeping its id and style. Renderer caches
        // are keyed by that id, so reassigning it would evict and re-upload
        // every overlay on each point-cloud replacement.
        sceneLayers_.push_back(sourceLayer);
        nextLayerValue_ =
            std::max(nextLayerValue_, sourceLayer.id.value() + 1U);
        copiedVector = copiedVector || vector;
        copiedRaster = copiedRaster || raster;
    }
    if (copiedVector)
        markVectorChanged();
    if (copiedRaster)
        markRasterChanged();
    return copiedVector || copiedRaster;
}

bool SceneDocument::setLayerColorMode(const PointCloudLayerId id,
                                      const PointColorMode colorMode)
{
    const auto found = findSceneLayer(id);
    PointCloudLayerState *point =
        found == sceneLayers_.end() ? nullptr : pointState(*found);
    if (!point || !pointColorModeAvailable(*colorMaps_,
                                           point->scene->metadata(),
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
    if (!point || !point->scene->metadata().hasClassification) {
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
    for (const PointCloudLayer &layer : layers()) {
        if (layer.visible) {
            total = saturatingAdd(total, layer.scene->totalPointCount());
        }
    }
    return total;
}

std::uint64_t SceneDocument::visibleExpectedPointCount() const
{
    std::uint64_t total = 0;
    for (const PointCloudLayer &layer : layers()) {
        if (layer.visible) {
            total = saturatingAdd(
                total,
                std::max(layer.scene->totalPointCount(),
                         layer.scene->metadata().sourcePointCount));
        }
    }
    return total;
}

std::optional<Bounds3d> SceneDocument::bounds() const
{
    std::optional<Bounds3d> combined;
    for (const PointCloudLayer &layer : layers()) {
        const Bounds3d sourceBounds = layer.scene->metadata().sourceBounds;
        const Bounds3d layerBounds =
            sourceBounds.valid() ? sourceBounds : layer.scene->bounds();
        if (!layerBounds.valid()) {
            continue;
        }
        if (combined) {
            combined->extend(layerBounds);
        } else {
            combined = layerBounds;
        }
    }
    return combined;
}

std::optional<Bounds3d> SceneDocument::visibleBounds() const
{
    std::optional<Bounds3d> combined;
    for (const PointCloudLayer &layer : layers()) {
        if (!layer.visible) {
            continue;
        }
        const Bounds3d bounds = layer.scene->bounds();
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

std::uint64_t SceneDocument::decodedByteBudget() const noexcept
{
    return residencyCoordinator_->byteBudget();
}

std::uint64_t SceneDocument::decodedResidentBytes() const
{
    return decodedPageCache_->residentBytes();
}

const HierarchyResidencyCoordinatorPtr &
SceneDocument::residencyCoordinator() const noexcept
{
    return residencyCoordinator_;
}

const PointMemoryBudgetPtr &SceneDocument::memoryBudget() const noexcept
{
    return memoryBudget_;
}

const DecodedPageCachePtr &SceneDocument::decodedPageCache() const noexcept
{
    return decodedPageCache_;
}

const std::shared_ptr<TaskScheduler> &
SceneDocument::hierarchyScheduler() const noexcept
{
    return hierarchyScheduler_;
}

SceneDocumentMetrics SceneDocument::hierarchyMetrics() const
{
    SceneDocumentMetrics result;
    bool hasSource = false;
    bool fetchedBytesKnown = true;
    for (const PointCloudLayer &layer : layers()) {
        const RasterPointColorMetrics color =
            layer.scene->rasterPointColorMetrics();
        result.activeColorTableBytes = saturatingAdd(
            result.activeColorTableBytes, color.activeColorTableBytes);
        result.flatDisplacedColorBytes = saturatingAdd(
            result.flatDisplacedColorBytes, color.flatDisplacedColorBytes);
        result.retainedSourceRootBytes = saturatingAdd(
            result.retainedSourceRootBytes, color.retainedSourceRootBytes);
        result.retainedColoredRootBytes = saturatingAdd(
            result.retainedColoredRootBytes, color.retainedColoredRootBytes);
        const PointCloudStorageMetrics storage = layer.scene->storageMetrics();
        result.persistentIndexBytes =
            saturatingAdd(result.persistentIndexBytes, storage.persistentBytes);
        if (storage.localPersistent) {
            ++result.localPersistentSources;
            if (storage.reused) {
                ++result.reusedPersistentSources;
            }
        }
        if (!layer.scene->hierarchical()) {
            result.retainedFlatBytes = saturatingAdd(
                result.retainedFlatBytes, layer.scene->decodedResidentBytes());
            continue;
        }
        ++result.hierarchicalLayers;
        const PointCloudSceneMetrics scene = layer.scene->hierarchyMetrics();
        result.source.requests =
            saturatingAdd(result.source.requests, scene.source.requests);
        result.source.completed =
            saturatingAdd(result.source.completed, scene.source.completed);
        result.source.cancelled =
            saturatingAdd(result.source.cancelled, scene.source.cancelled);
        result.source.failed =
            saturatingAdd(result.source.failed, scene.source.failed);
        result.source.estimatedDecodedBytesRequested =
            saturatingAdd(result.source.estimatedDecodedBytesRequested,
                          scene.source.estimatedDecodedBytesRequested);
        result.source.decodedBytesProduced =
            saturatingAdd(result.source.decodedBytesProduced,
                          scene.source.decodedBytesProduced);
        result.source.sourcePointsVisited =
            saturatingAdd(result.source.sourcePointsVisited,
                          scene.source.sourcePointsVisited);
        result.source.decodedPointsProduced =
            saturatingAdd(result.source.decodedPointsProduced,
                          scene.source.decodedPointsProduced);
        result.source.totalQueryNanoseconds =
            saturatingAdd(result.source.totalQueryNanoseconds,
                          scene.source.totalQueryNanoseconds);
        result.source.fetchedBytes = saturatingAdd(result.source.fetchedBytes,
                                                   scene.source.fetchedBytes);
        fetchedBytesKnown = fetchedBytesKnown && scene.source.fetchedBytesKnown;
        hasSource = true;

        result.decodeRequestsQueued = saturatingAdd(result.decodeRequestsQueued,
                                                    scene.decodeRequestsQueued);
        result.decodeRequestsStarted = saturatingAdd(
            result.decodeRequestsStarted, scene.decodeRequestsStarted);
        result.decodeRequestsCompleted = saturatingAdd(
            result.decodeRequestsCompleted, scene.decodeRequestsCompleted);
        result.decodeRequestsCancelled = saturatingAdd(
            result.decodeRequestsCancelled, scene.decodeRequestsCancelled);
        result.decodeRequestsFailed = saturatingAdd(result.decodeRequestsFailed,
                                                    scene.decodeRequestsFailed);
    }
    result.cache = decodedPageCache_->metrics();
    result.source.fetchedBytesKnown = hasSource && fetchedBytesKnown;
    result.decodeAdmission =
        residencyCoordinator_->decodeAdmission()->metrics();
    result.memoryBudget = memoryBudget_->metrics();
    result.scheduler = hierarchyScheduler_->metrics();
    return result;
}

void SceneDocument::syncResidencyBudgets()
{
    rebalanceHierarchyResidency();
    if (snapshotCache_ &&
        snapshotCache_->decodedByteBudget != decodedByteBudget()) {
        snapshotCache_.reset();
    }
}

SceneDocumentSnapshotPtr SceneDocument::snapshot() const
{
    if (!snapshotCache_) {
        snapshotCache_ =
            std::make_shared<SceneDocumentSnapshot>(SceneDocumentSnapshot{
                .revision = revision_,
                .pointRevision = pointRevision_,
                .vectorRevision = vectorRevision_,
                .rasterRevision = rasterRevision_,
                .layers = sceneLayers_,
                .bounds = bounds(),
                .visibleBounds = visibleSceneBounds(),
                .visibleExpectedPointCount = visibleExpectedPointCount(),
                .decodedByteBudget = decodedByteBudget(),
            });
    }
    return snapshotCache_;
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

void SceneDocument::rebalanceHierarchyResidency()
{
    fitHierarchyRoots({}, true);
    decodedPageCache_->setByteBudget(std::max<std::uint64_t>(
        1, residencyCoordinator_->availableResidencyBytes()));
    for (const PointCloudLayer &layer : layers()) {
        layer.scene->syncHierarchyResidencyBudget();
    }
}

void SceneDocument::fitHierarchyRoots(const PointCloudScenePtr &incoming,
                                      const bool restoreMinimumBudget)
{
    std::vector<PointCloudScenePtr> scenes;
    scenes.reserve(layerCount() + (incoming ? 1U : 0U));
    for (const PointCloudLayer &layer : layers()) {
        if (layer.scene->hierarchical()) {
            scenes.push_back(layer.scene);
        }
    }
    if (incoming && incoming->hierarchical()) {
        scenes.push_back(incoming);
    }
    if (scenes.empty()) {
        return;
    }

    std::uint64_t minimumBytes = 0;
    std::vector<std::uint64_t> minimumByScene;
    minimumByScene.reserve(scenes.size());
    for (const PointCloudScenePtr &scene : scenes) {
        const std::uint64_t minimum = scene->minimumRootPayloadBytes();
        minimumByScene.push_back(minimum);
        minimumBytes = saturatingAdd(minimumBytes, minimum);
    }
    std::uint64_t available = memoryBudget_->availableBytes();
    if (minimumBytes > available) {
        if (!restoreMinimumBudget) {
            throw std::length_error(
                "the document CPU budget cannot retain one hierarchy "
                "preview point per source; increase the CPU cache budget "
                "or split the document");
        }
        const std::uint64_t reserved = memoryBudget_->reservedBytes();
        const std::uint64_t required = saturatingAdd(reserved, minimumBytes);
        if (!memoryBudget_->setByteBudget(required)) {
            throw std::length_error(
                "the point-memory budget cannot retain hierarchy roots");
        }
        available = memoryBudget_->availableBytes();
    }

    const std::uint64_t discretionary = available - minimumBytes;
    const std::uint64_t count = scenes.size();
    const std::uint64_t common = discretionary / count;
    const std::uint64_t extra = discretionary % count;
    for (std::size_t index = 0; index < scenes.size(); ++index) {
        const std::uint64_t allocation =
            minimumByScene[index] + common + (index < extra ? 1U : 0U);
        static_cast<void>(scenes[index]->limitRootPayloadBytes(allocation));
    }
}

std::vector<SceneLayer>::iterator
SceneDocument::findSceneLayer(const SceneLayerId id)
{
    return std::ranges::find(sceneLayers_, id, &SceneLayer::id);
}

std::vector<SceneLayer>::const_iterator
SceneDocument::findSceneLayer(const SceneLayerId id) const
{
    return std::ranges::find(sceneLayers_, id, &SceneLayer::id);
}

} // namespace pci
