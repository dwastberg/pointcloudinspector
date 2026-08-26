#include "scene/RasterPointColorizer.h"

#include "scene/PointCloudScene.h"
#include "scene/RasterColorizedPointSource.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QTemporaryDir>

#include <atomic>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory()
        : path_(QDir(directory_.path()).filesystemPath())
    {
        if (!directory_.isValid()) {
            throw std::runtime_error(
                "could not create private colorizer fixture");
        }
    }
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

private:
    QTemporaryDir directory_;
    std::filesystem::path path_;
};

class GridRasterSource final : public pci::RasterTileSource {
public:
    explicit GridRasterSource(const std::uint8_t bias,
                              const bool transparentMiddle = false)
        : bias_(bias)
        , transparentMiddle_(transparentMiddle)
    {
        metadata_.width = 4;
        metadata_.height = 4;
        metadata_.geoTransform = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    }

    [[nodiscard]] const pci::RasterLayerMetadata &
    metadata() const noexcept override
    {
        return metadata_;
    }

    [[nodiscard]] pci::RasterTileData
    readTile(const pci::RasterTileRequest &request,
             const std::stop_token stop) const override
    {
        if (stop.stop_requested()) {
            throw pci::RasterReadCancelled();
        }
        ++reads_;
        pci::RasterTileData result;
        result.key = request.key;
        result.renderGeneration = request.renderGeneration;
        result.validWidth = 4;
        result.validHeight = 4;
        result.rgba.resize(pci::rasterStoredTileBytes);
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const std::uint32_t offset =
                    ((y + pci::rasterTileGutter) * pci::rasterStoredTilePixels +
                     x + pci::rasterTileGutter) *
                    4U;
                result.rgba[offset + 0] = static_cast<std::byte>(bias_ + x);
                result.rgba[offset + 1] = static_cast<std::byte>(bias_ + y);
                result.rgba[offset + 2] = std::byte{9};
                result.rgba[offset + 3] = transparentMiddle_ && x == 1 && y == 1
                                              ? std::byte{0}
                                              : std::byte{255};
            }
        }
        return result;
    }

    [[nodiscard]] std::uint64_t reads() const noexcept
    {
        return reads_.load();
    }

private:
    pci::RasterLayerMetadata metadata_;
    std::uint8_t bias_ = 0;
    bool transparentMiddle_ = false;
    mutable std::atomic_uint64_t reads_ = 0;
};

class TiledRasterSource final : public pci::RasterTileSource {
public:
    TiledRasterSource()
    {
        metadata_.width = 2048;
        metadata_.height = 2048;
        metadata_.geoTransform = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    }

    [[nodiscard]] const pci::RasterLayerMetadata &
    metadata() const noexcept override
    {
        return metadata_;
    }

    [[nodiscard]] pci::RasterTileData
    readTile(const pci::RasterTileRequest &request,
             const std::stop_token stop) const override
    {
        if (stop.stop_requested()) {
            throw pci::RasterReadCancelled();
        }
        ++reads_;
        pci::RasterTileData result;
        result.key = request.key;
        result.renderGeneration = request.renderGeneration;
        result.validWidth = pci::rasterTilePixels;
        result.validHeight = pci::rasterTilePixels;
        result.rgba.resize(pci::rasterStoredTileBytes);
        for (std::uint32_t y = 0; y < pci::rasterTilePixels; ++y) {
            for (std::uint32_t x = 0; x < pci::rasterTilePixels; ++x) {
                const std::uint32_t offset =
                    ((y + pci::rasterTileGutter) * pci::rasterStoredTilePixels +
                     x + pci::rasterTileGutter) *
                    4U;
                result.rgba[offset + 0] = static_cast<std::byte>(request.key.x);
                result.rgba[offset + 1] = static_cast<std::byte>(request.key.y);
                result.rgba[offset + 2] = std::byte{42};
                result.rgba[offset + 3] = std::byte{255};
            }
        }
        return result;
    }

    [[nodiscard]] std::uint64_t reads() const noexcept
    {
        return reads_.load();
    }

private:
    pci::RasterLayerMetadata metadata_;
    mutable std::atomic_uint64_t reads_ = 0;
};

class HierarchySource final : public pci::PointCloudDataSource {
public:
    explicit HierarchySource(pci::PointCloudNodePayloadPtr root)
        : root_(std::move(root))
    {
    }

