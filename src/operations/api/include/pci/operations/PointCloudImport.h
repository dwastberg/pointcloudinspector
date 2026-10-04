#pragma once

#include <pci/pointcloud/GpuPoint.h>
#include <pci/pointcloud/PointAttributes.h>
#include <pci/pointcloud/PointCloudMetadata.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>

namespace pci {

class LocalPageCacheContext;
using LocalPageCacheContextPtr = std::shared_ptr<const LocalPageCacheContext>;

enum class PointCloudImportStage {
    Reading,
    Optimizing,
};

struct PointCloudImportProgress {
    PointCloudImportStage stage = PointCloudImportStage::Reading;
    std::uint64_t processed = 0;
    std::uint64_t total = 0;
};

inline constexpr std::uint64_t flatImportWorkingBytes =
    std::uint64_t{64} * 1024 * 1024;

[[nodiscard]] inline std::uint64_t
estimatedFlatResidentBytes(const std::uint64_t pointCount) noexcept
{
    constexpr std::uint64_t bytesPerPoint =
        sizeof(GpuPoint) + sizeof(PointAttributes);
    constexpr std::uint64_t reserveGranularity = 4096;
    const std::uint64_t overheadBlocks = pointCount / 32'768 + 2;
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    if (pointCount > maximum / (2 * bytesPerPoint)) {
        return maximum;
    }
    const std::uint64_t payload = pointCount * 2 * bytesPerPoint;
    if (overheadBlocks > maximum / reserveGranularity / bytesPerPoint) {
        return maximum;
    }
    const std::uint64_t overhead =
        overheadBlocks * reserveGranularity * bytesPerPoint;
    return overhead > maximum - payload ? maximum : payload + overhead;
}

struct PointCloudImportPreflight {
    PointCloudMetadata metadata;
    std::uint64_t sourceFileBytes = 0;
    std::uint64_t sourceModificationTime = 0;
    // Complete source count for paged/hierarchical sources; bounded by the
    // requested maximum only for sources retained as flat point blocks.
    std::uint64_t desiredRetainedPoints = 0;
    std::uint64_t estimatedResidentBytes = 0;
    std::uint64_t estimatedActiveBytes = 0;
    // Admission fills these after inspection for retained-flat sources.
    // Zero means use the option's maximumPoints.
    std::uint64_t retainedPointLimit = 0;
    bool spatialPreview = false;
    bool hierarchical = false;
    bool localPaging = false;
};

inline constexpr std::uint64_t defaultPointCloudDecodedByteBudget =
    std::uint64_t{512} * 1024 * 1024;

struct LocalPagingOptions {
    // Ordinary LAS/LAZ above this source size uses the persistent local page
    // store. The value is a routing threshold, not a source-count limit.
    std::uint64_t pointThreshold = 1'000'000;
    LocalPageCacheContextPtr cache;
    std::uint32_t pagePoints = 32'768;
    std::uint32_t rootPreviewPoints = 16'384;
    std::uint64_t sortMemoryBytes = std::uint64_t{64} * 1024 * 1024;
    std::uint64_t diskCacheBytes = std::uint64_t{20} * 1024 * 1024 * 1024;

    bool operator==(const LocalPagingOptions &) const = default;
};

struct PointCloudLoadOptions {
    std::filesystem::path sourcePath;
    // Safety cap for non-paged sources. Paged and native-hierarchical sources
    // preserve the complete source and use their cache budgets for residency.
    std::uint64_t maximumPoints = 10'000'000;

    LocalPagingOptions localPaging;

    bool operator==(const PointCloudLoadOptions &) const = default;
};

class PointCloudImportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class PointCloudImportCancelled final : public PointCloudImportError {
public:
    PointCloudImportCancelled()
        : PointCloudImportError("Point-cloud import cancelled")
    {
    }
};

} // namespace pci
