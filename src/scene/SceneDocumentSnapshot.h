#pragma once

#include "scene/SceneDocument.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace pci {

struct SceneDocumentSnapshot {
    std::uint64_t revision = 0;
    std::uint64_t pointRevision = 0;
    std::uint64_t vectorRevision = 0;
    std::vector<SceneLayer> layers;
    std::optional<Bounds3d> bounds;
    std::optional<Bounds3d> visibleBounds;
    std::uint64_t visibleExpectedPointCount = 0;
    std::uint64_t decodedByteBudget = 0;

    [[nodiscard]] bool hasPointCloudLayers() const noexcept;
    [[nodiscard]] bool hasAnyLayer() const noexcept;
    [[nodiscard]] std::size_t layerCount() const noexcept;
    [[nodiscard]] std::size_t vectorLayerCount() const noexcept;
    [[nodiscard]] std::vector<PointCloudLayer> pointLayers() const;
    [[nodiscard]] std::vector<VectorLayer> vectorLayers() const;
    [[nodiscard]] std::vector<SceneLayerId> layerOrder() const;
    [[nodiscard]] std::optional<PointCloudLayer>
    layer(PointCloudLayerId id) const;
    [[nodiscard]] std::optional<VectorLayer> vectorLayer(SceneLayerId id) const;
    [[nodiscard]] std::optional<Bounds3d> layerBounds(SceneLayerId id) const;
};

} // namespace pci