    [[nodiscard]] pci::PointCloudNode rootNode() const override
    {
        return node(pci::rootPointCloudNode);
    }

    [[nodiscard]] pci::PointCloudNode
    node(const pci::PointCloudNodeId id) const override
    {
        return {.id = id,
                .bounds = root_->blocks.front()->bounds,
                .estimatedPointCount = pci::pointCloudNodePayloadPoints(*root_),
                .leaf = true};
    }

    [[nodiscard]] pci::PointCloudNodePayloadPtr
    loadNode(const pci::PointCloudNodeId,
             const std::stop_token stop) const override
    {
        if (stop.stop_requested()) {
            throw pci::PointCloudDataSourceCancelled();
        }
        ++loads_;
        return root_;
    }

    [[nodiscard]] pci::PointCloudStorageMetrics storageMetrics() const override
    {
        return {.localPersistent = true, .committed = true};
    }

    [[nodiscard]] std::optional<std::vector<pci::PointCloudStoredNode>>
    storedNodeIndex() const override
    {
        return std::vector<pci::PointCloudStoredNode>{{
            .id = pci::rootPointCloudNode,
            .bounds = root_->blocks.front()->bounds,
            .pointCount = pci::pointCloudNodePayloadPoints(*root_),
        }};
    }

    [[nodiscard]] std::uint64_t loads() const noexcept
    {
        return loads_.load();
    }

private:
    pci::PointCloudNodePayloadPtr root_;
    mutable std::atomic_uint64_t loads_ = 0;
};

[[nodiscard]] pci::PointCloudNodePayloadPtr
hierarchyRoot(const std::vector<std::uint32_t> &sourceColors)
{
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = {0.0, 0.0, 0.0};
    block->scale = 1.0;
    block->bounds = {.minimum = {0.0, 0.0, 0.0}, .maximum = {3.0, 0.0, 0.0}};
    for (std::size_t index = 0; index < sourceColors.size(); ++index) {
        block->points.push_back({.x = static_cast<std::uint16_t>(index),
                                 .y = 0,
                                 .z = 0,
                                 .rgba = sourceColors[index]});
    }
    auto payload = std::make_shared<pci::PointCloudNodePayload>();
    payload->nodeId = pci::rootPointCloudNode;
    payload->blocks.push_back(std::move(block));
    payload->sourcePointCount = sourceColors.size();
    return payload;
}

[[nodiscard]] pci::PointMemoryBudget::ReservationPtr
reserve(const pci::PointMemoryBudgetPtr &budget, const std::uint64_t bytes)
{
    const auto result = budget->tryReserve(bytes);
    REQUIRE(result.has_value());
    return *result;
}

[[nodiscard]] std::uint32_t expectedColor(const std::uint8_t bias,
                                          const std::uint8_t x,
                                          const std::uint8_t y)
{
    return 0xff000000U | (9U << 16U) |
           (static_cast<std::uint32_t>(bias + y) << 8U) |
           static_cast<std::uint32_t>(bias + x);
}

[[nodiscard]] std::optional<pci::RasterColorizePreparedPtr>
colorize(const std::shared_ptr<pci::PointCloudScene> &scene,
         const pci::RasterTileSourcePtr &raster,
         const pci::PointMemoryBudgetPtr &budget,
         const std::filesystem::path &temporaryDirectory,
         std::vector<pci::RasterColorizeProgress> *progress = nullptr,
         const std::uint32_t workerCount = 1,
         const std::uint64_t maximumScatterRecords = 1)
{
    auto target = scene->rasterPointColorizeTarget();
    REQUIRE(target.has_value());
    pci::RasterColorizeOptions options{
        .workerCount = workerCount,
        .maximumScatterRecords = maximumScatterRecords,
        .temporaryDirectory = temporaryDirectory,
    };
    pci::RasterColorizePreflight preflight = pci::preflightRasterPointColorize(
        std::move(*target), raster->metadata(), options);
    const std::uint64_t colorBytes =
        preflight.tableEntries * sizeof(std::uint32_t);
    auto colorReservation = reserve(budget, colorBytes);
    auto workingReservation =
        reserve(budget, preflight.workingReservationBytes);
    auto rootReservation =
        reserve(budget, preflight.rootStagingReservationBytes);
    auto flatReservation =
        reserve(budget, preflight.flatStagingReservationBytes);
    return pci::colorizePointCloudFromRaster(
        std::move(preflight),
        raster,
        std::make_shared<pci::RasterDecodeParameters>(),
        11,
        std::move(options),
        std::move(colorReservation),
        std::move(workingReservation),
        std::move(rootReservation),
        std::move(flatReservation),
        {},
        [progress](const pci::RasterColorizeProgress value) {
            if (progress) {
                progress->push_back(value);
            }
        });
}

