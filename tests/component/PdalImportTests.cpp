#include "fixtures/PdalFixtureFactory.h"
#include <pci/adapters/pdal/LocalPageBuildInfrastructure.h>
#include <pci/adapters/pdal/LocalPointIndexBuilder.h>
#include <pci/adapters/pdal/LocalPointPageFormat.h>
#include <pci/adapters/pdal/PdalHierarchicalPointSource.h>
#include <pci/adapters/pdal/PdalPointCloudLoader.h>
#include <pci/adapters/pdal/PdalPointCloudStatistics.h>
#include <pci/adapters/pdal/PdalSourceInspector.h>
#include <pci/operations/PointCloudImport.h>
#include <pci/operations/PointDatasetInstallation.h>
#include <pci/operations/local/LocalStorageMaintenance.h>
#include <pci/pointcloud/GpuPointProperties.h>
#include <pci/pointcloud/PointBlock.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <QByteArrayView>
#include <QCryptographicHash>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace {

class FixtureDirectory {
public:
    FixtureDirectory()
        : directory_(std::filesystem::temp_directory_path(), "pcinspector-pdal")
        , root_(std::filesystem::canonical(directory_.path()))
        , paths_(pci::test::writePdalFixtures(root_))
    {
    }

    [[nodiscard]] const pci::test::PdalFixturePaths &paths() const noexcept
    {
        return paths_;
    }

    [[nodiscard]] const std::filesystem::path &directory() const noexcept
    {
        return root_;
    }

private:
    pci::PrivateTemporaryDirectory directory_;
    std::filesystem::path root_;
    pci::test::PdalFixturePaths paths_;
};

[[nodiscard]] pci::LocalPageCacheContextPtr
testCache(const FixtureDirectory &fixture, const std::string_view name)
{
    return pci::LocalPageCacheContext::createPersistent(
        fixture.directory() / name,
        fixture.directory() / "cache-configuration");
}

void checkMetadata(const pci::PointCloudMetadata &metadata,
                   const std::string_view expectedDriver)
{
    CHECK(metadata.sourcePointCount == pci::test::fixturePoints.size());
    CHECK(metadata.sourceBounds.minimum == std::array{1000.0, 2000.0, 10.0});
    CHECK(metadata.sourceBounds.maximum == std::array{1010.0, 2010.0, 20.0});
    CHECK(metadata.sourceDriver == expectedDriver);
    CHECK_FALSE(metadata.spatialReferenceWkt.empty());
    CHECK(metadata.hasColor);
    CHECK(metadata.hasIntensity);
    CHECK(metadata.hasClassification);
    CHECK(metadata.hasReturnNumber);
    CHECK(metadata.hasNumberOfReturns);
}

std::vector<pci::GpuPoint>
sortedPoints(const pci::PointDatasetRuntimePtr &scene)
{
    std::vector<pci::GpuPoint> points;
    for (const auto &block : scene->blocks()) {
        points.insert(points.end(), block->points.begin(), block->points.end());
    }
    std::ranges::sort(
        points, [](const pci::GpuPoint &a, const pci::GpuPoint &b) {
            return std::tie(a.x,
                            a.y,
                            a.z,
                            a.rgba,
                            a.attributes,
                            a.packedProperties) <
                   std::tie(
                       b.x, b.y, b.z, b.rgba, b.attributes, b.packedProperties);
        });
    return points;
}

// Expected colors are literal app RGBA values, independent of PDAL mapping.
constexpr std::array<std::uint32_t, 8> fixtureColors{0xff0000ffU,
                                                     0xff00ff00U,
                                                     0xffff0000U,
                                                     0xff00ffffU,
                                                     0xffff00ffU,
                                                     0xffffff00U,
                                                     0xff808080U,
                                                     0xffffffffU};

void checkPointBlocks(const std::span<const pci::PointBlockPtr> blocks,
                      const std::span<const pci::test::FixturePoint> expected,
                      const std::span<const std::uint32_t> colors)
{
    REQUIRE(colors.size() == expected.size());
    std::vector<bool> seen(expected.size(), false);
    std::size_t count = 0;
    for (const auto &block : blocks) {
        REQUIRE(block);
        REQUIRE(block->attributes.size() == block->points.size());
        for (std::size_t index = 0; index < block->points.size(); ++index) {
            const auto &point = block->points[index];
            const auto position = pci::decodeBlockPosition(*block, point);
            const double tolerance = block->scale * 0.5 + 1e-8;
            std::size_t match = expected.size();
            for (std::size_t candidate = 0; candidate < expected.size();
                 ++candidate) {
                const auto &value = expected[candidate];
                if (!seen[candidate] &&
                    std::abs(position.x - value.x) <= tolerance &&
                    std::abs(position.y - value.y) <= tolerance &&
                    std::abs(position.z - value.z) <= tolerance) {
                    match = candidate;
                    break;
                }
            }
            CAPTURE(position.x, position.y, position.z);
            REQUIRE(match < expected.size());
            seen[match] = true;
            ++count;
            const auto &value = expected[match];
            const auto &attributes = block->attributes[index];
            CHECK(point.rgba == colors[match]);
            CHECK(attributes.intensity == value.intensity);
            CHECK(attributes.classification == value.classification);
            CHECK(attributes.returnNumber == value.returnNumber);
            CHECK(attributes.numberOfReturns == value.numberOfReturns);
            CHECK((point.attributes & 0xffU) == value.classification);
            CHECK((point.attributes >> 8U) == (value.intensity >> 8U));
            CHECK(pci::gpuPointIntensity(point.packedProperties) ==
                  value.intensity);
            CHECK(pci::gpuPointReturnNumber(point.packedProperties) ==
                  value.returnNumber);
            CHECK(pci::gpuPointNumberOfReturns(point.packedProperties) ==
                  value.numberOfReturns);
        }
    }
    CHECK(count == expected.size());
    CHECK(std::ranges::all_of(seen, [](const bool value) {
        return value;
    }));
}

void checkFixtureBlocks(const std::span<const pci::PointBlockPtr> blocks)
{
    checkPointBlocks(blocks, pci::test::fixturePoints, fixtureColors);
}

std::vector<pci::PointBlockPtr>
leafBlocks(const pci::LocalPointPageSourcePtr &source)
{
    const auto detail = source->fullDetailInfo();
    REQUIRE(detail);
    std::vector<pci::PointBlockPtr> blocks;
    for (const auto id : detail->leafNodes) {
        const auto payload = source->loadNode(id, {});
        REQUIRE(payload);
        blocks.insert(
            blocks.end(), payload->blocks.begin(), payload->blocks.end());
    }
    return blocks;
}

std::vector<std::byte> fileBytes(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    REQUIRE(input);
    const auto size = input.tellg();
    REQUIRE(size >= 0);
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!result.empty()) {
        input.read(reinterpret_cast<char *>(result.data()),
                   static_cast<std::streamsize>(result.size()));
    }
    REQUIRE(input);
    return result;
}

