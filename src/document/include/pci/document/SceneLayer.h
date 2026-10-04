#pragma once

#include <pci/document/SceneLayerTypes.h>
#include <pci/foundation/Generation.h>
#include <pci/pointcloud/PointClassificationFilter.h>
#include <pci/pointcloud/PointColorPolicy.h>
#include <pci/pointcloud/PointDatasetDescriptor.h>
#include <pci/raster/RasterDatasetDescriptor.h>
#include <pci/vector/VectorLayerData.h>
#include <pci/vector/VectorLayerStyle.h>

#include <pci/foundation/LayerIdentity.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

namespace pci {

struct PointCloudLayer {
    PointCloudLayerId id;
    bool visible = true;
    PointColorMode colorMode;
    PointClassificationFilter classificationFilter;
    std::optional<RasterPointColorBinding> rasterColors;
    std::uint64_t colorGeneration = 0;
    BindingGeneration bindingGeneration;
    PointDatasetDescriptor descriptor;
    PointDatasetAvailability availability;
};

struct VectorLayer {
    SceneLayerId id;
    VectorLayerDataPtr data;
    bool visible = true;
    VectorLayerStyle style;
    BindingGeneration bindingGeneration;
};

struct RasterLayer {
    SceneLayerId id;
    bool visible = true;
    RasterLayerStyle style;
    RasterElevationStatus elevationStatus =
        RasterElevationStatus::NotApplicable;
    std::optional<RasterElevationRange> exactElevationRange;
    std::string elevationFailure;
    std::uint64_t elevationGeneration = 1;
    // Bumped only when a style change alters decoded pixels, so a stale worker
    // result cannot enter the cache. Camera motion never changes it.
    std::uint64_t renderGeneration = 1;
    BindingGeneration bindingGeneration;
    RasterDatasetDescriptor descriptor;
};

struct PointCloudLayerState {
    PointColorMode colorMode;
    PointClassificationFilter classificationFilter;
    std::optional<RasterPointColorBinding> rasterColors;
    std::uint64_t colorGeneration = 0;
    PointDatasetDescriptor descriptor;
    Bounds3d availableBounds;
    std::uint64_t availablePointCount = 0;
    PointColorizeAvailability colorizeAvailability =
        PointColorizeAvailability::Unsupported;
    PointCloudScalarRanges scalarRanges;
    PointClassificationFilter presentClassifications =
        PointClassificationFilter::noneVisible();
    PointCloudStorageMetrics storage;
};

struct PointLayerAvailabilityUpdate {
    PointCloudLayerId layerId;
    PointDatasetAvailability availability;
};

struct VectorLayerState {
    VectorLayerDataPtr data;
    VectorLayerStyle style;
};

struct RasterLayerState {
    RasterLayerStyle style;
    RasterElevationStatus elevationStatus =
        RasterElevationStatus::NotApplicable;
    std::optional<RasterElevationRange> exactElevationRange;
    std::string elevationFailure;
    std::uint64_t elevationGeneration = 1;
    std::uint64_t renderGeneration = 1;
    RasterDatasetDescriptor descriptor;
};

struct SceneLayer {
    SceneLayerId id;
    bool visible = true;
    std::variant<PointCloudLayerState, VectorLayerState, RasterLayerState>
        payload;
    BindingGeneration bindingGeneration;
};

using SceneLayerIndexMap = std::unordered_map<SceneLayerId, std::size_t>;

} // namespace pci