TEST_CASE("flat raster colorization applies rebakes and reverts atomically",
          "[unit][scene][colorize]")
{
    constexpr std::uint32_t source0 = 0xff332211U;
    constexpr std::uint32_t source1 = 0xff665544U;
    constexpr std::uint32_t source2 = 0xff998877U;
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = {0.0, 0.0, 0.0};
    block->scale = 1.0;
    block->bounds.minimum = {0.0, 0.0, 0.0};
    block->bounds.maximum = {2.0, 2.0, 0.0};
    block->points = {
        {.x = 0, .y = 0, .z = 0, .rgba = source0},
        {.x = 1, .y = 1, .z = 0, .rgba = source1},
        {.x = 2, .y = 2, .z = 0, .rgba = source2},
    };

    auto scene = std::make_shared<pci::PointCloudScene>(
        pci::PointCloudMetadata{.sourcePointCount = 3});
    scene->addBlock(block);
    scene->markLoadingComplete();
    const auto originalEntry = scene->blockEntries().front();
    const auto budget =
        std::make_shared<pci::PointMemoryBudget>(64ULL * 1024 * 1024);
    TemporaryDirectory directory;

    auto firstRaster = std::make_shared<GridRasterSource>(std::uint8_t{10});
    std::vector<pci::RasterColorizeProgress> progress;
    const auto first =
        colorize(scene, firstRaster, budget, directory.path(), &progress);
    REQUIRE(first.has_value());
    REQUIRE(*first);
    CHECK((*first)->statistics.pointsConsidered == 3);
    CHECK((*first)->statistics.pointsColored == 3);
    CHECK((*first)->statistics.pointsUnchanged == 0);
    CHECK((*first)->statistics.tileReads == 1);
    CHECK((*first)->statistics.temporaryBytesWritten == 36);
    CHECK(firstRaster->reads() == 1);
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.back().phase == pci::RasterColorizePhase::SamplingRaster);
    CHECK(progress.back().completed == progress.back().total);

    CHECK(scene->applyRasterPointColors(*first) ==
          pci::RasterPointColorApplyOutcome::Applied);
    CHECK(scene->hasRasterPointColors());
    const auto bakedEntry = scene->blockEntries().front();
    CHECK(bakedEntry.id == originalEntry.id);
    CHECK(bakedEntry.block != originalEntry.block);
    REQUIRE(bakedEntry.block->points.size() == 3);
    CHECK(bakedEntry.block->points[0].rgba == expectedColor(10, 0, 0));
    CHECK(bakedEntry.block->points[1].rgba == expectedColor(10, 1, 1));
    CHECK(bakedEntry.block->points[2].rgba == expectedColor(10, 2, 2));

    auto secondRaster =
        std::make_shared<GridRasterSource>(std::uint8_t{20}, true);
    const auto second = colorize(scene, secondRaster, budget, directory.path());
    REQUIRE(second.has_value());
    REQUIRE(*second);
    CHECK((*second)->statistics.pointsColored == 2);
    CHECK((*second)->statistics.pointsUnchanged == 1);
    CHECK(scene->applyRasterPointColors(*second) ==
          pci::RasterPointColorApplyOutcome::Applied);
    const auto rebakedEntry = scene->blockEntries().front();
    CHECK(rebakedEntry.id == originalEntry.id);
    CHECK(rebakedEntry.block->points[0].rgba == expectedColor(20, 0, 0));
    // Transparency restores source color, not the prior raster bake.
    CHECK(rebakedEntry.block->points[1].rgba == source1);
    CHECK(rebakedEntry.block->points[2].rgba == expectedColor(20, 2, 2));

    CHECK(scene->revertPointColors() ==
          pci::RasterPointColorApplyOutcome::Applied);
    CHECK_FALSE(scene->hasRasterPointColors());
    const auto revertedEntry = scene->blockEntries().front();
    CHECK(revertedEntry.id == originalEntry.id);
    REQUIRE(revertedEntry.block->points.size() == 3);
    CHECK(revertedEntry.block->points[0].rgba == source0);
    CHECK(revertedEntry.block->points[1].rgba == source1);
    CHECK(revertedEntry.block->points[2].rgba == source2);
    CHECK(budget->reservedBytes() == 0);
}

