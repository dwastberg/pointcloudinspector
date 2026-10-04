#include <pci/adapters/storage/SecureStorage.h>
#include <pci/operations/RasterPointColorizer.h>
#include <pci/runtime/point/PointDatasetRuntime.h>

#include <pci/operations/local/LocalRasterColorizeRunStore.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory()
        : directory_(std::filesystem::temp_directory_path(),
                     "pcinspector-colorize-scale")
    {
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return directory_.path();
    }

    [[nodiscard]] bool empty() const
    {
        return std::filesystem::is_empty(directory_.path());
    }

private:
    pci::PrivateTemporaryDirectory directory_;
};

struct RasterCounters {
    bool pauseReads = false;
    std::atomic_bool enteredRead = false;
    std::promise<void> readStarted;
    std::condition_variable_any readWake;
    std::atomic_uint64_t displayReads = 0;
    std::atomic_uint64_t detachedReads = 0;
    std::atomic_uint32_t detachedMaximumHandles = 0;
    std::atomic_uint32_t activeReads = 0;
    std::atomic_uint32_t peakActiveReads = 0;
    std::mutex keysMutex;
    std::unordered_set<pci::RasterTileKey> keys;
};

class ScaleRasterSource final : public pci::RasterTileSource {
public:
    ScaleRasterSource(const std::uint32_t size,
                      std::shared_ptr<RasterCounters> counters =
                          std::make_shared<RasterCounters>(),
                      const bool detached = false)
        : counters_(std::move(counters))
        , detached_(detached)
    {
        metadata_.width = size;
        metadata_.height = size;
        metadata_.geoTransform = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    }

    [[nodiscard]] const pci::RasterLayerMetadata &
    metadata() const noexcept override
    {
        return metadata_;
    }

    [[nodiscard]] pci::RasterTileSourcePtr
    detachedReader(const std::uint32_t maximumHandles) const override
    {
        counters_->detachedMaximumHandles = maximumHandles;
        return std::make_shared<ScaleRasterSource>(
            metadata_.width, counters_, true);
    }