std::string sha256(const std::filesystem::path &path)
{
    const std::vector<std::byte> bytes = fileBytes(path);
    return QCryptographicHash::hash(
               QByteArrayView(reinterpret_cast<const char *>(bytes.data()),
                              static_cast<qsizetype>(bytes.size())),
               QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

TEST_CASE("PDAL inspection identifies supported point-cloud formats",
          "[component][pdal]")
{
    const FixtureDirectory fixture;
    const pci::PdalSourceInspector inspector;

    checkMetadata(inspector.inspect(fixture.paths().las), "readers.las");
    checkMetadata(inspector.inspect(fixture.paths().laz), "readers.las");
    checkMetadata(inspector.inspect(fixture.paths().copc), "readers.copc");
}

TEST_CASE("PDAL inspection errors identify the requested path",
          "[component][pdal]")
{
    const pci::PdalSourceInspector inspector;
    const std::filesystem::path path = "missing-cloud.unknown";

    try {
        static_cast<void>(inspector.inspect(path));
        FAIL("inspection unexpectedly succeeded");
    } catch (const pci::PointCloudImportError &error) {
        CHECK(std::string_view(error.what()).contains(path.string()));
    }
}

TEST_CASE("point-cloud statistics scan coordinates, density, attributes, and "
          "outliers",
          "[component][pdal][statistics]")
{
    const FixtureDirectory fixture;
    const pci::PdalSourceInspector inspector;
    const pci::PointCloudMetadata metadata =
        inspector.inspect(fixture.paths().las);
    const pci::PdalPointCloudStatistics calculator;
    std::uint64_t finalProgress = 0;
    const pci::PointCloudStatistics statistics = calculator.calculate(
        metadata,
        {},
        [&finalProgress](const std::uint64_t processed, const std::uint64_t) {
            finalProgress = processed;
        });

    CHECK(statistics.sourcePointCount == 8);
    CHECK(statistics.scannedPointCount == 8);
    CHECK(finalProgress == 8);
    CHECK(statistics.x.minimum == Catch::Approx(1000.0));
    CHECK(statistics.x.maximum == Catch::Approx(1010.0));
    CHECK(statistics.x.mean == Catch::Approx(1005.0));
    CHECK(statistics.x.standardDeviation == Catch::Approx(5.0));
    CHECK(statistics.y.mean == Catch::Approx(2005.0));
    CHECK(statistics.z.mean == Catch::Approx(15.0));
    REQUIRE(statistics.horizontalBoundingArea);
    CHECK(*statistics.horizontalBoundingArea == Catch::Approx(100.0));
    REQUIRE(statistics.boundingVolume);
    CHECK(*statistics.boundingVolume == Catch::Approx(1000.0));
    REQUIRE(statistics.horizontalDensity);
    CHECK(*statistics.horizontalDensity == Catch::Approx(0.08));
    REQUIRE(statistics.volumetricDensity);
    CHECK(*statistics.volumetricDensity == Catch::Approx(0.008));
    REQUIRE(statistics.nominalHorizontalSpacing);
    CHECK(*statistics.nominalHorizontalSpacing ==
          Catch::Approx(std::sqrt(12.5)));

    REQUIRE(statistics.intensity);
    CHECK(statistics.intensity->minimum == Catch::Approx(100.0));
    CHECK(statistics.intensity->maximum == Catch::Approx(800.0));
    CHECK(statistics.intensity->mean == Catch::Approx(450.0));
    REQUIRE(statistics.red);
    REQUIRE(statistics.green);
    REQUIRE(statistics.blue);
    CHECK(statistics.red->minimum == Catch::Approx(0.0));
    CHECK(statistics.red->maximum == Catch::Approx(65535.0));
    CHECK(statistics.classificationCounts[1] == 2);
    CHECK(statistics.classificationCounts[2] == 2);
    CHECK(statistics.classificationCounts[5] == 2);
    CHECK(statistics.classificationCounts[6] == 2);
    CHECK(statistics.returnNumberCounts[1] == 4);
    CHECK(statistics.numberOfReturnsCounts[1] == 1);
    CHECK(statistics.numberOfReturnsCounts[2] == 2);
    CHECK(statistics.numberOfReturnsCounts[3] == 3);
    CHECK(statistics.numberOfReturnsCounts[4] == 2);

    REQUIRE(statistics.spatialOutliers);
    CHECK(statistics.spatialOutliers->samplePointCount == 8);
    CHECK(statistics.spatialOutliers->neighbourCount == 7);
    CHECK(statistics.spatialOutliers->sampleOutlierCount == 0);

    std::stop_source stopped;
    stopped.request_stop();
    CHECK_THROWS_AS(calculator.calculate(metadata, stopped.get_token()),
                    pci::PointCloudStatisticsCancelled);
}

TEST_CASE("PDAL loads equivalent block scenes from supported formats",
          "[component][pdal]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;

    const auto las = pci::createPointDatasetRuntime(
        loader.load({.sourcePath = fixture.paths().las}));
    const auto laz = pci::createPointDatasetRuntime(
        loader.load({.sourcePath = fixture.paths().laz}));
    const pci::PointCloudLoadOptions copcOptions{
        .sourcePath = fixture.paths().copc,
        .maximumPoints = 4,
    };
    const pci::PointCloudImportPreflight copcPreflight =
        loader.inspect(copcOptions);
    const auto copc = pci::createPointDatasetRuntime(loader.load(copcOptions));

    CHECK(las->totalPointCount() == pci::test::fixturePoints.size());
    CHECK_FALSE(las->hierarchical());
    CHECK(copcPreflight.desiredRetainedPoints ==
          pci::test::fixturePoints.size());
    CHECK(copc->hierarchical());
    CHECK(sortedPoints(las) == sortedPoints(laz));
    CHECK(sortedPoints(las) == sortedPoints(copc));
    checkFixtureBlocks(las->blocks());
    checkFixtureBlocks(laz->blocks());
    checkFixtureBlocks(copc->blocks());
    CHECK(las->intensityMaximum() == 800);

    const auto blocks = las->blocks();
    REQUIRE(blocks.size() == 1);
    const auto &block = *blocks.front();
    CHECK(block.origin == pci::Vec3d{1000.0, 2000.0, 10.0});
    CHECK(block.scale == Catch::Approx(10.0 / 65535.0));
    CHECK(block.points.size() == block.attributes.size());

    // LAS preserves source order; the first fixture point decodes back
    // to its world position within half a quantization step.
    const pci::Vec3d front =
        pci::decodeBlockPosition(block, block.points.front());
    CHECK(front.x == Catch::Approx(1000.0).margin(block.scale));
    CHECK(front.y == Catch::Approx(2000.0).margin(block.scale));
    CHECK(front.z == Catch::Approx(10.0).margin(block.scale));
    CHECK(block.attributes.front().intensity == 100);
    CHECK(block.attributes.front().classification == 2);
    CHECK((block.points.front().attributes & 0xffU) == 2);
    CHECK(pci::gpuPointIntensity(block.points.front().packedProperties) == 100);
    CHECK(pci::gpuPointReturnNumber(block.points.front().packedProperties) ==
          1);
    CHECK(pci::gpuPointNumberOfReturns(block.points.front().packedProperties) ==
          2);
}

TEST_CASE("COPC hierarchy queries partition and bound decoded nodes",
          "[component][pdal][copc][hierarchy]")
{
    const FixtureDirectory fixture;
    const pci::PdalSourceInspector inspector;
    const pci::PointCloudMetadata metadata =
        inspector.inspect(fixture.paths().copc);
    const pci::PdalHierarchicalPointSource source(metadata, 8, 2);

    CHECK(source.maximumLevel() == 1);
    CHECK_FALSE(source.detailLimited());
    CHECK_FALSE(source.rootNode().leaf);
    CHECK(source.node(pci::childNodeIds(pci::rootPointCloudNode).front()).leaf);

    const pci::PdalHierarchicalPointSource capped(metadata, 4, 2);
    CHECK(capped.detailLimited());
    const pci::PointCloudNode cappedTerminal =
        capped.node(pci::childNodeIds(pci::rootPointCloudNode).front());
    CHECK_FALSE(cappedTerminal.leaf);
    CHECK(cappedTerminal.detailLimited);
    const pci::PointCloudNodePayloadPtr root =
        source.loadNode(pci::rootPointCloudNode, {});
    CHECK(pci::pointCloudNodePayloadPoints(*root) <= 2);

    std::uint64_t sourcePoints = 0;
    std::uint64_t decodedPoints = 0;
    std::vector<pci::PointBlockPtr> childBlocks;
    for (const pci::PointCloudNodeId child :
         pci::childNodeIds(pci::rootPointCloudNode)) {
        const pci::PointCloudNodePayloadPtr payload =
            source.loadNode(child, {});
        sourcePoints += payload->sourcePointCount;
        decodedPoints += pci::pointCloudNodePayloadPoints(*payload);
        CHECK(pci::pointCloudNodePayloadPoints(*payload) <= 2);
        const auto bounds = source.node(child).bounds;
        for (const auto &block : payload->blocks) {
            for (const auto &point : block->points) {
                const auto position = pci::decodeBlockPosition(*block, point);
                const std::array coordinates{
                    position.x, position.y, position.z};
                for (std::size_t axis = 0; axis < coordinates.size(); ++axis) {
                    CHECK(coordinates[axis] >=
                          bounds.minimum[axis] - block->scale * 0.5);
                    CHECK(coordinates[axis] <=
                          bounds.maximum[axis] + block->scale * 0.5);
                }
            }
            childBlocks.push_back(block);
        }
    }
    CHECK(sourcePoints == pci::test::fixturePoints.size());
    CHECK(decodedPoints == pci::test::fixturePoints.size());
    checkFixtureBlocks(childBlocks);

    std::stop_source cancelled;
    cancelled.request_stop();
    CHECK_THROWS_AS(
        source.loadNode(pci::rootPointCloudNode, cancelled.get_token()),
        pci::PointCloudDataSourceCancelled);

    const pci::PointCloudDataSourceMetrics metrics = source.metrics();
    CHECK(metrics.requests == 10);
    CHECK(metrics.completed == 9);
    CHECK(metrics.cancelled == 1);
    CHECK(metrics.failed == 0);
    CHECK(metrics.estimatedDecodedBytesRequested > 0);
    CHECK(metrics.decodedBytesProduced > 0);
    CHECK(metrics.sourcePointsVisited >= pci::test::fixturePoints.size());
    CHECK(metrics.decodedPointsProduced ==
          decodedPoints + pci::pointCloudNodePayloadPoints(*root));
    CHECK(metrics.totalQueryNanoseconds > 0);
    CHECK_FALSE(metrics.fetchedBytesKnown);
}

TEST_CASE("PDAL point limits use a deterministic stride", "[component][pdal]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;

    const auto first = pci::createPointDatasetRuntime(loader.load({
        .sourcePath = fixture.paths().las,
        .maximumPoints = 4,
    }));
    const auto repeated = pci::createPointDatasetRuntime(loader.load({
        .sourcePath = fixture.paths().las,
        .maximumPoints = 4,
    }));

    REQUIRE(first->totalPointCount() == 4);
    CHECK(sortedPoints(first) == sortedPoints(repeated));
    const auto &block = *first->blocks().front();
    // Stride 2 samples source indices 0, 2, 4, 6.
    CHECK(block.attributes[0].intensity == 100);
    CHECK(block.attributes[1].intensity == 300);
    CHECK(block.attributes[2].intensity == 500);
    CHECK(block.attributes[3].intensity == 700);
}

TEST_CASE(
    "ordinary LAS uses persistent local pages above the routing threshold",
    "[component][pdal][local-pages]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    const auto cache = testCache(fixture, "page-cache");
    std::optional<bool> shellLoadingComplete;
    const pci::PointCloudLoadRequest request{
        .options =
            {
                .sourcePath = fixture.paths().las,
                .maximumPoints = 4,
                .localPaging =
                    {
                        .pointThreshold = 1,
                        .cache = cache,
                        .pagePoints = 2,
                        .rootPreviewPoints = 2,
                        .sortMemoryBytes = 4096,
                    },
            },
        .resources =
            {
                .decodedByteBudget = std::uint64_t{8} * 1024 * 1024,
            },
    };
    const pci::PointCloudLoadContext context{
        .dataReady =
            [&](const pci::PointDatasetEvent &event) {
                if (const auto *value =
                        std::get_if<pci::PreparedPointDatasetPtr>(
                            &event.data)) {
                    shellLoadingComplete = (*value)->loadingComplete;
                }
            },
    };

    const pci::PointCloudImportPreflight preflight = loader.inspect(request);
    CHECK(preflight.hierarchical);
    CHECK(preflight.localPaging);
    CHECK(preflight.desiredRetainedPoints == pci::test::fixturePoints.size());
    const auto scene = pci::createPointDatasetRuntime(
        loader.load(request.options, request.resources, preflight, context));
    REQUIRE(scene);
    REQUIRE(shellLoadingComplete);
    CHECK_FALSE(*shellLoadingComplete);
    CHECK(scene->snapshot().loadingComplete);
    CHECK(scene->hierarchical());
    CHECK(scene->totalPointCount() == pci::test::fixturePoints.size());
    CHECK_FALSE(scene->rootNode().leaf);
    CHECK(scene->rootNode().bounds.minimum ==
          preflight.metadata.sourceBounds.minimum);
    CHECK(scene->rootNode().bounds.maximum ==
          preflight.metadata.sourceBounds.maximum);
    CHECK(pci::pointCloudNodePayloadPoints(
              *scene->nodePayload(pci::rootPointCloudNode)) <= 2);
}

TEST_CASE("a single-page local index retains every source point",
          "[component][pdal][local-pages]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    const pci::PointCloudLoadOptions request{
        .sourcePath = fixture.paths().las,
        .maximumPoints = 8,
        .localPaging = {.pointThreshold = 1},
    };
    const auto preflight = loader.inspect(request);
    const auto result = pci::LocalPointIndexBuilder().openOrBuild(
        preflight,
        8,
        {
            .cache = testCache(fixture, "single-page-cache"),
            .pointsPerLeaf = 16,
            .rootPreviewPoints = 2,
            .sortMemoryBytes = 4096,
        });
    REQUIRE(result.rootPayload);
    CHECK(result.pageCount == 1);
    CHECK(result.source->rootNode().leaf);
    CHECK(pci::pointCloudNodePayloadPoints(*result.rootPayload) ==
          pci::test::fixturePoints.size());
    CHECK(result.rootPayload->sourcePointCount ==
          pci::test::fixturePoints.size());
    const auto detail = result.source->fullDetailInfo();
    REQUIRE(detail);
    CHECK(detail->leafNodes == std::vector{pci::rootPointCloudNode});
    CHECK(detail->pointCount == pci::test::fixturePoints.size());
    CHECK(detail->decodedBytes ==
          detail->pointCount *
              (sizeof(pci::GpuPoint) + sizeof(pci::PointAttributes)));
    CHECK(detail->gpuBytes == detail->pointCount * sizeof(pci::GpuPoint));
    CHECK(detail->decodedBytes ==
          pci::pointCloudNodePayloadBytes(*result.rootPayload));
}

TEST_CASE("committed legacy local page fixture is rejected",
          "[component][pdal][local-pages][compatibility]")
{
    const std::filesystem::path store =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "data/local-page-v1/store.pcipages";
    const pci::LocalPointSourceFingerprint fingerprint{
        0x46, 0x10, 0x35, 0xbe, 0x96, 0xc5, 0x59, 0x44, 0x50, 0x77, 0x8c,
        0xc1, 0xb4, 0x12, 0x3a, 0xfd, 0xd3, 0xe0, 0x04, 0xa5, 0x9b, 0x34,
        0xd1, 0xbf, 0x26, 0x5d, 0x80, 0xc2, 0x2a, 0x7a, 0x0b, 0xe7,
    };

    CHECK(sha256(store / "manifest.pci") ==
          "1287d9de970ee542676b3f1e6a0d4c7ea9edb24e149827af6727a78a9b6c5f65");
    CHECK(sha256(store / "payload.bin") ==
          "81e5cc6dbf63a2303966063279def2f82826b439da3750d07d1a840c2c14a257");

    CHECK_THROWS(pci::LocalPointPageSource::openCommitted(
        store, fingerprint, pci::ManifestAuthenticationKey{}, 8));
}

TEST_CASE(
    "local page stores are deterministic, reusable, and fully addressable",
    "[component][pdal][local-pages][residency]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    pci::PointCloudLoadOptions request{
        .sourcePath = fixture.paths().laz,
        .maximumPoints = 8,
        .localPaging = {.pointThreshold = 1},
    };
    const pci::PointCloudImportPreflight preflight = loader.inspect(request);
    const auto optionsFor = [](const pci::LocalPageCacheContextPtr &cache) {
        return pci::LocalPointPageStoreOptions{
            .cache = cache,
            .pointsPerLeaf = 2,
            .rootPreviewPoints = 2,
            .sortMemoryBytes = 4096,
            .diskCacheBytes = std::uint64_t{64} * 1024 * 1024,
        };
    };
    const auto cacheA = testCache(fixture, "cache-a");
    const auto cacheB = testCache(fixture, "cache-b");
    pci::LocalPointIndexBuilder builder;
    std::uint64_t rootPublications = 0;
    bool rootPublishedBeforeCommit = false;
    const auto first =
        builder.openOrBuild(preflight,
                            8,
                            optionsFor(cacheA),
                            {},
                            {},
                            [&](const auto &source, const auto &) {
                                ++rootPublications;
                                rootPublishedBeforeCommit =
                                    !source->committed();
                            });
    REQUIRE(first.source);
    CHECK_FALSE(first.reused);
    const pci::PointCloudStorageMetrics firstStorage =
        first.source->storageMetrics();
    CHECK(firstStorage.localPersistent);
    CHECK(firstStorage.committed);
    CHECK_FALSE(firstStorage.reused);
    CHECK(firstStorage.persistentBytes > 0);
    CHECK(first.sourcePointsScanned == pci::test::fixturePoints.size());
    CHECK(first.pageCount > 1);
    CHECK(first.source->committed());
    REQUIRE(first.source->scalarRanges().intensity);
    CHECK(*first.source->scalarRanges().intensity ==
          pci::PointScalarRange{100.0, 800.0});
    CHECK(rootPublications == 1);
    CHECK(rootPublishedBeforeCommit);
    const pci::PointCloudNode rootNode = first.source->rootNode();
    CHECK(rootNode.geometricError ==
          Catch::Approx(
              std::max(rootNode.bounds.maximumExtent(), 1e-9) /
              std::sqrt(static_cast<double>(rootNode.estimatedPointCount))));

    std::uint64_t sourcePoints = 0;
    std::uint64_t decodedPoints = 0;
    std::uint64_t decodedBytes = 0;
    std::uint64_t nonEmptyLeaves = 0;
    std::uint64_t nonexistentLeaves = 0;
    for (const pci::PointCloudNodeId child :
         pci::childNodeIds(pci::rootPointCloudNode)) {
        const pci::PointCloudNode node = first.source->node(child);
        if (!node.bounds.valid()) {
            ++nonexistentLeaves;
            CHECK(node.estimatedPointCount == 0);
        }
        const auto payload = first.source->loadNode(child, {});
        CHECK(pci::pointCloudNodePayloadPoints(*payload) <= 2);
        if (payload->sourcePointCount != 0) {
            ++nonEmptyLeaves;
            CHECK(pci::pointCloudNodePayloadPoints(*payload) == 2);
        }
        sourcePoints += payload->sourcePointCount;
        decodedPoints += pci::pointCloudNodePayloadPoints(*payload);
        decodedBytes += pci::pointCloudNodePayloadBytes(*payload);
    }
    CHECK(nonEmptyLeaves == 4);
    CHECK(nonexistentLeaves == 4);
    CHECK(sourcePoints == pci::test::fixturePoints.size());
    CHECK(decodedPoints == pci::test::fixturePoints.size());
    const auto detail = first.source->fullDetailInfo();
    REQUIRE(detail);
    CHECK(detail->leafNodes.size() == nonEmptyLeaves);
    CHECK(detail->pointCount == sourcePoints);
    CHECK(detail->decodedBytes ==
          sourcePoints *
              (sizeof(pci::GpuPoint) + sizeof(pci::PointAttributes)));
    CHECK(detail->decodedBytes == decodedBytes);
    CHECK(detail->gpuBytes == sourcePoints * sizeof(pci::GpuPoint));
    CHECK(std::ranges::is_sorted(detail->leafNodes));

    const auto reused = builder.openOrBuild(preflight, 8, optionsFor(cacheA));
    CHECK(reused.reused);
    REQUIRE(cacheA->createdEntries().size() == 1);
    CHECK(cacheA->createdEntries().front().path == first.storeDirectory);
    const auto reopenedCache = testCache(fixture, "cache-a");
    CHECK(builder.openOrBuild(preflight, 8, optionsFor(reopenedCache)).reused);
    CHECK(reopenedCache->createdEntries().empty());
    const pci::PointCloudStorageMetrics reusedStorage =
        reused.source->storageMetrics();
    CHECK(reusedStorage.localPersistent);
    CHECK(reusedStorage.committed);
    CHECK(reusedStorage.reused);
    CHECK(reusedStorage.persistentBytes == firstStorage.persistentBytes);
    REQUIRE(reused.rootPayload);
    CHECK(pci::pointCloudNodePayloadPoints(*reused.rootPayload) == 2);
    CHECK(reused.sourcePointsScanned == 0);
    CHECK(reused.storeDirectory == first.storeDirectory);
    CHECK(reused.source->scalarRanges() == first.source->scalarRanges());
    for (int pass = 0; pass < 12; ++pass) {
        for (const pci::PointCloudNodeId child :
             pci::childNodeIds(pci::rootPointCloudNode)) {
            static_cast<void>(reused.source->loadNode(child, {}));
        }
    }
    CHECK(reused.source->metrics().requests == 97);
    CHECK(reused.source->metrics().fetchedBytesKnown);

    const auto independent =
        builder.openOrBuild(preflight, 8, optionsFor(cacheB));
    CHECK_FALSE(independent.reused);
    CHECK(fileBytes(first.storeDirectory / "manifest.pci") ==
          fileBytes(independent.storeDirectory / "manifest.pci"));
    CHECK(fileBytes(first.storeDirectory / "payload.bin") ==
          fileBytes(independent.storeDirectory / "payload.bin"));
}

