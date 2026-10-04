#pragma once

#include <pci/pointcloud/RasterPointColorData.h>
#include <pci/raster/RasterPointSampler.h>

#include <pci/runtime/PointMemoryBudget.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <variant>
#include <vector>

namespace pci {

struct RasterColorizeFlatRange {
    std::uint64_t blockId = 0;
    std::uint64_t offset = 0;
    std::uint32_t count = 0;
};

struct RasterColorizePreflight {
    RasterColorizeTargetSnapshot target;
    RasterInversePlacement placement;
    std::vector<RasterColorizeRange> ranges;
    std::vector<RasterColorizeFlatRange> flatRanges;
    std::uint64_t rootOffset = 0;
    std::uint32_t rootCount = 0;
    std::uint64_t tableEntries = 0;
    std::uint64_t maximumRecordCount = 0;
    std::uint64_t maximumTemporaryBytes = 0;
    std::uint64_t workingReservationBytes = 0;
    std::uint64_t writerScratchBytes = 0;
    std::uint64_t rootStagingReservationBytes = 0;
    std::uint64_t flatStagingReservationBytes = 0;
    std::uint64_t rejectedPoints = 0;
    std::uint64_t rejectedNodes = 0;
};

struct RasterColorizeResourceEstimate {
    std::uint64_t persistentColorBytes = 0;
    std::uint64_t workingMemoryBytes = 0;
    std::uint64_t maximumTemporaryBytes = 0;
};

[[nodiscard]] RasterColorizeResourceEstimate rasterColorizeResourceEstimate(
    const RasterColorizePreflight &preflight) noexcept;

struct RasterColorizeOptions {
    std::uint32_t workerCount = 2;
    std::uint64_t maximumScatterRecords = 2'000'000;
    std::uint32_t maximumTileFailures = 64;
    std::filesystem::path temporaryDirectory;
    std::uint64_t maximumTemporaryBytes = 4ULL * 1024 * 1024 * 1024;
    std::uint64_t maximumWriterScratchBytes = 64 * 1024;
};

enum class RasterColorizeFailureCode : std::uint8_t {
    Loading,
    UnsupportedSource,
    InvalidRasterPlacement,
    TooManyPoints,
    InsufficientPointMemory,
    InsufficientTemporarySpace,
    SourceChanged,
    Io,
    Internal,
};

class RasterColorizeError final : public std::runtime_error {
public:
    RasterColorizeError(RasterColorizeFailureCode code, std::string message);
    [[nodiscard]] RasterColorizeFailureCode code() const noexcept;

private:
    RasterColorizeFailureCode code_;
};

enum class RasterColorizePhase : std::uint8_t {
    BuildingRecords,
    SamplingRaster,
};

struct RasterColorizeProgress {
    RasterColorizePhase phase = RasterColorizePhase::BuildingRecords;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
    std::uint64_t tileReads = 0;
    std::uint64_t temporaryBytesWritten = 0;
    std::uint64_t tileFailures = 0;
};

struct RasterColorizeStatistics {
    std::uint64_t pointsConsidered = 0;
    std::uint64_t pointsColored = 0;
    std::uint64_t pointsUnchanged = 0;
    std::uint64_t nodesRejected = 0;
    std::uint64_t tileReads = 0;
    std::uint64_t tileFailures = 0;
    std::uint64_t temporaryBytesWritten = 0;
};

struct RasterColorizePreparedHierarchy {
    PointCloudDataSourcePtr baseSource;
    std::vector<std::uint32_t> colors;
    std::vector<RasterColorizeRange> ranges;
    PointCloudNodePayloadPtr sourceRootPayload;
    PointCloudNodePayloadPtr coloredRootPayload;
    std::uint64_t expectedRootPayloadRevision = 0;
    PointMemoryBudget::ReservationPtr colorReservation;
    PointMemoryBudget::ReservationPtr rootStagingReservation;
};

struct RasterColorizeFlatBlockIdentity {
    std::uint64_t id = 0;
    std::weak_ptr<const PointBlock> block;
    std::uint64_t pointCount = 0;
    std::uint64_t attributeCount = 0;
};

struct RasterColorizePreparedFlat {
    std::vector<RasterColorizeFlatBlockIdentity> expectedBlocks;
    std::vector<SceneBlock> replacementBlocks;
    std::shared_ptr<std::vector<std::uint32_t>> displacedSourceColors;
    PointMemoryBudget::ReservationPtr colorReservation;
    PointMemoryBudget::ReservationPtr stagingReservation;
};

using RasterColorizePreparedData =
    std::variant<RasterColorizePreparedHierarchy, RasterColorizePreparedFlat>;

struct RasterColorizePrepared {
    PointCloudSourceId sourceId;
    RasterColorizePreparedData data;
    RasterColorizeStatistics statistics;
};

using RasterColorizePreparedPtr = std::shared_ptr<RasterColorizePrepared>;

[[nodiscard]] RasterColorizePreflight
preflightRasterPointColorize(RasterColorizeTargetSnapshot target,
                             const RasterLayerMetadata &raster,
                             const RasterColorizeOptions &options);

inline constexpr std::uint64_t rasterColorizeTileReadOverheadBound = 2;

} // namespace pci
