#pragma once

#include <pci/document/SceneDocumentSnapshot.h>
#include <pci/document/SceneLayer.h>
#include <pci/foundation/SpatialReferenceComparator.h>
#include <pci/pointcloud/PointClassificationFilter.h>
#include <pci/pointcloud/PointColorPolicy.h>
#include <pci/vector/VectorLayerData.h>
#include <pci/vector/VectorLayerStyle.h>

#include <pci/color/PointColorMapCatalog.h>
#include <pci/foundation/Generation.h>
#include <pci/foundation/LayerIdentity.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace pci {

class SceneDocumentTestAccess;

class SceneDocument {
public:
    explicit SceneDocument(PointColorMapCatalogSnapshotPtr colorMaps = {},
                           DocumentGeneration generation = {});

    [[nodiscard]] DocumentGeneration generation() const noexcept;
    void setGeneration(DocumentGeneration generation) noexcept;

    [[nodiscard]] bool hasPointCloudLayers() const noexcept;
    [[nodiscard]] const PointColorMapCatalogSnapshotPtr &
    colorMaps() const noexcept;
    [[nodiscard]] std::size_t layerCount() const noexcept;
    [[nodiscard]] std::size_t vectorLayerCount() const noexcept;
    [[nodiscard]] PointCloudLayerId addLayer(PointDatasetView dataset,
                                             BindingGeneration binding = {});
    [[nodiscard]] PointCloudLayerId insertLayer(PointDatasetView dataset,
                                                std::size_t position,
                                                BindingGeneration binding = {});
    [[nodiscard]] SceneLayerId addVectorLayer(VectorLayerDataPtr data,
                                              bool initiallyVisible = true,
                                              BindingGeneration binding = {});
    [[nodiscard]] SceneLayerId
    addRasterLayer(RasterDatasetDescriptor descriptor,
                   bool initiallyVisible = true,
                   BindingGeneration binding = {});
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
    [[nodiscard]] bool
    setPointLayerAvailability(PointCloudLayerId id,
                              PointDatasetAvailability availability);
    [[nodiscard]] bool setPointLayerAvailabilities(
        std::span<const PointLayerAvailabilityUpdate> updates);
    [[nodiscard]] SceneLayerKind layerKind(SceneLayerId id) const noexcept;
    [[nodiscard]] bool hasAnyLayer() const noexcept;
    [[nodiscard]] std::optional<Bounds3d> sceneBounds() const;
    [[nodiscard]] std::optional<Bounds3d> visibleSceneBounds() const;
    [[nodiscard]] std::optional<Bounds3d> layerBounds(SceneLayerId id) const;
    [[nodiscard]] bool isolateLayer(SceneLayerId id);
    [[nodiscard]] bool setAllLayersVisible(bool visible);
    [[nodiscard]] BindingGeneration lastBindingGeneration() const noexcept;
    [[nodiscard]] bool setLayerBindingGeneration(SceneLayerId id,
                                                 BindingGeneration generation);
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

private:
    friend class SceneDocumentTestAccess;

    void appendSceneLayer(SceneLayer layer);
    void reindexLayerPositions(std::size_t first = 0) noexcept;
    void markPointChanged();
    void markVectorChanged();
    void markRasterChanged();
    [[nodiscard]] BindingGeneration
    allocateBindingGeneration(BindingGeneration requested);
    [[nodiscard]] std::vector<SceneLayer>::iterator
    findSceneLayer(SceneLayerId id);
    [[nodiscard]] std::vector<SceneLayer>::const_iterator
    findSceneLayer(SceneLayerId id) const;

    std::vector<SceneLayer> sceneLayers_;
    SceneLayerIndexMap layerIndices_;
    mutable std::size_t layerLookupInspections_ = 0;
    PointColorMapCatalogSnapshotPtr colorMaps_;
    mutable SceneDocumentSnapshotPtr snapshotCache_;
    DocumentGeneration generation_;
    BindingGeneration lastBindingGeneration_;
    std::uint64_t nextLayerValue_ = 1;
    std::uint64_t revision_ = 0;
    std::uint64_t pointRevision_ = 0;
    std::uint64_t vectorRevision_ = 0;
    std::uint64_t rasterRevision_ = 0;
};

using SceneDocumentPtr = std::shared_ptr<SceneDocument>;

} // namespace pci