TEST_CASE("local page node existence becomes authoritative on completion",
          "[component][pdal][local-pages][hierarchy-contract]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    const pci::PointCloudLoadOptions request{
        .sourcePath = fixture.paths().las,
        .maximumPoints = 8,
        .localPaging = {.pointThreshold = 1},
    };
    const pci::PointCloudImportPreflight preflight = loader.inspect(request);

    const auto building = pci::LocalPointPageSource::createBuilding(
        preflight.metadata,
        fixture.directory() / "building-payload.bin",
        fixture.directory() / "building-store.pcipages",
        {},
        1,
        2,
        8);
    const pci::PointCloudNodeId candidate =
        pci::childNodeIds(pci::rootPointCloudNode).back();
    const pci::PointCloudNode unknown = building->node(candidate);
    CHECK(unknown.bounds.valid());
    CHECK(unknown.estimatedPointCount > 0);

    const auto completed = pci::LocalPointIndexBuilder().openOrBuild(
        preflight,
        8,
        {
            .cache = testCache(fixture, "existence-cache"),
            .pointsPerLeaf = 2,
            .rootPreviewPoints = 2,
            .sortMemoryBytes = 4096,
        });
    const pci::PointCloudNode nonexistent = completed.source->node(candidate);
    CHECK_FALSE(nonexistent.bounds.valid());
    CHECK(nonexistent.estimatedPointCount == 0);
}

