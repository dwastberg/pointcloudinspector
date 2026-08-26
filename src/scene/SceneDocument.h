#pragma once

#include "foundation/SpatialReferenceComparator.h"
#include "foundation/StrongId.h"
#include "pointcloud/PointClassificationFilter.h"
#include "pointcloud/PointColorMapCatalog.h"
#include "pointcloud/PointColorPolicy.h"
#include "raster/RasterTileSource.h"
#include "scene/DecodedPageCache.h"
#include "scene/HierarchyResidencyCoordinator.h"
#include "scene/PointCloudScene.h"
#include "scene/PointMemoryBudget.h"
#include "tasking/TaskScheduler.h"
#include "vector/VectorLayerData.h"
#include "vector/VectorLayerStyle.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace pci {

struct SceneDocumentSnapshot;
using SceneDocumentSnapshotPtr = std::shared_ptr<const SceneDocumentSnapshot>;

using SceneLayerId = StrongId<struct SceneLayerTag>;
using PointCloudLayerId = SceneLayerId;

enum class SceneLayerKind : std::uint8_t {
    None = 0,
    PointCloud = 1,
    Vector = 2,
    Raster = 3
};

struct RasterPointColorBinding {
    std::optional<SceneLayerId> rasterLayerId;
    RasterSourceId rasterSourceId;
    std::filesystem::path rasterSourcePath;
    std::shared_ptr<const RasterDecodeParameters> decode;
    std::uint64_t rasterRenderGeneration = 0;
    std::uint64_t coloredPoints = 0;
    std::uint64_t uncoloredPoints = 0;
    std::optional<SpatialReferenceRelation> crsRelation;
};

struct PointCloudLayer {
    PointCloudLayerId id;
    PointCloudScenePtr scene;
    bool visible = true;
    PointColorMode colorMode;
    PointClassificationFilter classificationFilter;
    std::optional<RasterPointColorBinding> rasterColors;
    std::uint64_t colorGeneration = 0;
};

struct VectorLayer {
    SceneLayerId id;
    VectorLayerDataPtr data;
    bool visible = true;
    VectorLayerStyle style;
};

struct RasterLayer {
    SceneLayerId id;
    RasterLayerDataPtr data;
    bool visible = true;
    RasterLayerStyle style;
    RasterElevationStatus elevationStatus = RasterElevationStatus::NotApplicable;
    std::optional<RasterElevationRange> exactElevationRange;
    std::string elevationFailure;
    std::uint64_t elevationGeneration = 1;
    // Bumped only when a style change alters decoded pixels, so a stale worker
    // result cannot enter the cache. Camera motion never changes it.
    std::uint64_t renderGeneration = 1;
};

struct PointCloudLayerState {
    PointCloudScenePtr scene;
    PointColorMode colorMode;
    PointClassificationFilter classificationFilter;
    std::optional<RasterPointColorBinding> rasterColors;
    std::uint64_t colorGeneration = 0;
};

struct VectorLayerState {
    VectorLayerDataPtr data;
    VectorLayerStyle style;
};

struct RasterLayerState {
    RasterLayerDataPtr data;
    RasterLayerStyle style;
    RasterElevationStatus elevationStatus = RasterElevationStatus::NotApplicable;
    std::optional<RasterElevationRange> exactElevationRange;
    std::string elevationFailure;
    std::uint64_t elevationGeneration = 1;
    std::uint64_t renderGeneration = 1;
};

struct SceneLayer {
    SceneLayerId id;
    bool visible = true;
    std::variant<PointCloudLayerState, VectorLayerState, RasterLayerState>
        payload;
};

