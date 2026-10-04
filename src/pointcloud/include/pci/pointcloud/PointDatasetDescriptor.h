#pragma once

#include <pci/pointcloud/PointClassificationFilter.h>
#include <pci/pointcloud/PointCloudMetadata.h>
#include <pci/pointcloud/PointCloudStorageMetrics.h>
#include <pci/pointcloud/PointColorPolicy.h>
#include <pci/pointcloud/PointIdentity.h>

#include <cstdint>

namespace pci {

// Immutable document-facing identity and metadata for one point dataset.
// Decoder, cache, scheduling, and mutable availability belong to its runtime
// binding and are deliberately absent from this value.
struct PointDatasetDescriptor {
    PointCloudSourceId sourceId;
    PointCloudMetadata metadata;
};

enum class PointColorizeAvailability : std::uint8_t {
    Ready,
    Loading,
    Unsupported,
};

// A coherent value snapshot of the changing, document-visible portion of a
// point dataset. It deliberately contains no decoder, cache, scheduler, or
// mutable dataset handle.
struct PointDatasetAvailability {
    Bounds3d bounds;
    std::uint64_t pointCount = 0;
    PointColorizeAvailability colorizeAvailability =
        PointColorizeAvailability::Unsupported;
    PointCloudScalarRanges scalarRanges;
    PointClassificationFilter presentClassifications =
        PointClassificationFilter::noneVisible();
    PointCloudStorageMetrics storage;

    [[nodiscard]] bool
    operator==(const PointDatasetAvailability &other) const noexcept
    {
        return bounds.minimum == other.bounds.minimum &&
               bounds.maximum == other.bounds.maximum &&
               pointCount == other.pointCount &&
               colorizeAvailability == other.colorizeAvailability &&
               scalarRanges == other.scalarRanges &&
               presentClassifications == other.presentClassifications &&
               storage == other.storage;
    }
};

struct PointDatasetView {
    PointDatasetDescriptor descriptor;
    PointDatasetAvailability availability;
};

} // namespace pci
