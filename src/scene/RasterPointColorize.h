#pragma once

#include "raster/RasterPointSampler.h"
#include "scene/PointCloudDataSource.h"
#include "scene/PointMemoryBudget.h"
#include "scene/SceneBlock.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <variant>
#include <vector>

namespace pci {

class PointCloudScene;
using PointCloudScenePtr = std::shared_ptr<PointCloudScene>;

enum class RasterPointColorizeAvailability : std::uint8_t {
    Ready,
    Loading,
    Unsupported,
};

enum class RasterPointColorApplyOutcome : std::uint8_t {
    Applied,
    Stale,
};

struct RasterColorizeHierarchicalTarget {
    PointCloudDataSourcePtr baseSource;
    std::vector<PointCloudStoredNode> storedNodes;
    PointCloudNodePayloadPtr sourceRootPayload;
    std::uint64_t expectedRootPayloadRevision = 0;
};

struct RasterColorizeFlatTarget {
    std::vector<SceneBlock> blocks;
    std::shared_ptr<const std::vector<std::uint32_t>> sourceColors;
};

using RasterColorizeTargetData =
    std::variant<RasterColorizeHierarchicalTarget, RasterColorizeFlatTarget>;

struct RasterColorizeTargetSnapshot {
    PointCloudScenePtr scene;
    RasterColorizeTargetData data;
};

struct RasterColorizeRange {
    PointCloudNodeId node;
    std::uint64_t offset = 0;
    std::uint32_t count = 0;
};

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
    std::uint64_t rootStagingReservationBytes = 0;
    std::uint64_t flatStagingReservationBytes = 0;
    std::uint64_t rejectedPoints = 0;
    std::uint64_t rejectedNodes = 0;
};

struct RasterColorizeOptions {
    std::uint32_t workerCount = 2;
    std::uint64_t maximumScatterRecords = 2'000'000;
    std::uint32_t maximumTileFailures = 64;
    std::filesystem::path temporaryDirectory;
    std::uint64_t maximumTemporaryBytes = 4ULL * 1024 * 1024 * 1024;
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
    PointCloudDataSourcePtr colorizedSource;
    PointCloudNodePayloadPtr sourceRootPayload;
    PointCloudNodePayloadPtr coloredRootPayload;
    std::uint64_t expectedRootPayloadRevision = 0;
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
    PointCloudScenePtr scene;
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