TEST_CASE("cancelled raster colorization produces no prepared transaction",
          "[unit][scene][colorize]")
{
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = {0.0, 0.0, 0.0};
    block->scale = 1.0;
    block->bounds.minimum = {0.0, 0.0, 0.0};
    block->bounds.maximum = {0.0, 0.0, 0.0};
    block->points = {{.x = 0, .y = 0, .z = 0, .rgba = 0xff123456U}};
    auto scene = std::make_shared<pci::PointCloudScene>(
        pci::PointCloudMetadata{.sourcePointCount = 1});
    scene->addBlock(block);
    scene->markLoadingComplete();
    auto raster = std::make_shared<GridRasterSource>(std::uint8_t{0});
    auto budget = std::make_shared<pci::PointMemoryBudget>(64ULL * 1024 * 1024);
    TemporaryDirectory directory;
    auto target = scene->rasterPointColorizeTarget();
    REQUIRE(target.has_value());
    pci::RasterColorizeOptions options{
        .workerCount = 1,
        .maximumScatterRecords = 1,
        .temporaryDirectory = directory.path(),
    };
    auto preflight = pci::preflightRasterPointColorize(
        std::move(*target), raster->metadata(), options);
    std::stop_source stop;
    stop.request_stop();
    const auto result = pci::colorizePointCloudFromRaster(
        preflight,
        raster,
        std::make_shared<pci::RasterDecodeParameters>(),
        1,
        options,
        reserve(budget, preflight.tableEntries * sizeof(std::uint32_t)),
        reserve(budget, preflight.workingReservationBytes),
        reserve(budget, preflight.rootStagingReservationBytes),
        reserve(budget, preflight.flatStagingReservationBytes),
        stop.get_token(),
        {});
    CHECK_FALSE(result.has_value());
    CHECK(scene->blockEntries().front().block == block);
    CHECK_FALSE(scene->hasRasterPointColors());
    CHECK(raster->reads() == 0);
    CHECK(budget->reservedBytes() == 0);
}

TEST_CASE("raster colorization rejects unbounded run fan-in before writing",
          "[unit][scene][colorize]")
{
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = {0.0, 0.0, 0.0};
    block->scale = 1.0;
    block->bounds.minimum = {0.0, 0.0, 0.0};
    block->bounds.maximum = {0.0, 0.0, 0.0};
    block->points.resize(257, {.x = 0, .y = 0, .z = 0, .rgba = 0xff123456U});
    auto scene = std::make_shared<pci::PointCloudScene>(
        pci::PointCloudMetadata{.sourcePointCount = block->points.size()});
    scene->addBlock(std::move(block));
    scene->markLoadingComplete();

    auto raster = std::make_shared<GridRasterSource>(std::uint8_t{0});
    auto budget = std::make_shared<pci::PointMemoryBudget>(64ULL * 1024 * 1024);
    TemporaryDirectory directory;
    auto target = scene->rasterPointColorizeTarget();
    REQUIRE(target.has_value());
    pci::RasterColorizeOptions options{
        .workerCount = 4,
        .maximumScatterRecords = 1,
        .temporaryDirectory = directory.path(),
    };
    auto preflight = pci::preflightRasterPointColorize(
        std::move(*target), raster->metadata(), options);

    bool rejected = false;
    try {
        static_cast<void>(pci::colorizePointCloudFromRaster(
            preflight,
            raster,
            std::make_shared<pci::RasterDecodeParameters>(),
            1,
            options,
            reserve(budget, preflight.tableEntries * sizeof(std::uint32_t)),
            reserve(budget, preflight.workingReservationBytes),
            reserve(budget, preflight.rootStagingReservationBytes),
            reserve(budget, preflight.flatStagingReservationBytes),
            {},
            {}));
    } catch (const pci::RasterColorizeError &error) {
        rejected = true;
        CHECK(error.code() ==
              pci::RasterColorizeFailureCode::InsufficientPointMemory);
    }
    CHECK(rejected);
    CHECK(std::filesystem::is_empty(directory.path()));
    CHECK(raster->reads() == 0);
    CHECK(budget->reservedBytes() == 0);
}