TEST_CASE("local page detail caps do not masquerade as source leaves",
          "[component][pdal][local-pages][hierarchy-contract]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    const pci::PointCloudLoadOptions request{
        .sourcePath = fixture.paths().las,
        .maximumPoints = 8,
        .localPaging = {.pointThreshold = 1},
    };
    const pci::PointCloudImportPreflight preflight = loader.inspect(request);
    const auto capped = pci::LocalPointIndexBuilder().openOrBuild(
        preflight,
        2,
        {
            .cache = testCache(fixture, "detail-cap-cache"),
            .pointsPerLeaf = 2,
            .rootPreviewPoints = 2,
            .sortMemoryBytes = 4096,
        });

    REQUIRE(capped.source);
    CHECK(capped.source->maximumLevel() == 0);
    CHECK(capped.source->detailLimited());
    const pci::PointCloudNode terminal = capped.source->rootNode();
    CHECK_FALSE(terminal.leaf);
    CHECK(terminal.detailLimited);
    CHECK_FALSE(capped.source->fullDetailInfo());
}

TEST_CASE("concurrent local page builds publish one reusable store",
          "[component][pdal][local-pages][race]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    const pci::PointCloudLoadOptions request{
        .sourcePath = fixture.paths().laz,
        .maximumPoints = 8,
        .localPaging = {.pointThreshold = 1},
    };
    const auto preflight = loader.inspect(request);
    const pci::LocalPointPageStoreOptions options{
        .cache = testCache(fixture, "race-cache"),
        .pointsPerLeaf = 2,
        .rootPreviewPoints = 2,
        .sortMemoryBytes = 4096,
    };
    std::atomic_uint32_t ready = 0;
    std::atomic_bool start = false;
    const auto build = [&] {
        ready.fetch_add(1);
        while (!start.load()) {
            std::this_thread::yield();
        }
        return pci::LocalPointIndexBuilder().openOrBuild(preflight, 8, options);
    };
    auto firstFuture = std::async(std::launch::async, build);
    auto secondFuture = std::async(std::launch::async, build);
    while (ready.load() != 2) {
        std::this_thread::yield();
    }
    start.store(true);

    const auto first = firstFuture.get();
    const auto second = secondFuture.get();
    CHECK(first.storeDirectory == second.storeDirectory);
    CHECK(first.reused != second.reused);
    CHECK(options.cache->createdEntries().size() == 1);
    CHECK(first.source->committed());
    CHECK(second.source->committed());
    checkFixtureBlocks(leafBlocks(first.source));
    checkFixtureBlocks(leafBlocks(second.source));
    const pci::local_index::LocalPageCacheLocator locator(preflight, options);
    const auto reopened = pci::LocalPointPageSource::openCommitted(
        first.storeDirectory,
        locator.fingerprint(),
        options.cache->manifestAuthenticationKey(),
        8);
    checkFixtureBlocks(leafBlocks(reopened));
}