struct SceneDocumentMetrics {
    DecodedCacheMetrics cache;
    PointCloudDataSourceMetrics source;
    HierarchyDecodeAdmissionMetrics decodeAdmission;
    std::uint64_t decodeRequestsQueued = 0;
    std::uint64_t decodeRequestsStarted = 0;
    std::uint64_t decodeRequestsCompleted = 0;
    std::uint64_t decodeRequestsCancelled = 0;
    std::uint64_t decodeRequestsFailed = 0;
    std::uint64_t hierarchicalLayers = 0;
    std::uint64_t retainedFlatBytes = 0;
    std::uint64_t persistentIndexBytes = 0;
    std::uint64_t localPersistentSources = 0;
    std::uint64_t reusedPersistentSources = 0;
    std::uint64_t activeColorTableBytes = 0;
    std::uint64_t flatDisplacedColorBytes = 0;
    std::uint64_t retainedSourceRootBytes = 0;
    std::uint64_t retainedColoredRootBytes = 0;
    PointMemoryBudgetMetrics memoryBudget;
    TaskSchedulerMetrics scheduler;
};

class SceneDocument {
public:
    explicit SceneDocument(
        std::uint64_t decodedByteBudget =
            HierarchyResidencyCoordinator::defaultByteBudget,
        std::size_t maximumConcurrentDecodes =
            HierarchyResidencyCoordinator::defaultMaximumConcurrentDecodes,
        HierarchyDecodeAdmissionPtr decodeAdmission = {},
        PointMemoryBudgetPtr memoryBudget = {},
        PointColorMapCatalogSnapshotPtr colorMaps = {});