    [[nodiscard]] pci::RasterTileData
    readTile(const pci::RasterTileRequest &request,
             const std::stop_token stop) const override
    {
        if (stop.stop_requested()) {
            throw pci::RasterReadCancelled();
        }
        if (counters_->pauseReads) {
            if (!counters_->enteredRead.exchange(true)) {
                counters_->readStarted.set_value();
            }
            std::unique_lock lock(counters_->keysMutex);
            counters_->readWake.wait(lock, stop, [] {
                return false;
            });
            throw pci::RasterReadCancelled();
        }
        const std::uint32_t active = counters_->activeReads.fetch_add(1) + 1;
        std::uint32_t peak = counters_->peakActiveReads.load();
        while (
            peak < active &&
            !counters_->peakActiveReads.compare_exchange_weak(peak, active)) {
        }
        struct ActiveGuard {
            std::atomic_uint32_t &active;
            ~ActiveGuard()
            {
                active.fetch_sub(1);
            }
        } guard{counters_->activeReads};

        if (detached_) {
            ++counters_->detachedReads;
        } else {
            ++counters_->displayReads;
        }
        {
            const std::scoped_lock lock(counters_->keysMutex);
            counters_->keys.insert(request.key);
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

    [[nodiscard]] const std::shared_ptr<RasterCounters> &counters() const
    {
        return counters_;
    }

private:
    pci::RasterLayerMetadata metadata_;
    std::shared_ptr<RasterCounters> counters_;
    bool detached_ = false;
};

[[nodiscard]] std::shared_ptr<pci::PointDatasetRuntime>
makeScaleScene(const std::uint32_t copies,
               const std::uint32_t width,
               const std::uint32_t height)
{
    constexpr std::uint32_t blockCount = 64;
    const std::uint64_t total =
        static_cast<std::uint64_t>(width) * height * copies;
    auto scene = std::make_shared<pci::PointDatasetRuntime>(
        pci::PointCloudMetadata{.sourcePointCount = total});
    for (std::uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
        auto block = std::make_shared<pci::PointBlock>();
        block->origin = {0.0, 0.0, 0.0};
        block->scale = 1.0;
        block->bounds = {.minimum = {0.0, 0.0, 0.0},
                         .maximum = {width - 1.0, height - 1.0, 0.0}};
        const std::uint64_t first = total * blockIndex / blockCount;
        const std::uint64_t last = total * (blockIndex + 1) / blockCount;
        block->points.reserve(static_cast<std::size_t>(last - first));
        for (std::uint64_t index = first; index < last; ++index) {
            const std::uint64_t pixel = index % (width * height);
            block->points.push_back({
                .x = static_cast<std::uint16_t>(pixel % width),
                .y = static_cast<std::uint16_t>(pixel / width),
                .z = 0,
                .rgba = static_cast<std::uint32_t>(0xff000000U | index),
            });
        }
        scene->addBlock(std::move(block));
    }
    scene->markLoadingComplete();
    return scene;
}

[[nodiscard]] pci::PointMemoryBudget::ReservationPtr
reserve(const pci::PointMemoryBudgetPtr &budget, const std::uint64_t bytes)
{
    const auto result = budget->tryReserve(bytes);
    REQUIRE(result.has_value());
    return *result;
}

struct ScaleOutcome {
    pci::RasterColorizeStatistics statistics;
    pci::PointMemoryBudgetMetrics memory;
    std::uint64_t distinctTiles = 0;
    std::uint64_t displayReads = 0;
    std::uint32_t detachedMaximumHandles = 0;
};

[[nodiscard]] ScaleOutcome bake(const std::uint32_t copies,
                                const std::uint32_t rasterSize,
                                const std::filesystem::path &temporaryDirectory,
                                const std::uint32_t width,
                                const std::uint32_t height,
                                const std::uint32_t scatterRecords)
{
    auto scene = makeScaleScene(copies, width, height);
    auto raster = std::make_shared<ScaleRasterSource>(rasterSize);
    auto budget =
        std::make_shared<pci::PointMemoryBudget>(512ULL * 1024 * 1024);
    pci::RasterColorizeOptions options{
        .workerCount = 4,
        .maximumScatterRecords = scatterRecords,
        .temporaryDirectory = temporaryDirectory,
    };
    auto target = scene->rasterPointColorizeTarget();
    REQUIRE(target.has_value());
    pci::RasterColorizePreflight preflight = pci::preflightRasterPointColorize(
        std::move(*target), raster->metadata(), options);
    const std::uint64_t admitted =
        preflight.tableEntries * sizeof(std::uint32_t) +
        preflight.workingReservationBytes +
        preflight.rootStagingReservationBytes +
        preflight.flatStagingReservationBytes;
    auto result = pci::colorizePointCloudFromRaster(
        preflight,
        raster,
        std::make_shared<pci::RasterDecodeParameters>(),
        1,
        options,
        reserve(budget, preflight.tableEntries * sizeof(std::uint32_t)),
        reserve(budget, preflight.workingReservationBytes),
        reserve(budget, preflight.rootStagingReservationBytes),
        reserve(budget, preflight.flatStagingReservationBytes),
        pci::makeLocalRasterColorizeRunStoreFactory(),
        {},
        {});
    REQUIRE(result.has_value());
    REQUIRE(*result);
    const pci::RasterColorizeStatistics statistics = (*result)->statistics;
    CHECK(statistics.temporaryBytesWritten ==
          preflight.maximumRecordCount * 12ULL);
    CHECK(budget->metrics().peakReservedBytes <= admitted);
    result.reset();
    CHECK(budget->reservedBytes() == 0);
    std::uint64_t distinctTiles = 0;
    {
        const std::scoped_lock lock(raster->counters()->keysMutex);
        distinctTiles = raster->counters()->keys.size();
    }
    return {.statistics = statistics,
            .memory = budget->metrics(),
            .distinctTiles = distinctTiles,
            .displayReads = raster->counters()->displayReads.load(),
            .detachedMaximumHandles =
                raster->counters()->detachedMaximumHandles.load()};
}

void checkBoundedBake(const std::uint32_t width,
                      const std::uint32_t height,
                      const std::uint32_t scatterRecords,
                      const std::uint32_t expectedHandles)
{
    TemporaryDirectory directory;
    const auto first =
        bake(1, 5000, directory.path(), width, height, scatterRecords);
    CHECK(directory.empty());
    const auto doubled =
        bake(2, 5000, directory.path(), width, height, scatterRecords);
    CHECK(directory.empty());
    const auto sparseExtent =
        bake(1, 100'000, directory.path(), width, height, scatterRecords);
    CHECK(directory.empty());

    const std::uint64_t pointCount = static_cast<std::uint64_t>(width) * height;
    CHECK(first.statistics.pointsConsidered == pointCount);
    CHECK(first.statistics.pointsColored == pointCount);
    CHECK(doubled.statistics.pointsConsidered == pointCount * 2);
    CHECK(doubled.statistics.pointsColored == pointCount * 2);
    CHECK(sparseExtent.statistics.pointsColored == pointCount);
    CHECK(first.distinctTiles > 1);
    CHECK(first.distinctTiles == doubled.distinctTiles);
    CHECK(sparseExtent.distinctTiles == first.distinctTiles);
    const auto checkTileReadsBounded = [](const ScaleOutcome &outcome) {
        CHECK(outcome.statistics.tileReads >= outcome.distinctTiles);
        CHECK(outcome.statistics.tileReads <=
              outcome.distinctTiles * pci::rasterColorizeTileReadOverheadBound);
    };
    checkTileReadsBounded(first);
    checkTileReadsBounded(doubled);
    checkTileReadsBounded(sparseExtent);
    CHECK(first.displayReads == 0);
    CHECK(doubled.displayReads == 0);
    CHECK(sparseExtent.displayReads == 0);
    CHECK(doubled.detachedMaximumHandles == expectedHandles);
}

void checkCancellation(const std::uint32_t width, const std::uint32_t height)
{
    TemporaryDirectory directory;
    auto scene = makeScaleScene(2, width, height);
    auto raster = std::make_shared<ScaleRasterSource>(5000);
    raster->counters()->pauseReads = true;
    auto enteredFuture = raster->counters()->readStarted.get_future();
    auto budget =
        std::make_shared<pci::PointMemoryBudget>(512ULL * 1024 * 1024);
    pci::RasterColorizeOptions options{
        .workerCount = 4,
        .maximumScatterRecords = 32'768,
        .temporaryDirectory = directory.path(),
    };
    auto target = scene->rasterPointColorizeTarget();
    REQUIRE(target.has_value());
    pci::RasterColorizePreflight preflight = pci::preflightRasterPointColorize(
        std::move(*target), raster->metadata(), options);
    std::stop_source stop;
    auto future = std::async(
        std::launch::async,
        [preflight = std::move(preflight),
         raster,
         budget,
         options,
         stopToken = stop.get_token()]() mutable {
            return pci::colorizePointCloudFromRaster(
                preflight,
                raster,
                std::make_shared<pci::RasterDecodeParameters>(),
                1,
                options,
                reserve(budget, preflight.tableEntries * sizeof(std::uint32_t)),
                reserve(budget, preflight.workingReservationBytes),
                reserve(budget, preflight.rootStagingReservationBytes),
                reserve(budget, preflight.flatStagingReservationBytes),
                pci::makeLocalRasterColorizeRunStoreFactory(),
                stopToken,
                {});
        });
    // Stop only after sampling has started, even on a fast machine. Always
    // release the worker before asserting so a failure cannot hang its future.
    const auto entered = enteredFuture.wait_for(std::chrono::seconds(60));
    const auto cancelledAt = std::chrono::steady_clock::now();
    stop.request_stop();
    const auto result = future.get();
    const auto elapsed = std::chrono::steady_clock::now() - cancelledAt;
    REQUIRE(entered == std::future_status::ready);
    CHECK_FALSE(result.has_value());
    CHECK(elapsed < std::chrono::milliseconds(500));
    CHECK(budget->reservedBytes() == 0);
    CHECK(directory.empty());
}

TEST_CASE("raster bake remains bounded across multiple tiles and runs",
          "[integration][raster][colorize]")
{
    checkBoundedBake(512, 256, 8192, 2);
}

TEST_CASE("raster bake cancellation during sampling releases runs and memory",
          "[integration][raster][colorize][cancellation]")
{
    checkCancellation(512, 256);
}

TEST_CASE("four-million-point raster bake remains footprint bounded",
          "[integration][stress][long-stress][scale][colorize]")
{
    checkBoundedBake(2000, 1000, 100'000, 1);
}

TEST_CASE("four-million-point raster bake cancels promptly and cleans runs",
          "[integration][stress][long-stress][scale][colorize][cancellation]")
{
    checkCancellation(2000, 1000);
}

} // namespace