TEST_CASE("local page build resources clean up with scope",
          "[component][pdal][local-pages][resources]")
{
    const FixtureDirectory fixture;
    const auto cacheContext = testCache(fixture, "resource-cache");
    const std::filesystem::path &cache = cacheContext->directory();
    std::filesystem::path temporaryDirectory;
    {
        const pci::local_index::LocalPageBuildSession session(cache, "entry");
        temporaryDirectory = session.directory();
        CHECK(std::filesystem::is_directory(temporaryDirectory / "runs"));
    }
    CHECK_FALSE(std::filesystem::exists(temporaryDirectory));

    const std::filesystem::path lockPath = cache / "entry.lock";
    {
        pci::local_index::LocalPageBuildLock owner(lockPath);
        pci::local_index::LocalPageBuildLock contender(lockPath);
        CHECK(owner.tryAcquire());
        CHECK_FALSE(contender.tryAcquire());
        CHECK(std::filesystem::is_regular_file(lockPath));
    }
    CHECK_FALSE(std::filesystem::exists(lockPath));
}

TEST_CASE("local page cache corruption rebuilds and source changes invalidate",
          "[component][pdal][local-pages][recovery]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    const pci::PointCloudLoadOptions request{
        .sourcePath = fixture.paths().las,
        .maximumPoints = 8,
        .localPaging = {.pointThreshold = 1},
    };
    const auto options = pci::LocalPointPageStoreOptions{
        .cache = testCache(fixture, "recovery-cache"),
        .pointsPerLeaf = 2,
        .rootPreviewPoints = 2,
        .sortMemoryBytes = 4096,
    };
    pci::LocalPointIndexBuilder builder;
    const auto preflight = loader.inspect(request);
    const auto first = builder.openOrBuild(preflight, 8, options);

    auto wrongAuthenticationKey = options.cache->manifestAuthenticationKey();
    wrongAuthenticationKey.front() ^= 0xffU;
    const auto fingerprint =
        pci::fingerprintLocalPointSource(preflight.metadata,
                                         preflight.sourceFileBytes,
                                         preflight.sourceModificationTime,
                                         options);
    CHECK_THROWS_WITH(
        pci::readLocalPointManifest(first.storeDirectory / "manifest.pci",
                                    fingerprint,
                                    wrongAuthenticationKey),
        Catch::Matchers::ContainsSubstring("authentication failed"));

    {
        std::fstream manifest(first.storeDirectory / "manifest.pci",
                              std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(manifest);
        manifest.seekp(12);
        const char corrupt = '\x7f';
        manifest.write(&corrupt, 1);
    }
    const auto rebuilt = builder.openOrBuild(preflight, 8, options);
    CHECK_FALSE(rebuilt.reused);
    CHECK(rebuilt.storeDirectory == first.storeDirectory);
    CHECK(rebuilt.sourcePointsScanned == pci::test::fixturePoints.size());

    const auto oldTime = std::filesystem::last_write_time(fixture.paths().las);
    std::filesystem::last_write_time(fixture.paths().las,
                                     oldTime + std::chrono::seconds(1));
    const auto changedPreflight = loader.inspect(request);
    const auto changed = builder.openOrBuild(changedPreflight, 8, options);
    CHECK_FALSE(changed.reused);
    CHECK(changed.storeDirectory != rebuilt.storeDirectory);
}

TEST_CASE("local page payload digests reject corrupted detail",
          "[component][pdal][local-pages][recovery]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    const pci::PointCloudLoadOptions request{
        .sourcePath = fixture.paths().las,
        .maximumPoints = 8,
        .localPaging = {.pointThreshold = 1},
    };
    const auto preflight = loader.inspect(request);
    pci::LocalPointIndexBuilder builder;
    const auto result = builder.openOrBuild(
        preflight,
        8,
        {
            .cache = testCache(fixture, "payload-corruption-cache"),
            .pointsPerLeaf = 2,
            .rootPreviewPoints = 2,
            .sortMemoryBytes = 4096,
        });
    {
        std::fstream payload(result.storeDirectory / "payload.bin",
                             std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(payload);
        // Root is two fixed 34-byte records; the next byte starts the first
        // Morton-ordered leaf page.
        payload.seekp(2 * pci::localPointDiskBytes);
        const char corrupt = '\x5a';
        payload.write(&corrupt, 1);
    }
    CHECK_THROWS_WITH(result.source->loadNode(
                          pci::childNodeId(pci::rootPointCloudNode, 0), {}),
                      Catch::Matchers::ContainsSubstring("digest mismatch"));
}

TEST_CASE("local page construction refuses an insufficient disk allowance",
          "[component][pdal][local-pages][recovery]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    const pci::PointCloudLoadOptions request{
        .sourcePath = fixture.paths().las,
        .maximumPoints = 8,
        .localPaging = {.pointThreshold = 1},
    };
    const auto preflight = loader.inspect(request);
    const auto cacheContext = testCache(fixture, "disk-limit-cache");
    const std::filesystem::path &cache = cacheContext->directory();
    CHECK_THROWS_WITH(
        pci::LocalPointIndexBuilder().openOrBuild(preflight,
                                                  8,
                                                  {
                                                      .cache = cacheContext,
                                                      .pointsPerLeaf = 2,
                                                      .rootPreviewPoints = 2,
                                                      .sortMemoryBytes = 4096,
                                                      .diskCacheBytes = 1024,
                                                  }),
        Catch::Matchers::ContainsSubstring("disk cache allowance"));
    REQUIRE(std::filesystem::exists(cache));
    for (const auto &entry : std::filesystem::directory_iterator(cache)) {
        CHECK(entry.path().extension() != ".pcipages");
        CHECK_FALSE(entry.path().filename().string().contains(".tmp-"));
    }
}