TEST_CASE("parallel raster sampling reads each addressed tile once",
          "[unit][scene][colorize]")
{
    const auto makeScene = [] {
        auto block = std::make_shared<pci::PointBlock>();
        block->origin = {0.0, 0.0, 0.0};
        block->scale = 1.0;
        block->bounds.minimum = {0.0, 0.0, 0.0};
        block->bounds.maximum = {1792.0, 1792.0, 0.0};
        for (std::uint16_t tileY = 0; tileY < 8; ++tileY) {
            for (std::uint16_t tileX = 0; tileX < 8; ++tileX) {
                block->points.push_back(
                    {.x = static_cast<std::uint16_t>(tileX * 256U),
                     .y = static_cast<std::uint16_t>(tileY * 256U),
                     .z = 0,
                     .rgba = 0xff010203U});
            }
        }
        auto scene = std::make_shared<pci::PointCloudScene>(
            pci::PointCloudMetadata{.sourcePointCount = 64});
        scene->addBlock(std::move(block));
        scene->markLoadingComplete();
        return scene;
    };

    TemporaryDirectory directory;
    auto oneScene = makeScene();
    auto fourScene = makeScene();
    auto oneRaster = std::make_shared<TiledRasterSource>();
    auto fourRaster = std::make_shared<TiledRasterSource>();
    auto oneBudget =
        std::make_shared<pci::PointMemoryBudget>(64ULL * 1024 * 1024);
    auto fourBudget =
        std::make_shared<pci::PointMemoryBudget>(64ULL * 1024 * 1024);

    const auto one = colorize(
        oneScene, oneRaster, oneBudget, directory.path(), nullptr, 1, 3);
    const auto four = colorize(
        fourScene, fourRaster, fourBudget, directory.path(), nullptr, 4, 3);
    REQUIRE(one.has_value());
    REQUIRE(four.has_value());
    REQUIRE(*one);
    REQUIRE(*four);
    CHECK((*one)->statistics.pointsColored == 64);
    CHECK((*four)->statistics.pointsColored == 64);
    CHECK((*one)->statistics.tileReads == 64);
    CHECK((*four)->statistics.tileReads == 64);
    CHECK((*one)->statistics.temporaryBytesWritten == 64 * 12);
    CHECK((*one)->statistics.temporaryBytesWritten ==
          (*four)->statistics.temporaryBytesWritten);
    CHECK(oneRaster->reads() == 64);
    CHECK(fourRaster->reads() == 64);

    const auto &oneFlat =
        std::get<pci::RasterColorizePreparedFlat>((*one)->data);
    const auto &fourFlat =
        std::get<pci::RasterColorizePreparedFlat>((*four)->data);
    REQUIRE(oneFlat.replacementBlocks.size() == 1);
    REQUIRE(fourFlat.replacementBlocks.size() == 1);
    CHECK(oneFlat.replacementBlocks.front().block->points ==
          fourFlat.replacementBlocks.front().block->points);
}

