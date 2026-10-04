// pci_raster_colorize_bench — non-default release qualification for the
// bounded raster-to-point colorization path.

#include <pci/operations/RasterPointColorizer.h>
#include <pci/runtime/point/PointDatasetRuntime.h>

#include <pci/operations/local/LocalRasterColorizeRunStore.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::uint64_t storedSamples = 22'900'000;
    std::uint32_t pointsPerNode = 100'000;
    std::uint32_t workers = 2;
    std::uint64_t scatterRecords = 2'000'000;
    std::filesystem::path temporaryDirectory =
        std::filesystem::temp_directory_path();
};

[[noreturn]] void usage()
{
    throw std::invalid_argument("usage: pci_raster_colorize_bench [--points N] "
                                "[--points-per-node N] [--workers 1..4] "
                                "[--scatter-records N] [--temp-dir PATH]");
}

template <typename Integer>
Integer parseInteger(const std::string_view text, const char *name)
{
    Integer value = 0;
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value == 0) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return value;
}

Options parseOptions(const int argc, char **argv)
{
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto value = [&]() -> std::string_view {
            if (++index >= argc) {
                usage();
            }
            return argv[index];
        };
        if (argument == "--points") {
            result.storedSamples =
                parseInteger<std::uint64_t>(value(), "point count");
        } else if (argument == "--points-per-node") {
            result.pointsPerNode =
                parseInteger<std::uint32_t>(value(), "points per node");
        } else if (argument == "--workers") {
            result.workers =
                parseInteger<std::uint32_t>(value(), "worker count");
        } else if (argument == "--scatter-records") {
            result.scatterRecords =
                parseInteger<std::uint64_t>(value(), "scatter record count");
        } else if (argument == "--temp-dir") {
            result.temporaryDirectory =
                std::filesystem::path(std::string(value()));
        } else {
            usage();
        }
    }
    if (result.workers > 4 || result.temporaryDirectory.empty()) {
        usage();
    }
    return result;
}

[[nodiscard]] pci::Bounds3d benchmarkBounds()
{
    return {.minimum = {0.0, 0.0, 0.0}, .maximum = {4999.0, 4999.0, 0.0}};
}

[[nodiscard]] pci::PointCloudNodeId nodeId(const std::uint64_t index)
{
    return index == 0 ? pci::rootPointCloudNode
                      : pci::PointCloudNodeId{
                            .level = 1,
                            .x = static_cast<std::uint32_t>(index),
                        };
}

class GeneratedPageSource final : public pci::PointCloudDataSource {
public:
    GeneratedPageSource(const std::uint64_t samples,
                        const std::uint32_t pointsPerNode)
        : pointsPerNode_(pointsPerNode)
    {
        const std::uint64_t nodeCount =
            (samples + pointsPerNode - 1) / pointsPerNode;
        nodes_.reserve(static_cast<std::size_t>(nodeCount));
        std::uint64_t remaining = samples;
        for (std::uint64_t index = 0; index < nodeCount; ++index) {
            const std::uint32_t count = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(remaining, pointsPerNode));
            nodes_.push_back({.id = nodeId(index),
                              .bounds = benchmarkBounds(),
                              .pointCount = count,
                              .localityKey = index});
            remaining -= count;
        }
    }

    [[nodiscard]] pci::PointCloudNode rootNode() const override
    {
        return node(pci::rootPointCloudNode);
    }

    [[nodiscard]] pci::PointCloudNode
    node(const pci::PointCloudNodeId id) const override
    {
        const auto found =
            std::ranges::find(nodes_, id, &pci::PointCloudStoredNode::id);
        return {.id = id,
                .bounds = benchmarkBounds(),
                .estimatedPointCount =
                    found == nodes_.end() ? 0 : found->pointCount,
                .leaf = true};
    }

    [[nodiscard]] pci::PointCloudNodePayloadPtr
    loadNode(const pci::PointCloudNodeId id,
             const std::stop_token stop) const override
    {
        const auto found =
            std::ranges::find(nodes_, id, &pci::PointCloudStoredNode::id);
        if (found == nodes_.end()) {
            return {};
        }
        const std::uint64_t pageIndex =
            static_cast<std::uint64_t>(found - nodes_.begin());
        auto block = std::make_shared<pci::PointBlock>();
        block->origin = {0.0, 0.0, 0.0};
        block->scale = 1.0;
        block->bounds = benchmarkBounds();
        block->points.reserve(found->pointCount);
        for (std::uint32_t point = 0; point < found->pointCount; ++point) {
            if ((point & 4095U) == 0 && stop.stop_requested()) {
                throw pci::PointCloudDataSourceCancelled();
            }
            const std::uint64_t ordinal = pageIndex * pointsPerNode_ + point;
            const std::uint64_t pixel = ordinal % 25'000'000ULL;
            block->points.push_back({
                .x = static_cast<std::uint16_t>(pixel % 5000),
                .y = static_cast<std::uint16_t>(pixel / 5000),
                .z = 0,
                .attributes = 0,
                .rgba = static_cast<std::uint32_t>(0xff000000U | ordinal),
                .packedProperties = 0,
            });
        }
        auto payload = std::make_shared<pci::PointCloudNodePayload>();
        payload->nodeId = id;
        payload->blocks.push_back(std::move(block));
        payload->sourcePointCount = found->pointCount;
        return payload;
    }

    [[nodiscard]] pci::PointCloudStorageMetrics storageMetrics() const override
    {
        return {.localPersistent = true, .committed = true};
    }

    [[nodiscard]] std::optional<std::vector<pci::PointCloudStoredNode>>
    storedNodeIndex() const override
    {
        return nodes_;
    }