TEST_CASE("cancelled local page construction never commits a partial index",
          "[component][pdal][local-pages][cancellation]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    const pci::PointCloudLoadOptions request{
        .sourcePath = fixture.paths().las,
        .maximumPoints = 8,
        .localPaging = {.pointThreshold = 1},
    };
    const auto preflight = loader.inspect(request);
    for (const pci::PointCloudImportStage cancelStage : {
             pci::PointCloudImportStage::Reading,
             pci::PointCloudImportStage::Optimizing,
         }) {
        DYNAMIC_SECTION("cancel at stage " << static_cast<int>(cancelStage))
        {
            const std::filesystem::path cache =
                fixture.paths().las.parent_path() /
                ("cancel-cache-" +
                 std::to_string(static_cast<int>(cancelStage)));
            const auto cacheContext =
                pci::LocalPageCacheContext::createPersistent(
                    cache, fixture.directory() / "cache-configuration");
            std::stop_source stop;
            pci::LocalPointIndexBuilder builder;
            CHECK_THROWS_AS(
                builder.openOrBuild(
                    preflight,
                    8,
                    {
                        .cache = cacheContext,
                        .pointsPerLeaf = 2,
                        .rootPreviewPoints = 2,
                        .sortMemoryBytes = 4096,
                    },
                    stop.get_token(),
                    [&](const pci::PointCloudImportProgress &progress) {
                        if (progress.stage == cancelStage) {
                            stop.request_stop();
                        }
                    }),
                pci::PointCloudImportCancelled);
            CHECK(cacheContext->createdEntries().empty());
            if (std::filesystem::exists(cache)) {
                for (const auto &entry :
                     std::filesystem::directory_iterator(cache)) {
                    CHECK(entry.path().extension() != ".pcipages");
                    CHECK_FALSE(
                        entry.path().filename().string().contains(".tmp-"));
                }
            }
        }
    }
}

TEST_CASE("PDAL safety previews sample deterministic spatial cells",
          "[component][pdal][residency]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    const pci::PointCloudLoadRequest request{
        .options =
            {
                .sourcePath = fixture.paths().las,
                .maximumPoints = 8,
            },
    };

    pci::PointCloudImportPreflight preflight = loader.inspect(request);
    preflight.retainedPointLimit = 4;
    preflight.spatialPreview = true;
    const auto first = pci::createPointDatasetRuntime(
        loader.load(request.options, request.resources, preflight, {}));
    const auto repeated = pci::createPointDatasetRuntime(
        loader.load(request.options, request.resources, preflight, {}));
    REQUIRE(first->totalPointCount() == 4);
    CHECK(sortedPoints(first) == sortedPoints(repeated));

    std::vector<std::uint16_t> intensities;
    for (const pci::PointBlockPtr &block : first->blocks()) {
        for (const pci::PointAttributes attributes : block->attributes) {
            intensities.push_back(attributes.intensity);
        }
    }
    std::ranges::sort(intensities);
    // The 2x2x1 preview grid covers all XY quadrants. It intentionally differs
    // from the source-order stride (100, 300, 500, 700).
    CHECK(intensities == std::vector<std::uint16_t>{100, 200, 300, 400});
}

TEST_CASE("PDAL publishes immutable metadata before streaming blocks",
          "[component][pdal]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    pci::PreparedPointDatasetPtr early;
    std::vector<pci::PointBlockPtr> streamed;
    std::uint64_t sequence = 0;
    const pci::PointCloudLoadContext context{
        .dataReady =
            [&](const pci::PointDatasetEvent &event) {
                REQUIRE(event.sequence == sequence++);
                if (const auto *seed =
                        std::get_if<pci::PreparedPointDatasetPtr>(
                            &event.data)) {
                    early = *seed;
                    CHECK(early->blocks.empty());
                    CHECK_FALSE(early->loadingComplete);
                } else {
                    const auto &block =
                        std::get<pci::PreparedPointBlock>(event.data);
                    REQUIRE(early);
                    REQUIRE(block.index == streamed.size());
                    CHECK(block.sourceId == early->descriptor.sourceId);
                    streamed.push_back(block.block);
                }
            },
    };
    const auto result =
        loader.load({.sourcePath = fixture.paths().las}, {}, context);
    REQUIRE(early);
    CHECK(early->blocks.empty());
    CHECK_FALSE(early->loadingComplete);
    CHECK(result->loadingComplete);
    CHECK(result->eventCount == sequence);
    CHECK(result->blocks == streamed);
    CHECK(result->descriptor.sourceId == early->descriptor.sourceId);
    CHECK(result->pointCount() == pci::test::fixturePoints.size());
}

TEST_CASE("PDAL reports progress and observes cancellation",
          "[component][pdal]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    std::vector<pci::PointCloudImportProgress> progress;
    std::vector<int> callbackOrder;

    const pci::PointCloudLoadOptions options{
        .sourcePath = fixture.paths().las,
        .maximumPoints = 8,
    };
    const pci::PointCloudLoadContext context{
        .progress =
            [&progress,
             &callbackOrder](const pci::PointCloudImportProgress value) {
                progress.push_back(value);
                if (value.stage == pci::PointCloudImportStage::Reading &&
                    value.processed == 0) {
                    callbackOrder.push_back(0);
                }
            },
        .dataReady =
            [&callbackOrder](const pci::PointDatasetEvent &event) {
                if (event.sequence == 0) {
                    callbackOrder.push_back(1);
                }
            },
    };
    const auto scene =
        pci::createPointDatasetRuntime(loader.load(options, {}, context));

    CHECK(scene->totalPointCount() == 8);
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.front().stage == pci::PointCloudImportStage::Reading);
    CHECK(progress.front().processed == 0);
    CHECK(progress.front().total == 8);
    CHECK(progress.back().stage == pci::PointCloudImportStage::Reading);
    CHECK(progress.back().processed == 8);
    CHECK(progress.back().total == 8);
    REQUIRE(callbackOrder.size() >= 2);
    CHECK(callbackOrder[0] == 0);
    CHECK(callbackOrder[1] == 1);

    std::stop_source stop;
    stop.request_stop();
    REQUIRE_THROWS_AS(pci::createPointDatasetRuntime(loader.load(
                          pci::PointCloudLoadOptions{
                              .sourcePath = fixture.paths().las,
                              .maximumPoints = 8,
                          },
                          {},
                          pci::PointCloudLoadContext{
                              .stopToken = stop.get_token(),
                          })),
                      pci::PointCloudImportCancelled);
}

TEST_CASE("PDAL rejects a zero point limit", "[component][pdal]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;

    try {
        static_cast<void>(pci::createPointDatasetRuntime(loader.load({
            .sourcePath = fixture.paths().las,
            .maximumPoints = 0,
        })));
        FAIL("loading unexpectedly succeeded");
    } catch (const pci::PointCloudImportError &error) {
        CHECK(std::string_view(error.what()).contains("maximumPoints"));
    }

    try {
        static_cast<void>(pci::createPointDatasetRuntime(loader.load(
            pci::PointCloudLoadOptions{.sourcePath = fixture.paths().copc},
            pci::PointCloudLoadResources{.decodedByteBudget = 0})));
        FAIL("loading unexpectedly succeeded");
    } catch (const pci::PointCloudImportError &error) {
        CHECK(std::string_view(error.what()).contains("decodedByteBudget"));
    }
}

TEST_CASE("PDAL inspection preserves context for invalid LAS inputs",
          "[component][pdal]")
{
    const FixtureDirectory fixture;
    std::filesystem::path path;
    SECTION("missing supported format")
    {
        path = fixture.directory() / "missing.las";
    }
    SECTION("existing malformed supported format")
    {
        path = fixture.directory() / "malformed.las";
        std::ofstream output(path, std::ios::binary);
        output << "not a LAS header";
        output.close();
        REQUIRE(output);
    }
    try {
        static_cast<void>(pci::PdalSourceInspector().inspect(path));
        FAIL("inspection unexpectedly succeeded");
    } catch (const pci::PointCloudImportError &error) {
        CHECK(std::string_view(error.what()).contains(path.string()));
    }
}

