#include "scene/RasterPointColorize.h"

#include "foundation/CheckedArithmetic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <system_error>

namespace pci {
namespace {

constexpr std::uint64_t temporarySafetyMargin = 64ULL * 1024 * 1024;

[[nodiscard]] bool candidateBounds(const RasterInversePlacement &placement,
                                   const RasterLayerMetadata &raster,
                                   const Bounds3d &bounds) noexcept
{
    const std::array<std::array<double, 2>, 4> corners{{
        {bounds.minimum[0], bounds.minimum[1]},
        {bounds.maximum[0], bounds.minimum[1]},
        {bounds.maximum[0], bounds.maximum[1]},
        {bounds.minimum[0], bounds.maximum[1]},
    }};
    double minimumPixel = std::numeric_limits<double>::infinity();
    double minimumLine = std::numeric_limits<double>::infinity();
    double maximumPixel = -std::numeric_limits<double>::infinity();
    double maximumLine = -std::numeric_limits<double>::infinity();
    for (const auto &corner : corners) {
        double pixel = 0.0;
        double line = 0.0;
        placement.toPixelEdge(corner[0], corner[1], pixel, line);
        if (!std::isfinite(pixel) || !std::isfinite(line)) {
            return false;
        }
        minimumPixel = std::min(minimumPixel, pixel);
        maximumPixel = std::max(maximumPixel, pixel);
        minimumLine = std::min(minimumLine, line);
        maximumLine = std::max(maximumLine, line);
    }
    return maximumPixel >= 0.0 && maximumLine >= 0.0 &&
           minimumPixel < static_cast<double>(raster.width) &&
           minimumLine < static_cast<double>(raster.height);
}

[[nodiscard]] std::uint64_t blockBytes(const PointBlock &block) noexcept
{
    return static_cast<std::uint64_t>(block.points.capacity()) *
               sizeof(GpuPoint) +
           static_cast<std::uint64_t>(block.attributes.capacity()) *
               sizeof(PointAttributes);
}

void ensureAdd(std::uint64_t &value,
               const std::uint64_t addition,
               const char *message)
{
    const auto sum = checkedAdd(value, addition);
    if (!sum) {
        throw RasterColorizeError(RasterColorizeFailureCode::TooManyPoints,
                                  message);
    }
    value = *sum;
}

} // namespace

RasterColorizeError::RasterColorizeError(const RasterColorizeFailureCode code,
                                         std::string message)
    : std::runtime_error(message)
    , code_(code)
{
}

RasterColorizeFailureCode RasterColorizeError::code() const noexcept
{
    return code_;
}

RasterColorizePreflight
preflightRasterPointColorize(RasterColorizeTargetSnapshot target,
                             const RasterLayerMetadata &raster,
                             const RasterColorizeOptions &options)
{
    if (!target.scene) {
        throw RasterColorizeError(RasterColorizeFailureCode::SourceChanged,
                                  "Point-cloud target is no longer available");
    }
    if (options.workerCount == 0 || options.workerCount > 4 ||
        options.maximumScatterRecords == 0 ||
        options.temporaryDirectory.empty()) {
        throw RasterColorizeError(RasterColorizeFailureCode::Internal,
                                  "Invalid raster colorization options");
    }
    const auto placement = RasterInversePlacement::forMetadata(raster);
    if (!placement) {
        throw RasterColorizeError(
            RasterColorizeFailureCode::InvalidRasterPlacement,
            "Raster has no usable affine placement");
    }

    RasterColorizePreflight result;
    result.target = std::move(target);
    result.placement = *placement;

    std::uint64_t maximumDecodedPageBytes = 0;
    if (const auto *hierarchy = std::get_if<RasterColorizeHierarchicalTarget>(
            &result.target.data)) {
        std::vector<PointCloudStoredNode> nodes = hierarchy->storedNodes;
        std::ranges::sort(nodes,
                          [](const PointCloudStoredNode &left,
                             const PointCloudStoredNode &right) {
                              if (left.localityKey != right.localityKey) {
                                  return left.localityKey < right.localityKey;
                              }
                              return left.id < right.id;
                          });
        result.ranges.reserve(nodes.size());
        for (const PointCloudStoredNode &node : nodes) {
            if (!candidateBounds(*placement, raster, node.bounds)) {
                ensureAdd(result.rejectedPoints,
                          node.pointCount,
                          "Rejected point count overflows");
                ++result.rejectedNodes;
                continue;
            }
            if (node.pointCount > std::numeric_limits<std::uint32_t>::max()) {
                throw RasterColorizeError(
                    RasterColorizeFailureCode::TooManyPoints,
                    "One stored point-cloud node exceeds the supported size");
            }
            result.ranges.push_back({
                .node = node.id,
                .offset = result.tableEntries,
                .count = static_cast<std::uint32_t>(node.pointCount),
            });
            ensureAdd(result.tableEntries,
                      node.pointCount,
                      "Raster color table size overflows");
            ensureAdd(result.maximumRecordCount,
                      node.pointCount,
                      "Raster sample record count overflows");
            maximumDecodedPageBytes =
                std::max(maximumDecodedPageBytes,
                         saturatingMultiply(
                             node.pointCount,
                             static_cast<std::uint64_t>(
                                 sizeof(GpuPoint) + sizeof(PointAttributes))));
        }
        result.rootOffset = result.tableEntries;
        const std::uint64_t rootPoints =
            hierarchy->sourceRootPayload
                ? pointCloudNodePayloadPoints(*hierarchy->sourceRootPayload)
                : 0;
        if (rootPoints > std::numeric_limits<std::uint32_t>::max()) {
            throw RasterColorizeError(
                RasterColorizeFailureCode::TooManyPoints,
                "Point-cloud root exceeds supported size");
        }
        result.rootCount = static_cast<std::uint32_t>(rootPoints);
        ensureAdd(result.tableEntries,
                  rootPoints,
                  "Raster color table size overflows");
        ensureAdd(result.maximumRecordCount,
                  rootPoints,
                  "Raster sample record count overflows");
        result.rootStagingReservationBytes =
            hierarchy->sourceRootPayload
                ? pointCloudNodePayloadBytes(*hierarchy->sourceRootPayload)
                : 0;
    } else {
        const auto &flat =
            std::get<RasterColorizeFlatTarget>(result.target.data);
        result.flatRanges.reserve(flat.blocks.size());
        for (const SceneBlock &entry : flat.blocks) {
            if (!entry.block) {
                throw RasterColorizeError(
                    RasterColorizeFailureCode::SourceChanged,
                    "Point-cloud block snapshot is incomplete");
            }
            const std::uint64_t count = entry.block->points.size();
            if (count > std::numeric_limits<std::uint32_t>::max()) {
                throw RasterColorizeError(
                    RasterColorizeFailureCode::TooManyPoints,
                    "One point-cloud block exceeds the supported size");
            }
            result.flatRanges.push_back({
                .blockId = entry.id,
                .offset = result.tableEntries,
                .count = static_cast<std::uint32_t>(count),
            });
            ensureAdd(result.tableEntries,
                      count,
                      "Raster color table size overflows");
            if (candidateBounds(*placement, raster, entry.block->bounds)) {
                ensureAdd(result.maximumRecordCount,
                          count,
                          "Raster sample record count overflows");
            } else {
                ensureAdd(result.rejectedPoints,
                          count,
                          "Rejected point count overflows");
                ++result.rejectedNodes;
            }
            ensureAdd(result.flatStagingReservationBytes,
                      blockBytes(*entry.block),
                      "Flat staging size overflows");
        }
        if (flat.sourceColors &&
            flat.sourceColors->size() != result.tableEntries) {
            throw RasterColorizeError(
                RasterColorizeFailureCode::SourceChanged,
                "Retained source colors no longer match the point cloud");
        }
    }

    if (result.tableEntries > std::numeric_limits<std::uint32_t>::max()) {
        throw RasterColorizeError(
            RasterColorizeFailureCode::TooManyPoints,
            "Raster colorization requires " +
                std::to_string(result.tableEntries) +
                " table entries, exceeding the 32-bit destination limit");
    }
    result.maximumTemporaryBytes = saturatingAdd(
        saturatingMultiply(result.maximumRecordCount, std::uint64_t{12}),
        std::uint64_t{4096});
    if (result.maximumTemporaryBytes > options.maximumTemporaryBytes) {
        throw RasterColorizeError(
            RasterColorizeFailureCode::InsufficientTemporarySpace,
            "Raster colorization may require " +
                std::to_string(result.maximumTemporaryBytes) +
                " temporary bytes, exceeding the configured limit");
    }

    std::error_code error;
    const auto space =
        std::filesystem::space(options.temporaryDirectory, error);
    if (error || space.available < saturatingAdd(result.maximumTemporaryBytes,
                                                 temporarySafetyMargin)) {
        throw RasterColorizeError(
            RasterColorizeFailureCode::InsufficientTemporarySpace,
            "Insufficient free space for raster colorization temporary files");
    }

    const std::uint64_t recordBytes = saturatingMultiply(
        std::min(options.maximumScatterRecords, result.maximumRecordCount),
        std::uint64_t{16});
    result.workingReservationBytes = saturatingAdd(
        recordBytes,
        saturatingMultiply(
            static_cast<std::uint64_t>(options.workerCount),
            saturatingAdd(maximumDecodedPageBytes,
                          sizeof(RasterTileData) + rasterStoredTileBytes +
                              rasterMaximumTileReadReservationBytes)));
    return result;
}

} // namespace pci