private:
    std::uint32_t pointsPerNode_ = 0;
    std::vector<pci::PointCloudStoredNode> nodes_;
};

struct RasterState {
    std::mutex mutex;
    std::unordered_set<pci::RasterTileKey> tiles;
    std::uint64_t displayReads = 0;
    std::uint64_t detachedReads = 0;
};

class GeneratedRasterSource final : public pci::RasterTileSource {
public:
    GeneratedRasterSource(
        std::shared_ptr<RasterState> state = std::make_shared<RasterState>(),
        const bool detached = false)
        : state_(std::move(state))
        , detached_(detached)
    {
        metadata_.width = 5000;
        metadata_.height = 5000;
        metadata_.geoTransform = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    }

    [[nodiscard]] const pci::RasterLayerMetadata &
    metadata() const noexcept override
    {
        return metadata_;
    }

    [[nodiscard]] pci::RasterTileSourcePtr
    detachedReader(const std::uint32_t) const override
    {
        return std::make_shared<GeneratedRasterSource>(state_, true);
    }

    [[nodiscard]] pci::RasterTileData
    readTile(const pci::RasterTileRequest &request,
             const std::stop_token stop) const override
    {
        if (stop.stop_requested()) {
            throw pci::RasterReadCancelled();
        }
        {
            const std::scoped_lock lock(state_->mutex);
            ++(detached_ ? state_->detachedReads : state_->displayReads);
            state_->tiles.insert(request.key);
        }
        pci::RasterTileData result;
        result.key = request.key;
        result.renderGeneration = request.renderGeneration;
        const std::uint64_t originX =
            static_cast<std::uint64_t>(request.key.x) * pci::rasterTilePixels;
        const std::uint64_t originY =
            static_cast<std::uint64_t>(request.key.y) * pci::rasterTilePixels;
        result.validWidth = static_cast<std::uint16_t>(std::min<std::uint64_t>(
            pci::rasterTilePixels, metadata_.width - originX));
        result.validHeight = static_cast<std::uint16_t>(std::min<std::uint64_t>(
            pci::rasterTilePixels, metadata_.height - originY));
        result.rgba.assign(pci::rasterStoredTileBytes, std::byte{255});
        return result;
    }

    [[nodiscard]] const std::shared_ptr<RasterState> &state() const
    {
        return state_;
    }

private:
    pci::RasterLayerMetadata metadata_;
    std::shared_ptr<RasterState> state_;
    bool detached_ = false;
};

[[nodiscard]] pci::PointCloudNodePayloadPtr makeRoot()
{
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = {0.0, 0.0, 0.0};
    block->scale = 1.0;
    block->bounds = benchmarkBounds();
    block->points.reserve(1000);
    for (std::uint32_t point = 0; point < 1000; ++point) {
        block->points.push_back({
            .x = static_cast<std::uint16_t>(point % 5000),
            .y = static_cast<std::uint16_t>(point / 5000),
            .z = 0,
            .attributes = 0,
            .rgba = 0xff010203U,
            .packedProperties = 0,
        });
    }
    auto root = std::make_shared<pci::PointCloudNodePayload>();
    root->nodeId = pci::rootPointCloudNode;
    root->blocks.push_back(std::move(block));
    root->sourcePointCount = 1000;
    return root;
}

[[nodiscard]] pci::PointMemoryBudget::ReservationPtr
reserve(const pci::PointMemoryBudgetPtr &budget, const std::uint64_t bytes)
{
    const auto result = budget->tryReserve(bytes);
    if (!result) {
        throw std::runtime_error("benchmark point-memory admission failed");
    }
    return *result;
}