TEST_CASE("PDAL selects COPC for a mixed-case filename suffix",
          "[component][pdal][copc]")
{
    const FixtureDirectory fixture;
    const auto path = fixture.directory() / "mixed.CoPc.LaZ";
    std::filesystem::copy_file(fixture.paths().copc, path);
    checkMetadata(pci::PdalSourceInspector().inspect(path), "readers.copc");
    const auto scene = pci::createPointDatasetRuntime(
        pci::PdalPointCloudLoader().load({.sourcePath = path}));
    REQUIRE(scene);
    CHECK(scene->hierarchical());
    checkFixtureBlocks(scene->blocks());
}

TEST_CASE("PDAL mapping rounds RGB channels at byte conversion boundaries",
          "[component][pdal]")
{
    const FixtureDirectory fixture;
    constexpr std::array<std::uint16_t, 5> channels{0, 128, 129, 32768, 65535};
    constexpr std::array<std::uint32_t, 5> colors{
        0xff000000U, 0xff000000U, 0xff010101U, 0xff808080U, 0xffffffffU};
    std::vector<pci::test::FixturePoint> points;
    for (std::size_t index = 0; index < channels.size(); ++index) {
        points.push_back({1000.0 + static_cast<double>(index),
                          2000.0,
                          10.0,
                          channels[index],
                          channels[index],
                          channels[index],
                          static_cast<std::uint16_t>(index),
                          2,
                          1,
                          1});
    }
    const auto path = fixture.directory() / "rgb-boundaries.las";
    pci::test::writePdalLasFixture(path, points);
    const auto scene = pci::createPointDatasetRuntime(
        pci::PdalPointCloudLoader().load({.sourcePath = path}));
    REQUIRE(scene);
    checkPointBlocks(scene->blocks(), points, colors);
}

TEST_CASE("PDAL XYZ-only sources retain absent attributes and use defaults",
          "[component][pdal][statistics]")
{
    const FixtureDirectory fixture;
    const auto path = fixture.directory() / "xyz.csv";
    {
        std::ofstream output(path);
        output << "X,Y,Z\n1000,2000,10\n1001,2002,13\n";
        output.close();
        REQUIRE(output);
    }
    const auto metadata = pci::PdalSourceInspector().inspect(path);
    CHECK_FALSE(metadata.hasColor);
    CHECK_FALSE(metadata.hasIntensity);
    CHECK_FALSE(metadata.hasClassification);
    CHECK_FALSE(metadata.hasReturnNumber);
    CHECK_FALSE(metadata.hasNumberOfReturns);
    const auto scene = pci::createPointDatasetRuntime(
        pci::PdalPointCloudLoader().load({.sourcePath = path}));
    REQUIRE(scene);
    constexpr std::array<pci::test::FixturePoint, 2> expected{{
        {1000, 2000, 10, 0, 0, 0, 0, 0, 0, 0},
        {1001, 2002, 13, 0, 0, 0, 0, 0, 0, 0},
    }};
    constexpr std::array colors{0xffffffffU, 0xffffffffU};
    checkPointBlocks(scene->blocks(), expected, colors);
    const auto statistics = pci::PdalPointCloudStatistics().calculate(metadata);
    CHECK(statistics.scannedPointCount == 2);
    CHECK_FALSE(statistics.intensity);
    CHECK_FALSE(statistics.red);
    CHECK_FALSE(statistics.green);
    CHECK_FALSE(statistics.blue);
    CHECK_FALSE(statistics.hasClassification);
    CHECK_FALSE(statistics.hasReturnNumber);
    CHECK_FALSE(statistics.hasNumberOfReturns);
    const auto allZero = [](const auto &counts) {
        return std::ranges::all_of(counts, [](const auto count) {
            return count == 0;
        });
    };
    CHECK(allZero(statistics.classificationCounts));
    CHECK(allZero(statistics.returnNumberCounts));
    CHECK(allZero(statistics.numberOfReturnsCounts));
}

TEST_CASE("legacy page stores rebuild completely and then become reusable",
          "[component][pdal][local-pages][compatibility]")
{
    const FixtureDirectory fixture;
    const auto preflight = pci::PdalPointCloudLoader().inspect(
        {.sourcePath = fixture.paths().las});
    const pci::LocalPointPageStoreOptions options{
        .cache = testCache(fixture, "legacy-rebuild-cache"),
        .pointsPerLeaf = 2,
        .rootPreviewPoints = 2,
        .sortMemoryBytes = 4096,
    };
    const pci::local_index::LocalPageCacheLocator locator(preflight, options);
    const auto legacy =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "data/local-page-v1/store.pcipages";
    std::filesystem::copy(legacy,
                          locator.finalDirectory(),
                          std::filesystem::copy_options::recursive);
    pci::LocalPointIndexBuilder builder;
    const auto rebuilt = builder.openOrBuild(preflight, 8, options);
    CHECK_FALSE(rebuilt.reused);
    CHECK(rebuilt.sourcePointsScanned == 8);
    REQUIRE(rebuilt.source);
    CHECK(rebuilt.source->committed());
    CHECK(rebuilt.storeDirectory == locator.finalDirectory());
    checkFixtureBlocks(leafBlocks(rebuilt.source));
    const auto reused = builder.openOrBuild(preflight, 8, options);
    CHECK(reused.reused);
    CHECK(reused.sourcePointsScanned == 0);
    checkFixtureBlocks(leafBlocks(reused.source));
}

TEST_CASE("local page sorting merges multiple runs without losing points",
          "[component][pdal][local-pages]")
{
    const FixtureDirectory fixture;
    std::vector<pci::test::FixturePoint> points;
    for (std::uint32_t index = 0; index < 256; ++index) {
        // An odd multiplier permutes all 256 grid indices.
        const auto value = (index * 73U) % 256U;
        points.push_back({1000.0 + value % 8U,
                          2000.0 + (value / 8U) % 8U,
                          10.0 + value / 64U,
                          65535,
                          65535,
                          65535,
                          static_cast<std::uint16_t>(value),
                          2,
                          1,
                          1});
    }
    const std::vector<std::uint32_t> colors(points.size(), 0xffffffffU);
    const auto path = fixture.directory() / "multi-run.las";
    pci::test::writePdalLasFixture(path, points);
    const auto preflight =
        pci::PdalPointCloudLoader().inspect({.sourcePath = path});
    const auto optionsFor = [&](const std::string_view name) {
        return pci::LocalPointPageStoreOptions{
            .cache = testCache(fixture, name),
            .pointsPerLeaf = 16,
            .rootPreviewPoints = 8,
            .sortMemoryBytes = 4096,
        };
    };
    const auto optionsA = optionsFor("multi-run-a");
    const auto optionsB = optionsFor("multi-run-b");
    REQUIRE(optionsA.cache->manifestAuthenticationKey() ==
            optionsB.cache->manifestAuthenticationKey());
    std::size_t runCount = 0;
    const auto first = pci::LocalPointIndexBuilder().openOrBuild(
        preflight,
        points.size(),
        optionsA,
        {},
        [&](const pci::PointCloudImportProgress &progress) {
            if (progress.stage != pci::PointCloudImportStage::Optimizing ||
                progress.processed != 0)
                return;
            for (const auto &entry :
                 std::filesystem::recursive_directory_iterator(
                     optionsA.cache->directory())) {
                if (entry.is_regular_file() &&
                    entry.path().parent_path().filename() == "runs")
                    ++runCount;
            }
        });
    REQUIRE(runCount > 1);
    REQUIRE(first.source);
    CHECK_FALSE(first.reused);
    CHECK(first.sourcePointsScanned == points.size());
    checkPointBlocks(leafBlocks(first.source), points, colors);
    const auto second = pci::LocalPointIndexBuilder().openOrBuild(
        preflight, points.size(), optionsB);
    CHECK_FALSE(second.reused);
    checkPointBlocks(leafBlocks(second.source), points, colors);
    CHECK(fileBytes(first.storeDirectory / "manifest.pci") ==
          fileBytes(second.storeDirectory / "manifest.pci"));
    CHECK(fileBytes(first.storeDirectory / "payload.bin") ==
          fileBytes(second.storeDirectory / "payload.bin"));
    const pci::local_index::LocalPageCacheLocator locator(preflight, optionsA);
    const auto reopened = pci::LocalPointPageSource::openCommitted(
        first.storeDirectory,
        locator.fingerprint(),
        optionsA.cache->manifestAuthenticationKey(),
        points.size());
    checkPointBlocks(leafBlocks(reopened), points, colors);
}

