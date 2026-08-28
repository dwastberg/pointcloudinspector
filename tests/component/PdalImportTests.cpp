#include "fixtures/PdalFixtureFactory.h"
#include "import/PointCloudImport.h"
#include "import/local/LocalPointIndexBuilder.h"
#include "import/local/LocalPointPageFormat.h"
#include "import/pdal/LocalPageBuildInfrastructure.h"
#include "import/pdal/PdalHierarchicalPointSource.h"
#include "import/pdal/PdalPointCloudLoader.h"
#include "import/pdal/PdalPointCloudStatistics.h"
#include "import/pdal/PdalSourceInspector.h"
#include "pointcloud/GpuPointProperties.h"
#include "scene/PointBlock.h"

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

std::vector<pci::GpuPoint> sortedPoints(const pci::PointCloudScenePtr &scene)
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

TEST_CASE("PDAL fixtures create valid LAS, LAZ, and COPC inputs",
          "[component][pdal]")
{
    const FixtureDirectory fixture;
    CHECK(std::filesystem::is_regular_file(fixture.paths().las));
    CHECK(std::filesystem::is_regular_file(fixture.paths().laz));
    CHECK(std::filesystem::is_regular_file(fixture.paths().copc));
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

    const auto las = loader.load({.sourcePath = fixture.paths().las});
    const auto laz = loader.load({.sourcePath = fixture.paths().laz});
    const pci::PointCloudLoadOptions copcOptions{
        .sourcePath = fixture.paths().copc,
        .maximumPoints = 4,
    };
    const pci::PointCloudImportPreflight copcPreflight =
        loader.inspect(copcOptions);
    const auto copc = loader.load(copcOptions);

    CHECK(las->totalPointCount() == pci::test::fixturePoints.size());
    CHECK_FALSE(las->hierarchical());
    CHECK(copcPreflight.desiredRetainedPoints ==
          pci::test::fixturePoints.size());
    CHECK(copc->hierarchical());
    CHECK(sortedPoints(las) == sortedPoints(laz));
    CHECK(sortedPoints(las) == sortedPoints(copc));
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
    for (const pci::PointCloudNodeId child :
         pci::childNodeIds(pci::rootPointCloudNode)) {
        const pci::PointCloudNodePayloadPtr payload =
            source.loadNode(child, {});
        sourcePoints += payload->sourcePointCount;
        decodedPoints += pci::pointCloudNodePayloadPoints(*payload);
        CHECK(pci::pointCloudNodePayloadPoints(*payload) <= 2);
    }
    CHECK(sourcePoints == pci::test::fixturePoints.size());
    CHECK(decodedPoints == pci::test::fixturePoints.size());

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

    const auto first = loader.load({
        .sourcePath = fixture.paths().las,
        .maximumPoints = 4,
    });
    const auto repeated = loader.load({
        .sourcePath = fixture.paths().las,
        .maximumPoints = 4,
    });

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
        .sceneReady =
            [&](const pci::PointCloudScenePtr &value) {
                shellLoadingComplete = value->snapshot().loadingComplete;
            },
    };

    const pci::PointCloudImportPreflight preflight = loader.inspect(request);
    CHECK(preflight.hierarchical);
    CHECK(preflight.localPaging);
    CHECK(preflight.desiredRetainedPoints == pci::test::fixturePoints.size());
    const auto scene =
        loader.load(request.options, request.resources, preflight, context);
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

TEST_CASE("committed v1 local page fixture is rejected for rebuilding",
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
    CHECK(first.source->committed());
    CHECK(second.source->committed());
    CHECK(fileBytes(first.storeDirectory / "manifest.pci") ==
          fileBytes(second.storeDirectory / "manifest.pci"));
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
    const auto first =
        loader.load(request.options, request.resources, preflight, {});
    const auto repeated =
        loader.load(request.options, request.resources, preflight, {});
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

TEST_CASE("PDAL publishes the scene before streaming blocks",
          "[component][pdal]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;

    pci::PointCloudScenePtr early;
    pci::PointCloudSceneInvalidationSubscription subscription;
    std::uint64_t blocksAtSceneReady = 0;
    std::uint64_t publishedRevision = 0;
    std::uint64_t publishedPoints = 0;
    bool loadActive = true;
    const pci::PointCloudLoadOptions options{
        .sourcePath = fixture.paths().las,
    };
    const pci::PointCloudLoadContext context{
        .sceneReady =
            [&](const pci::PointCloudScenePtr &value) {
                early = value;
                blocksAtSceneReady = value->blocks().size();
                subscription = value->subscribeInvalidation([&, value] {
                    if (loadActive) {
                        publishedRevision = value->revision();
                        publishedPoints = value->totalPointCount();
                    }
                });
            },
    };
    const auto scene = loader.load(options, {}, context);
    loadActive = false;

    REQUIRE(early != nullptr);
    CHECK(early.get() == scene.get());
    CHECK(blocksAtSceneReady == 0);
    CHECK(early->metadata().sourcePointCount ==
          pci::test::fixturePoints.size());
    CHECK(publishedRevision > 0);
    CHECK(publishedPoints > 0);
    CHECK(publishedPoints == scene->totalPointCount());
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
        .sceneReady =
            [&callbackOrder](const pci::PointCloudScenePtr &) {
                callbackOrder.push_back(1);
            },
    };
    const auto scene = loader.load(options, {}, context);

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
    REQUIRE_THROWS_AS(loader.load(
                          pci::PointCloudLoadOptions{
                              .sourcePath = fixture.paths().las,
                              .maximumPoints = 8,
                          },
                          {},
                          pci::PointCloudLoadContext{
                              .stopToken = stop.get_token(),
                          }),
                      pci::PointCloudImportCancelled);
}

TEST_CASE("PDAL rejects a zero point limit", "[component][pdal]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;

    try {
        static_cast<void>(loader.load({
            .sourcePath = fixture.paths().las,
            .maximumPoints = 0,
        }));
        FAIL("loading unexpectedly succeeded");
    } catch (const pci::PointCloudImportError &error) {
        CHECK(std::string_view(error.what()).contains("maximumPoints"));
    }

    try {
        static_cast<void>(loader.load(
            pci::PointCloudLoadOptions{.sourcePath = fixture.paths().copc},
            pci::PointCloudLoadResources{.decodedByteBudget = 0}));
        FAIL("loading unexpectedly succeeded");
    } catch (const pci::PointCloudImportError &error) {
        CHECK(std::string_view(error.what()).contains("decodedByteBudget"));
    }
}

} // namespace