    [[nodiscard]] bool hasPointCloudLayers() const noexcept;
    [[nodiscard]] const PointColorMapCatalogSnapshotPtr &
    colorMaps() const noexcept;
    [[nodiscard]] std::size_t layerCount() const noexcept;
    [[nodiscard]] std::size_t vectorLayerCount() const noexcept;
    [[nodiscard]] PointCloudLayerId addLayer(PointCloudScenePtr scene);
    [[nodiscard]] PointCloudLayerId insertLayer(PointCloudScenePtr scene,
                                                std::size_t position);
    [[nodiscard]] SceneLayerId addVectorLayer(VectorLayerDataPtr data,
                                              bool initiallyVisible = true);
    [[nodiscard]] SceneLayerId addRasterLayer(RasterLayerDataPtr data,
                                              bool initiallyVisible = true);
    [[nodiscard]] bool removeLayer(SceneLayerId id);
    [[nodiscard]] std::optional<PointCloudLayer>
    layer(PointCloudLayerId id) const;
    [[nodiscard]] std::vector<PointCloudLayer> layers() const;
    [[nodiscard]] std::optional<VectorLayer> vectorLayer(SceneLayerId id) const;
    [[nodiscard]] std::vector<VectorLayer> vectorLayers() const;
    [[nodiscard]] std::optional<RasterLayer> rasterLayer(SceneLayerId id) const;
    [[nodiscard]] std::vector<RasterLayer> rasterLayers() const;
    [[nodiscard]] std::size_t rasterLayerCount() const noexcept;
    [[nodiscard]] const std::vector<SceneLayer> &sceneLayers() const noexcept;
    [[nodiscard]] std::vector<SceneLayerId> layerOrder() const;
    [[nodiscard]] bool setLayerVisible(SceneLayerId id, bool visible);
    [[nodiscard]] bool setVectorLayerStyle(SceneLayerId id,
                                           VectorLayerStyle style);
    [[nodiscard]] bool setRasterLayerStyle(SceneLayerId id,
                                           RasterLayerStyle style);
    [[nodiscard]] bool setRasterElevationState(
        SceneLayerId id,
        RasterElevationStatus status,
        std::optional<RasterElevationRange> exactRange = std::nullopt,
        std::string failure = {});
    [[nodiscard]] bool setLayerColorMode(PointCloudLayerId id,
                                         PointColorMode colorMode);
    [[nodiscard]] bool
    setLayerClassificationFilter(PointCloudLayerId id,
                                 PointClassificationFilter filter);
    [[nodiscard]] bool setLayerRasterColors(PointCloudLayerId id,
                                            RasterPointColorBinding binding);
    [[nodiscard]] bool clearLayerRasterColors(PointCloudLayerId id);
    [[nodiscard]] std::vector<PointCloudLayerId>
    clearRasterColorsForLayer(SceneLayerId rasterLayerId);
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::uint64_t pointRevision() const noexcept;
    [[nodiscard]] std::uint64_t vectorRevision() const noexcept;
    [[nodiscard]] std::uint64_t rasterRevision() const noexcept;
    // Deterministic precedence: the first point-cloud layer carrying a CRS,
    // then the first raster layer, then the first vector layer. A comparison
    // aid only; the document neither reprojects nor enforces a CRS.
    [[nodiscard]] std::string referenceSpatialReferenceWkt() const;
    [[nodiscard]] SceneDocumentSnapshotPtr snapshot() const;
    [[nodiscard]] SceneLayerKind layerKind(SceneLayerId id) const noexcept;
    [[nodiscard]] bool hasAnyLayer() const noexcept;
    [[nodiscard]] std::optional<Bounds3d> sceneBounds() const;
    [[nodiscard]] std::optional<Bounds3d> visibleSceneBounds() const;
    [[nodiscard]] std::optional<Bounds3d> layerBounds(SceneLayerId id) const;
    [[nodiscard]] bool isolateLayer(SceneLayerId id);
    [[nodiscard]] bool setAllLayersVisible(bool visible);
    // Copies vector and raster layers in their relative document order,
    // preserving each SceneLayerId. Renderer caches are keyed by that id, so
    // reassigning it would evict and re-upload every overlay whenever a batch
    // of point clouds is replaced.
    [[nodiscard]] bool copyOverlayLayersFrom(const SceneDocument &source);
    [[nodiscard]] std::uint64_t visiblePointCount() const;
    [[nodiscard]] std::uint64_t visibleExpectedPointCount() const;
    // Bounds of every loaded layer, including hidden layers. Automatic X/Y/Z
    // color normalization uses this stable document domain.
    [[nodiscard]] std::optional<Bounds3d> bounds() const;
    [[nodiscard]] std::optional<Bounds3d> visibleBounds() const;
    [[nodiscard]] std::uint64_t decodedByteBudget() const noexcept;
    [[nodiscard]] std::uint64_t decodedResidentBytes() const;
    [[nodiscard]] const HierarchyResidencyCoordinatorPtr &
    residencyCoordinator() const noexcept;
    [[nodiscard]] const PointMemoryBudgetPtr &memoryBudget() const noexcept;
    [[nodiscard]] const DecodedPageCachePtr &decodedPageCache() const noexcept;
    [[nodiscard]] const std::shared_ptr<TaskScheduler> &
    hierarchyScheduler() const noexcept;
    [[nodiscard]] SceneDocumentMetrics hierarchyMetrics() const;
    void syncResidencyBudgets();

private:
    void rebalanceHierarchyResidency();
    void fitHierarchyRoots(const PointCloudScenePtr &incoming = {},
                           bool restoreMinimumBudget = false);
    void markPointChanged();
    void markVectorChanged();
    void markRasterChanged();
    [[nodiscard]] std::vector<SceneLayer>::iterator
    findSceneLayer(SceneLayerId id);
    [[nodiscard]] std::vector<SceneLayer>::const_iterator
    findSceneLayer(SceneLayerId id) const;

    std::vector<SceneLayer> sceneLayers_;
    PointMemoryBudgetPtr memoryBudget_;
    PointColorMapCatalogSnapshotPtr colorMaps_;
    HierarchyResidencyCoordinatorPtr residencyCoordinator_;
    DecodedPageCachePtr decodedPageCache_;
    std::shared_ptr<TaskScheduler> hierarchyScheduler_;
    mutable SceneDocumentSnapshotPtr snapshotCache_;
    std::uint64_t nextLayerValue_ = 1;
    std::uint64_t revision_ = 0;
    std::uint64_t pointRevision_ = 0;
    std::uint64_t vectorRevision_ = 0;
    std::uint64_t rasterRevision_ = 0;
};

using SceneDocumentPtr = std::shared_ptr<SceneDocument>;

} // namespace pci