TEST_CASE("PDAL statistics detect an isolated point in an asymmetric source",
          "[component][pdal][statistics]")
{
    const FixtureDirectory fixture;
    std::vector<pci::test::FixturePoint> points;
    for (int index = 0; index < 20; ++index) {
        points.push_back(
            {static_cast<double>(index), 0, 0, 0, 0, 0, 0, 0, 1, 1});
    }
    points.push_back({10000, 0, 0, 0, 0, 0, 0, 0, 1, 1});
    const auto path = fixture.directory() / "isolated-point.las";
    pci::test::writePdalLasFixture(path, points);
    const auto statistics = pci::PdalPointCloudStatistics().calculate(
        pci::PdalSourceInspector().inspect(path));
    // Sum(0..19) = 190 and sum of their squares = 2470.
    constexpr double mean = 10190.0 / 21.0;
    const double deviation = std::sqrt(100002470.0 / 21.0 - mean * mean);
    CHECK(statistics.scannedPointCount == 21);
    CHECK(statistics.x.minimum == 0);
    CHECK(statistics.x.maximum == 10000);
    CHECK(statistics.x.mean == Catch::Approx(mean));
    CHECK(statistics.x.standardDeviation == Catch::Approx(deviation));
    CHECK(statistics.y.mean == 0);
    CHECK(statistics.z.mean == 0);
    REQUIRE(statistics.spatialOutliers);
    CHECK(statistics.spatialOutliers->samplePointCount == 21);
    CHECK(statistics.spatialOutliers->sampleOutlierCount == 1);
    CHECK(statistics.spatialOutliers->estimatedSourceOutlierCount == 1);
    CHECK(statistics.spatialOutliers->estimatedPercentage ==
          Catch::Approx(100.0 / 21.0));
}

TEST_CASE("PDAL single-point statistics omit undefined density and outliers",
          "[component][pdal][statistics]")
{
    const FixtureDirectory fixture;
    const auto path = fixture.directory() / "single-point.las";
    const std::array points{pci::test::fixturePoints.front()};
    pci::test::writePdalLasFixture(path, points);
    const auto statistics = pci::PdalPointCloudStatistics().calculate(
        pci::PdalSourceInspector().inspect(path));
    CHECK(statistics.scannedPointCount == 1);
    CHECK(statistics.x.mean == 1000);
    CHECK(statistics.y.mean == 2000);
    CHECK(statistics.z.mean == 10);
    CHECK(statistics.x.standardDeviation == 0);
    CHECK(statistics.y.standardDeviation == 0);
    CHECK(statistics.z.standardDeviation == 0);
    CHECK_FALSE(statistics.horizontalBoundingArea);
    CHECK_FALSE(statistics.boundingVolume);
    CHECK_FALSE(statistics.horizontalDensity);
    CHECK_FALSE(statistics.volumetricDensity);
    CHECK_FALSE(statistics.nominalHorizontalSpacing);
    CHECK_FALSE(statistics.spatialOutliers);
}

TEST_CASE("PDAL loading and statistics cancel during source processing",
          "[component][pdal][cancellation]")
{
    const FixtureDirectory fixture;
    constexpr std::uint64_t pointCount = 65537;
    const std::vector points(pointCount, pci::test::fixturePoints.front());
    const auto path = fixture.directory() / "cancel-mid-read.las";
    pci::test::writePdalLasFixture(path, points);
    const auto metadata = pci::PdalSourceInspector().inspect(path);
    for (const bool statistics : {false, true}) {
        DYNAMIC_SECTION((statistics ? "statistics" : "flat loading"))
        {
            std::stop_source stop;
            std::vector<std::uint64_t> progress;
            const auto cancelAfterProgress = [&](const std::uint64_t processed,
                                                 const std::uint64_t total) {
                CHECK(total == pointCount);
                progress.push_back(processed);
                if (processed > 0 && processed < total)
                    stop.request_stop();
            };
            if (statistics) {
                CHECK_THROWS_AS(
                    pci::PdalPointCloudStatistics().calculate(
                        metadata, stop.get_token(), cancelAfterProgress),
                    pci::PointCloudStatisticsCancelled);
            } else {
                const pci::PointCloudLoadContext context{
                    .stopToken = stop.get_token(),
                    .progress =
                        [&](const pci::PointCloudImportProgress value) {
                            if (value.stage ==
                                pci::PointCloudImportStage::Reading)
                                cancelAfterProgress(value.processed,
                                                    value.total);
                        },
                };
                CHECK_THROWS_AS(pci::PdalPointCloudLoader().load(
                                    {.sourcePath = path}, {}, context),
                                pci::PointCloudImportCancelled);
            }
            REQUIRE(stop.stop_requested());
            REQUIRE_FALSE(progress.empty());
            CHECK(progress.back() > 0);
            CHECK(std::ranges::is_sorted(progress));
            CHECK(std::ranges::all_of(progress, [](const auto value) {
                return value < pointCount;
            }));
        }
    }
}

} // namespace

TEST_CASE(
    "storage cleanup preserves progressive builds and retained point readers",
    "[component][pdal][storage]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    const auto cache = testCache(fixture, "maintenance-cache");
    const auto maintenance =
        pci::makeLocalStorageMaintenance({.pointCache = cache->directory()});
    pci::PointCloudLoadOptions request{
        .sourcePath = fixture.paths().laz,
        .maximumPoints = 8,
        .localPaging = {.pointThreshold = 1},
    };
    const auto preflight = loader.inspect(request);
    const pci::LocalPointPageStoreOptions options{
        .cache = cache,
        .pointsPerLeaf = 2,
        .rootPreviewPoints = 2,
        .sortMemoryBytes = 4096,
        .diskCacheBytes = 64 * 1024 * 1024,
    };
    pci::LocalPointIndexBuilder builder;
    bool checkedBuild = false;
    auto built = builder.openOrBuild(
        preflight, 8, options, {}, {}, [&](const auto &, const auto &) {
            const auto result = maintenance->run(
                pci::StorageMaintenanceAction::CleanUnused, {}, {});
            CHECK(result.errorCount == 0);
            CHECK(result.removedBytes == 0);
            CHECK(result.usage[0].protectedBytes > 0);
            checkedBuild = true;
        });
    REQUIRE(checkedBuild);
    auto retained = built.source;
    const auto directory = built.storeDirectory;
    built.source.reset();
    auto clean =
        maintenance->run(pci::StorageMaintenanceAction::CleanUnused, {}, {});
    CHECK(clean.errorCount == 0);
    CHECK(clean.removedBytes == 0);
    CHECK(clean.usage[0].protectedBytes > 0);
    CHECK(retained->loadNode(pci::rootPointCloudNode, {}));
    // A second open registers independent ownership before returning.
    auto reopened = builder.openOrBuild(preflight, 8, options);
    REQUIRE(reopened.reused);
    retained.reset();
    CHECK(maintenance->run(pci::StorageMaintenanceAction::CleanUnused, {}, {})
              .removedBytes == 0);
    CHECK(reopened.source->loadNode(pci::rootPointCloudNode, {}));
    reopened.source.reset();
    clean =
        maintenance->run(pci::StorageMaintenanceAction::CleanUnused, {}, {});
    CHECK(clean.removedBytes > 0);
    CHECK_FALSE(std::filesystem::exists(directory));
    CHECK(std::filesystem::exists(fixture.paths().laz));
}
