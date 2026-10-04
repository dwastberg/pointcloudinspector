#pragma once

#include <pci/document/SceneLayerTypes.h>

#include <pci/foundation/Bounds3d.h>
#include <pci/foundation/Generation.h>
#include <pci/pointcloud/PointClassificationFilter.h>
#include <pci/pointcloud/PointColorPolicy.h>
#include <pci/pointcloud/PointDatasetDescriptor.h>
#include <pci/raster/RasterDatasetDescriptor.h>
#include <pci/raster/RasterLayer.h>
#include <pci/vector/VectorLayerData.h>
#include <pci/vector/VectorLayerStyle.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

namespace pci {

struct PointCloudLayerSnapshot {
    PointCloudLayerId id;
    bool visible = true;
    PointColorMode colorMode;
    PointClassificationFilter classificationFilter;
    std::optional<RasterPointColorBinding> rasterColors;
    std::uint64_t colorGeneration = 0;
    BindingGeneration bindingGeneration;
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

struct VectorLayerSnapshot {
    SceneLayerId id;
    VectorLayerDataPtr data;
    bool visible = true;
    VectorLayerStyle style;
    BindingGeneration bindingGeneration;
};

struct RasterLayerSnapshot {
    SceneLayerId id;
    bool visible = true;
    RasterLayerStyle style;
    RasterElevationStatus elevationStatus =
        RasterElevationStatus::NotApplicable;
    std::optional<RasterElevationRange> exactElevationRange;
    std::string elevationFailure;
    std::uint64_t elevationGeneration = 1;
    std::uint64_t renderGeneration = 1;
    BindingGeneration bindingGeneration;
    RasterDatasetDescriptor descriptor;
};

struct PointCloudLayerSnapshotState {
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

struct VectorLayerSnapshotState {
    VectorLayerDataPtr data;
    VectorLayerStyle style;
};

struct RasterLayerSnapshotState {
    RasterLayerStyle style;
    RasterElevationStatus elevationStatus =
        RasterElevationStatus::NotApplicable;
    std::optional<RasterElevationRange> exactElevationRange;
    std::string elevationFailure;
    std::uint64_t elevationGeneration = 1;
    std::uint64_t renderGeneration = 1;
    RasterDatasetDescriptor descriptor;
};

struct SceneSnapshotLayer {
    SceneLayerId id;
    bool visible = true;
    std::variant<PointCloudLayerSnapshotState,
                 VectorLayerSnapshotState,
                 RasterLayerSnapshotState>
        payload;
    BindingGeneration bindingGeneration;
};

using SceneSnapshotLayerIndexMap =
    std::unordered_map<SceneLayerId, std::size_t>;

} // namespace pci