TEST_CASE(
    "hierarchy colorization decorates stored pages and pairs root reduction",
    "[unit][scene][colorize][hierarchy]")
{
    const std::vector<std::uint32_t> sourceColors{
        0xff000001U, 0xff000002U, 0xff000003U, 0xff000004U};
    const pci::PointCloudNodePayloadPtr root = hierarchyRoot(sourceColors);
    auto source = std::make_shared<HierarchySource>(root);
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = sourceColors.size();
    metadata.sourceBounds = root->blocks.front()->bounds;
    auto scene = std::make_shared<pci::PointCloudScene>(
        metadata, source, root, 1024 * 1024);
    auto raster = std::make_shared<GridRasterSource>(std::uint8_t{10});
    auto budget = std::make_shared<pci::PointMemoryBudget>(64ULL * 1024 * 1024);
    TemporaryDirectory directory;

    const auto result =
        colorize(scene, raster, budget, directory.path(), nullptr, 2, 2);
    REQUIRE(result.has_value());
    REQUIRE(*result);
    CHECK((*result)->statistics.pointsConsidered == 4);
    CHECK((*result)->statistics.pointsColored == 4);
    CHECK((*result)->statistics.tileReads == 1);
    CHECK(source->loads() == 1);
    auto &prepared =
        std::get<pci::RasterColorizePreparedHierarchy>((*result)->data);
    auto decorated = std::dynamic_pointer_cast<pci::RasterColorizedPointSource>(
        prepared.colorizedSource);
    REQUIRE(decorated != nullptr);
    const auto coloredPage = decorated->loadNode(pci::rootPointCloudNode, {});
    REQUIRE(coloredPage != root);
    CHECK(coloredPage->blocks.front()->points.front().rgba ==
          expectedColor(10, 0, 0));
    decorated.reset();

    CHECK(scene->applyRasterPointColors(*result) ==
          pci::RasterPointColorApplyOutcome::Applied);
    CHECK(scene->hasRasterPointColors());
    const pci::RasterPointColorMetrics before =
        scene->rasterPointColorMetrics();
    CHECK(before.activeColorTableBytes >= 8 * sizeof(std::uint32_t));
    CHECK(before.retainedSourceRootBytes == 4 * sizeof(pci::GpuPoint));
    CHECK(before.retainedColoredRootBytes == 4 * sizeof(pci::GpuPoint));
    CHECK(scene->minimumRootPayloadBytes() == 2 * sizeof(pci::GpuPoint));

    CHECK(scene->limitRootPayloadBytes(4 * sizeof(pci::GpuPoint)) ==
          4 * sizeof(pci::GpuPoint));
    const pci::RasterPointColorMetrics reduced =
        scene->rasterPointColorMetrics();
    CHECK(reduced.retainedSourceRootBytes == 2 * sizeof(pci::GpuPoint));
    CHECK(reduced.retainedColoredRootBytes == 2 * sizeof(pci::GpuPoint));
    const auto reducedTarget = scene->rasterPointColorizeTarget();
    REQUIRE(reducedTarget.has_value());
    const auto &sourceRoot =
        std::get<pci::RasterColorizeHierarchicalTarget>(reducedTarget->data)
            .sourceRootPayload;
    const auto activeRoot = scene->peekNodePayload(pci::rootPointCloudNode);
    REQUIRE(sourceRoot);
    REQUIRE(activeRoot);
    REQUIRE(sourceRoot->blocks.front()->points.size() == 2);
    REQUIRE(activeRoot->blocks.front()->points.size() == 2);
    CHECK(sourceRoot->blocks.front()->points[0].x == 0);
    CHECK(sourceRoot->blocks.front()->points[1].x == 2);
    CHECK(activeRoot->blocks.front()->points[0].x == 0);
    CHECK(activeRoot->blocks.front()->points[1].x == 2);
    CHECK(sourceRoot->blocks.front()->points[0].rgba == sourceColors[0]);
    CHECK(sourceRoot->blocks.front()->points[1].rgba == sourceColors[2]);
    CHECK(activeRoot->blocks.front()->points[0].rgba ==
          expectedColor(10, 0, 0));
    CHECK(activeRoot->blocks.front()->points[1].rgba ==
          expectedColor(10, 2, 0));

    CHECK(scene->revertPointColors() ==
          pci::RasterPointColorApplyOutcome::Applied);
    CHECK_FALSE(scene->hasRasterPointColors());
    const auto reverted = scene->peekNodePayload(pci::rootPointCloudNode);
    REQUIRE(reverted);
    CHECK(reverted->blocks.front()->points[0].rgba == sourceColors[0]);
    CHECK(reverted->blocks.front()->points[1].rgba == sourceColors[2]);
    CHECK(budget->reservedBytes() == 0);
}

TEST_CASE(
    "hierarchy color decorator forwards misses and fails count mismatch closed",
    "[unit][scene][colorize][hierarchy]")
{
    const auto root = hierarchyRoot({0xff000001U, 0xff000002U});
    auto source = std::make_shared<HierarchySource>(root);
    auto budget = std::make_shared<pci::PointMemoryBudget>(1024);
    const auto reservation = budget->tryReserve(2 * sizeof(std::uint32_t));
    REQUIRE(reservation.has_value());
    pci::RasterColorizedPointSource noRange(source, {1, 2}, {}, *reservation);
    CHECK(noRange.loadNode(pci::rootPointCloudNode, {}) == root);

    const auto secondReservation =
        budget->tryReserve(2 * sizeof(std::uint32_t));
    REQUIRE(secondReservation.has_value());
    pci::RasterColorizedPointSource mismatch(
        source,
        {1, 2},
        {{.node = pci::rootPointCloudNode, .offset = 0, .count = 1}},
        *secondReservation);
    CHECK(mismatch.loadNode(pci::rootPointCloudNode, {}) == root);
    CHECK(mismatch.countMismatchCount() == 1);
}

} // namespace
