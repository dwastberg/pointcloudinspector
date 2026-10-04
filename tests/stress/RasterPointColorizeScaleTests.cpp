#include <pci/adapters/storage/SecureStorage.h>
#include <pci/operations/RasterPointColorizer.h>
#include <pci/runtime/point/PointDatasetRuntime.h>

#include <pci/operations/local/LocalRasterColorizeRunStore.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
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
makeScaleScene(const std::uint32_t copies)
{
    constexpr std::uint32_t width = 2000;
    constexpr std::uint32_t height = 1000;
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
                                const std::filesystem::path &temporaryDirectory)
{
    auto scene = makeScaleScene(copies);
    auto raster = std::make_shared<ScaleRasterSource>(rasterSize);
    auto budget =
        std::make_shared<pci::PointMemoryBudget>(512ULL * 1024 * 1024);
    pci::RasterColorizeOptions options{
        .workerCount = 4,
        .maximumScatterRecords = 100'000,
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

TEST_CASE("four-million-point raster bake remains footprint bounded",
          "[integration][stress][scale][colorize]")
{
    TemporaryDirectory directory;
    const ScaleOutcome twoMillion = bake(1, 5000, directory.path());
    CHECK(directory.empty());
    const ScaleOutcome fourMillion = bake(2, 5000, directory.path());
    CHECK(directory.empty());
    const ScaleOutcome sparseExtent = bake(1, 100'000, directory.path());
    CHECK(directory.empty());

    CHECK(twoMillion.statistics.pointsConsidered == 2'000'000);
    CHECK(fourMillion.statistics.pointsConsidered == 4'000'000);
    CHECK(fourMillion.statistics.pointsColored == 4'000'000);
    CHECK(twoMillion.distinctTiles == fourMillion.distinctTiles);
    CHECK(sparseExtent.distinctTiles == twoMillion.distinctTiles);
    const auto checkTileReadsBounded = [](const ScaleOutcome &outcome) {
        CHECK(outcome.statistics.tileReads >= outcome.distinctTiles);
        CHECK(outcome.statistics.tileReads <=
              outcome.distinctTiles * pci::rasterColorizeTileReadOverheadBound);
    };
    checkTileReadsBounded(twoMillion);
    checkTileReadsBounded(fourMillion);
    checkTileReadsBounded(sparseExtent);
    CHECK(twoMillion.displayReads == 0);
    CHECK(fourMillion.displayReads == 0);
    CHECK(sparseExtent.displayReads == 0);
    CHECK(fourMillion.detachedMaximumHandles == 1);
}

TEST_CASE("four-million-point raster bake cancels promptly and cleans runs",
          "[integration][stress][scale][colorize][cancellation]")
{
    TemporaryDirectory directory;
    auto scene = makeScaleScene(2);
    auto raster = std::make_shared<ScaleRasterSource>(5000);
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
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    auto future = std::async(
        std::launch::async,
        [preflight = std::move(preflight),
         raster,
         budget,
         options,
         stopToken = stop.get_token(),
         entered = std::move(entered)]() mutable {
            entered.set_value();
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
    enteredFuture.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto cancelledAt = std::chrono::steady_clock::now();
    stop.request_stop();
    const auto result = future.get();
    const auto elapsed = std::chrono::steady_clock::now() - cancelledAt;
    CHECK_FALSE(result.has_value());
    CHECK(elapsed < std::chrono::milliseconds(500));
    CHECK(budget->reservedBytes() == 0);
    CHECK(directory.empty());
}

} // namespace