int run(const Options &options)
{
    auto source = std::make_shared<GeneratedPageSource>(options.storedSamples,
                                                        options.pointsPerNode);
    const auto root = makeRoot();
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = options.storedSamples;
    metadata.sourceBounds = benchmarkBounds();
    auto scene = std::make_shared<pci::PointDatasetRuntime>(
        metadata, source, root, 512ULL * 1024 * 1024);
    auto raster = std::make_shared<GeneratedRasterSource>();
    auto budget =
        std::make_shared<pci::PointMemoryBudget>(1024ULL * 1024 * 1024);
    pci::RasterColorizeOptions colorizeOptions{
        .workerCount = options.workers,
        .maximumScatterRecords = options.scatterRecords,
        .temporaryDirectory = options.temporaryDirectory,
    };
    auto target = scene->rasterPointColorizeTarget();
    if (!target) {
        throw std::runtime_error("benchmark scene is not colorizable");
    }
    pci::RasterColorizePreflight preflight = pci::preflightRasterPointColorize(
        std::move(*target), raster->metadata(), colorizeOptions);
    const std::uint64_t runBufferBytes =
        std::min(colorizeOptions.maximumScatterRecords,
                 preflight.maximumRecordCount) *
        sizeof(pci::RasterSortRecord);
    const auto started = Clock::now();
    auto result = pci::colorizePointCloudFromRaster(
        preflight,
        raster,
        std::make_shared<pci::RasterDecodeParameters>(),
        1,
        colorizeOptions,
        reserve(budget, preflight.tableEntries * sizeof(std::uint32_t)),
        reserve(budget, preflight.workingReservationBytes),
        reserve(budget, preflight.rootStagingReservationBytes),
        reserve(budget, preflight.flatStagingReservationBytes),
        pci::makeLocalRasterColorizeRunStoreFactory(),
        {},
        {});
    if (!result || !*result) {
        throw std::runtime_error("benchmark colorization was cancelled");
    }
    const pci::RasterColorizeStatistics statistics = (*result)->statistics;
    if (scene->applyRasterPointColors(pci::preparePointColorInstallation(
            *result)) != pci::RasterPointColorApplyOutcome::Applied) {
        throw std::runtime_error("benchmark prepared result was stale");
    }
    const pci::RasterPointColorMetrics retained =
        scene->rasterPointColorMetrics();
    if (scene->revertPointColors() !=
        pci::RasterPointColorApplyOutcome::Applied) {
        throw std::runtime_error("benchmark revert failed");
    }
    result.reset();
    const double milliseconds =
        std::chrono::duration<double, std::milli>(Clock::now() - started)
            .count();
    std::uint64_t distinctTiles = 0;
    std::uint64_t displayReads = 0;
    std::uint64_t detachedReads = 0;
    {
        const std::scoped_lock lock(raster->state()->mutex);
        distinctTiles = raster->state()->tiles.size();
        displayReads = raster->state()->displayReads;
        detachedReads = raster->state()->detachedReads;
    }
    const pci::PointMemoryBudgetMetrics memory = budget->metrics();
    const bool bounded =
        statistics.tileReads >= distinctTiles &&
        statistics.tileReads <=
            distinctTiles * pci::rasterColorizeTileReadOverheadBound &&
        statistics.temporaryBytesWritten <= preflight.maximumTemporaryBytes &&
        memory.reservedBytes == 0 && displayReads == 0 &&
        detachedReads == statistics.tileReads;
    std::printf(
        "COLORIZE_RESULT status=%s stored_samples=%llu table_entries=%llu "
        "table_bytes=%llu run_buffer_bytes=%llu temp_bytes=%llu "
        "tile_reads=%llu distinct_tiles=%llu display_reads=%llu "
        "point_memory_peak=%llu retained_table_bytes=%llu "
        "retained_source_root_bytes=%llu retained_colored_root_bytes=%llu "
        "elapsed_ms=%.3f bounded=%d\n",
        bounded ? "ok" : "unbounded",
        static_cast<unsigned long long>(options.storedSamples),
        static_cast<unsigned long long>(preflight.tableEntries),
        static_cast<unsigned long long>(preflight.tableEntries * 4ULL),
        static_cast<unsigned long long>(runBufferBytes),
        static_cast<unsigned long long>(statistics.temporaryBytesWritten),
        static_cast<unsigned long long>(statistics.tileReads),
        static_cast<unsigned long long>(distinctTiles),
        static_cast<unsigned long long>(displayReads),
        static_cast<unsigned long long>(memory.peakReservedBytes),
        static_cast<unsigned long long>(retained.activeColorTableBytes),
        static_cast<unsigned long long>(retained.retainedSourceRootBytes),
        static_cast<unsigned long long>(retained.retainedColoredRootBytes),
        milliseconds,
        bounded ? 1 : 0);
    return bounded ? 0 : 2;
}

} // namespace

int main(const int argc, char **argv)
{
    try {
        return run(parseOptions(argc, argv));
    } catch (const std::exception &error) {
        std::fprintf(
            stderr, "COLORIZE_RESULT status=error message=%s\n", error.what());
        return 1;
    }
}
