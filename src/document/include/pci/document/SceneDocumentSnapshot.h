#pragma once

#include <pci/document/SceneSnapshotLayer.h>
#include <pci/document/SceneSnapshotLayerView.h>

#include <pci/foundation/Generation.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace pci {

struct SceneDocumentSnapshot;
using SceneDocumentSnapshotPtr = std::shared_ptr<const SceneDocumentSnapshot>;

struct SceneDocumentSnapshot {
    DocumentGeneration generation;
    std::uint64_t revision = 0;
    std::uint64_t pointRevision = 0;
    std::uint64_t vectorRevision = 0;
    std::uint64_t rasterRevision = 0;
    std::vector<SceneSnapshotLayer> layers;
    SceneSnapshotLayerIndexMap layerIndices;
    std::vector<std::size_t> pointLayerIndices;
    std::vector<std::size_t> vectorLayerIndices;
    std::vector<std::size_t> rasterLayerIndices;
    std::optional<Bounds3d> bounds;
    std::optional<Bounds3d> visibleBounds;
    std::uint64_t visibleExpectedPointCount = 0;

    [[nodiscard]] bool hasPointCloudLayers() const noexcept;
    [[nodiscard]] bool hasAnyLayer() const noexcept;
    [[nodiscard]] std::size_t layerCount() const noexcept;
    [[nodiscard]] std::size_t vectorLayerCount() const noexcept;
    [[nodiscard]] std::size_t rasterLayerCount() const noexcept;
    [[nodiscard]] PointCloudLayerSnapshotView pointLayers() const noexcept;
    [[nodiscard]] VectorLayerSnapshotView vectorLayers() const noexcept;
    [[nodiscard]] RasterLayerSnapshotView rasterLayers() const noexcept;
    [[nodiscard]] std::vector<SceneLayerId> layerOrder() const;
    [[nodiscard]] std::optional<PointCloudLayerSnapshot>
    layer(PointCloudLayerId id) const;
    [[nodiscard]] std::optional<VectorLayerSnapshot>
    vectorLayer(SceneLayerId id) const;
    [[nodiscard]] std::optional<RasterLayerSnapshot>
    rasterLayer(SceneLayerId id) const;
    [[nodiscard]] std::optional<Bounds3d> layerBounds(SceneLayerId id) const;
};

} // namespace pci
