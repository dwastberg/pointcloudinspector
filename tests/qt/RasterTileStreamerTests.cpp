#include "renderer/rhi/RasterTileStreamer.h"
#include "scene/RasterLayerDisplay.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <utility>

namespace {

// Produces tiles on demand, and can be told to fail, to block, or to abandon a
// read through its stop token.
class StreamerSource final : public pci::RasterTileSource {
public:
    explicit StreamerSource(pci::RasterLayerMetadata metadata)
        : metadata_(std::move(metadata))
    {
    }

    [[nodiscard]] const pci::RasterLayerMetadata &
    metadata() const noexcept override
    {
        return metadata_;
    }

    [[nodiscard]] pci::RasterTileData
    readTile(const pci::RasterTileRequest &request,
             std::stop_token stop) const override
    {
        ++reads;
        while (blocked.load()) {
            if (stop.stop_requested()) {
                throw pci::RasterReadCancelled{};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (stop.stop_requested()) {
            throw pci::RasterReadCancelled{};
        }
        if (failing.load()) {
            throw pci::RasterReadError("stub tile failure");
        }
        pci::RasterTileData tile;
        tile.key = request.key;
        tile.renderGeneration = request.renderGeneration;
        tile.validWidth = pci::rasterTilePixels;
        tile.validHeight = pci::rasterTilePixels;
        tile.rgba.resize(pci::rasterStoredTileBytes);
        return tile;
    }

    mutable std::atomic_int reads{0};
    std::atomic_bool blocked{false};
    std::atomic_bool failing{false};

private:
    pci::RasterLayerMetadata metadata_;
};

struct Fixture {
    std::shared_ptr<StreamerSource> source;
    pci::RasterLayer layer;
};

[[nodiscard]] Fixture makeFixture(const pci::RasterSampleKind sampleKind =
                                      pci::RasterSampleKind::ContinuousColor)
{
    pci::RasterLayerMetadata metadata;
    metadata.width = 1024;
    metadata.height = 1024;
    metadata.geoTransform = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    metadata.defaultDisplay.sampleKind = sampleKind;
    metadata.defaultDisplay.displayRange =
        pci::RasterDisplayRange{.minimum = 0.0, .maximum = 100.0};
    pci::RasterLevel level;
    level.width = 1024;
    level.height = 1024;
    level.channelCount = 3;
    metadata.levels.push_back(level);

    auto source = std::make_shared<StreamerSource>(std::move(metadata));
    pci::RasterLayer layer;
    layer.id = pci::SceneLayerId{1};
    layer.renderGeneration = 1;
    layer.data = std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = source,
    });
    return {std::move(source), std::move(layer)};
}

[[nodiscard]] pci::RasterLodPlan planFor(std::vector<pci::RasterTileKey> keys)
{
    pci::RasterLodPlan plan;
    plan.selected = keys;
    plan.requests = std::move(keys);
    return plan;
}

[[nodiscard]] pci::RasterCacheKey cacheKey(const pci::RasterLayer &layer,
                                           const pci::RasterTileKey tile)
{
    return {.sourceId = layer.data->sourceId,
            .renderGeneration = layer.renderGeneration,
            .tile = tile};
}

// One MiB per tile is far above a guttered RGBA tile, so budget pressure is
// opt-in rather than incidental.
constexpr std::uint64_t roomyBudget = 64ULL * 1024 * 1024;

TEST_CASE("raster decode parameters apply the edited scalar range and ramp",
          "[qt][raster][color]")
{
    Fixture fixture = makeFixture(pci::RasterSampleKind::ContinuousScalar);
    fixture.layer.style.displayRange =
        pci::RasterDisplayRange{.minimum = 20.0, .maximum = 40.0};
    fixture.layer.style.colorRampKey =
        std::string(pci::defaultRasterScalarColorRampKey);

    pci::PointColorMapCatalog catalog;
    const auto registration = catalog.registerContinuous(
        std::string(pci::defaultRasterScalarColorRampKey),
        "Test viridis",
        {{.position = 0.0F, .color = {1.0F, 0.0F, 0.0F, 1.0F}},
         {.position = 1.0F, .color = {0.0F, 1.0F, 0.0F, 1.0F}}});
    REQUIRE(registration);
    const pci::PointColorMapCatalogSnapshotPtr maps = catalog.freeze();

    const auto decode =
        pci::resolveRasterDecodeParameters(fixture.layer, *maps);
    REQUIRE(decode != nullptr);
    REQUIRE(decode->displayRange.has_value());
    CHECK(decode->displayRange->minimum == 20.0);
    CHECK(decode->displayRange->maximum == 40.0);
    REQUIRE(decode->colorRamp != nullptr);
    CHECK(decode->colorRamp->size() == 2);
}

TEST_CASE("raster streamer admits completed reads once", "[qt][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 2);

    streamer.reconcile(planFor({{0, 0, 0}, {0, 1, 0}}), fixture.layer);
    streamer.waitForIdle();
    const std::vector<pci::RasterCacheKey> admitted =
        streamer.drainCompletions({});

    CHECK(admitted.size() == 2);
    CHECK(streamer.cpuResident(cacheKey(fixture.layer, {0, 0, 0})));
    CHECK(streamer.tile(cacheKey(fixture.layer, {0, 0, 0})) != nullptr);
    CHECK(streamer.metrics().admitted == 2);

    // Reconciling the same plan must not re-read what is already decoded.
    streamer.reconcile(planFor({{0, 0, 0}, {0, 1, 0}}), fixture.layer);
    streamer.waitForIdle();
    CHECK(streamer.drainCompletions({}).empty());
    CHECK(fixture.source->reads.load() == 2);
}

TEST_CASE("raster CPU accounting includes active reads and completions",
          "[qt][raster][stream][memory]")
{
    Fixture fixture = makeFixture();
    const std::uint64_t budget = pci::rasterStoredTileBytes * 3;
    pci::RasterTileStreamer streamer(budget, 2);
    fixture.source->blocked = true;

    streamer.reconcile(planFor({{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}}),
                       fixture.layer);
    while (fixture.source->reads.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // The cache is still empty, so a non-zero value here can only be the
    // reservation acquired before the source allocated its destination.
    CHECK(streamer.metrics().cpuBytes >= pci::rasterStoredTileBytes);
    CHECK(streamer.metrics().cpuBytes <= budget);

    fixture.source->blocked = false;
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));
    CHECK(streamer.metrics().cpuBytes <= budget);
    CHECK(streamer.metrics().cpuPeakBytes <= budget);
}

TEST_CASE("raster streamer hands uploads over in bounded batches",
          "[qt][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 2);

    streamer.reconcile(planFor({{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}}),
                       fixture.layer);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));

    // A burst of completions cannot stall a frame: uploads are handed over a
    // few at a time.
    CHECK(streamer.takeReadyUploads(2).size() == 2);
    CHECK(streamer.takeReadyUploads(10).size() == 2);
    CHECK(streamer.takeReadyUploads(10).empty());
}

TEST_CASE("raster streamer caches failures but never cancellations",
          "[qt][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);

    fixture.source->failing = true;
    streamer.reconcile(planFor({{0, 0, 0}}), fixture.layer);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));

    CHECK(streamer.failed(cacheKey(fixture.layer, {0, 0, 0})));
    CHECK(streamer.metrics().failed == 1);
    REQUIRE(streamer.lastError(fixture.layer.data->sourceId).has_value());
    CHECK(*streamer.lastError(fixture.layer.data->sourceId) ==
          "stub tile failure");

    // A remembered failure is reported once rather than retried every frame.
    fixture.source->failing = false;
    const int readsAfterFailure = fixture.source->reads.load();
    streamer.reconcile(planFor({{0, 0, 0}}), fixture.layer);
    streamer.waitForIdle();
    CHECK(fixture.source->reads.load() == readsAfterFailure);
}

TEST_CASE("raster streamer drops cancelled reads without poisoning them",
          "[qt][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);

    fixture.source->blocked = true;
    streamer.reconcile(planFor({{0, 0, 0}}), fixture.layer);
    while (fixture.source->reads.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Shutting down cancels the in-flight read. Cancellation is not a failure:
    // it must not enter the negative cache, or a cancelled pan would poison
    // the tiles the next frame needs.
    fixture.source->blocked = false;
    streamer.shutdown();
    CHECK_FALSE(streamer.failed(cacheKey(fixture.layer, {0, 0, 0})));
    CHECK_FALSE(streamer.lastError(fixture.layer.data->sourceId).has_value());
}

TEST_CASE("raster streamer drops queued work the camera moved away from",
          "[qt][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);

    fixture.source->blocked = true;
    streamer.reconcile(planFor({{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}}),
                       fixture.layer);
    while (fixture.source->reads.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(streamer.metrics().queued > 0);

    // The next plan wants an entirely different region; the queue tracks the
    // current view rather than its history.
    streamer.reconcile(planFor({{0, 9, 9}}), fixture.layer);
    CHECK(streamer.metrics().cancelled > 0);

    fixture.source->blocked = false;
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));
    CHECK(streamer.cpuResident(cacheKey(fixture.layer, {0, 9, 9})));
    CHECK_FALSE(streamer.cpuResident(cacheKey(fixture.layer, {0, 3, 0})));
    CHECK(streamer.metrics().pending == 0);
}

TEST_CASE("raster streamer camera churn releases cancelled queue slots",
          "[qt][raster][stream][churn]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);
    fixture.source->blocked = true;

    streamer.reconcile(planFor({{0, 0, 0}, {0, 1, 0}}), fixture.layer);
    while (fixture.source->reads.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Only one current queued key plus the active read may survive each pan.
    // Leaving cancelled keys in the dedupe set eventually fills all 256 slots
    // and permanently prevents the current view from scheduling work.
    for (std::uint32_t frame = 1; frame < 400; ++frame) {
        streamer.reconcile(planFor({{0, frame % 4, frame / 4}}), fixture.layer);
        CAPTURE(frame);
        CHECK(streamer.metrics().pending <= 2);
    }

    fixture.source->blocked = false;
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));
    CHECK(streamer.metrics().pending == 0);
}

TEST_CASE("raster streamer separates render generations",
          "[qt][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);

    streamer.reconcile(planFor({{0, 0, 0}}), fixture.layer);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));
    CHECK(streamer.cpuResident(cacheKey(fixture.layer, {0, 0, 0})));

    // A range or ramp change bumps the generation, so the old decode cannot be
    // served as current and the tile is read again.
    pci::RasterLayer restyled = fixture.layer;
    restyled.renderGeneration = 2;
    streamer.reconcile(planFor({{0, 0, 0}}), restyled);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));

    CHECK(streamer.cpuResident(cacheKey(restyled, {0, 0, 0})));
    CHECK(fixture.source->reads.load() == 2);
}

TEST_CASE("raster streamer releases a removed layer", "[qt][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 2);

    streamer.reconcile(planFor({{0, 0, 0}, {0, 1, 0}}), fixture.layer);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));
    REQUIRE(streamer.metrics().cpuBytes > 0);

    streamer.releaseSource(fixture.layer.data->sourceId);
    CHECK_FALSE(streamer.cpuResident(cacheKey(fixture.layer, {0, 0, 0})));
    CHECK(streamer.metrics().cpuBytes == 0);
    CHECK(streamer.takeReadyUploads(10).empty());
}

TEST_CASE("raster streamer drops completions for a released source",
          "[qt][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 2);

    // Reads that finish after their layer is gone must not sit in the
    // completion queue holding decoded bytes that no cache counter reports.
    streamer.reconcile(planFor({{0, 0, 0}, {0, 1, 0}}), fixture.layer);
    streamer.waitForIdle();
    streamer.releaseSource(fixture.layer.data->sourceId);

    CHECK(streamer.drainCompletions({}).empty());
    CHECK(streamer.metrics().cpuBytes == 0);
    CHECK(streamer.metrics().pendingUploads == 0);
}

TEST_CASE("raster streamer rejects a read that finishes after source release",
          "[qt][raster][stream][cancellation]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);
    fixture.source->blocked = true;

    streamer.reconcile(planFor({{0, 0, 0}, {0, 1, 0}}), fixture.layer);
    while (fixture.source->reads.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    streamer.releaseSource(fixture.layer.data->sourceId);
    fixture.source->blocked = false;
    streamer.waitForIdle();

    CHECK(streamer.drainCompletions({}).empty());
    CHECK_FALSE(streamer.cpuResident(cacheKey(fixture.layer, {0, 0, 0})));
    CHECK(streamer.metrics().pending == 0);
    CHECK(streamer.metrics().cpuBytes == 0);
}

TEST_CASE("raster streamer retries uploads rejected by temporary GPU pressure",
          "[qt][raster][stream][cache]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);
    streamer.reconcile(planFor({{0, 0, 0}}), fixture.layer);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));

    const std::vector<pci::RasterCacheKey> first = streamer.takeReadyUploads(1);
    REQUIRE(first.size() == 1);
    streamer.requeueReadyUploads(first);
    CHECK(streamer.metrics().pendingUploads == 1);
    CHECK(streamer.takeReadyUploads(1) == first);
}

TEST_CASE("raster streamer reconciles every layer of a frame at once",
          "[qt][raster][stream]")
{
    Fixture first = makeFixture();
    Fixture second = makeFixture();
    second.layer.id = pci::SceneLayerId{2};
    pci::RasterTileStreamer streamer(roomyBudget, 2);

    const pci::RasterLodPlan firstPlan = planFor({{0, 0, 0}, {0, 1, 0}});
    const pci::RasterLodPlan secondPlan = planFor({{0, 2, 0}, {0, 3, 0}});
    const std::array<pci::RasterFrameLayer, 2> frame{
        pci::RasterFrameLayer{.plan = &firstPlan, .layer = &first.layer},
        pci::RasterFrameLayer{.plan = &secondPlan, .layer = &second.layer},
    };

    streamer.reconcile(frame);
    streamer.waitForIdle();

    CHECK(streamer.drainCompletions({}).size() == 4);
    CHECK(first.source->reads.load() == 2);
    CHECK(second.source->reads.load() == 2);
    // The queue is frame-global. A sweep that saw one layer at a time would
    // treat the other layer's requests as work the camera moved away from and
    // cancel them, which is why reconciliation takes the whole frame.
    CHECK(streamer.metrics().cancelled == 0);
}

TEST_CASE("raster streamer shares the bounded queue fairly across layers",
          "[qt][raster][stream][fairness]")
{
    Fixture first = makeFixture();
    Fixture second = makeFixture();
    second.layer.id = pci::SceneLayerId{2};
    first.source->blocked.store(true);
    second.source->blocked.store(true);
    pci::RasterTileStreamer streamer(256ULL * 1024 * 1024, 1);

    std::vector<pci::RasterTileKey> firstKeys;
    std::vector<pci::RasterTileKey> secondKeys;
    for (std::uint32_t index = 0; index < 300; ++index) {
        firstKeys.push_back({0, index % 20, index / 20});
        secondKeys.push_back({0, index % 20, index / 20});
    }
    const pci::RasterLodPlan firstPlan = planFor(std::move(firstKeys));
    const pci::RasterLodPlan secondPlan = planFor(std::move(secondKeys));
    const std::array<pci::RasterFrameLayer, 2> frame{
        pci::RasterFrameLayer{.plan = &firstPlan, .layer = &first.layer},
        pci::RasterFrameLayer{.plan = &secondPlan, .layer = &second.layer},
    };

    streamer.reconcile(frame);
    REQUIRE(streamer.metrics().pending == pci::rasterMaximumPendingRequests);
    first.source->blocked.store(false);
    second.source->blocked.store(false);
    streamer.waitForIdle();

    // Layer-major insertion would give all 256 slots to the first plan. The
    // round-robin order gives both layers their coverage-first requests.
    CHECK(first.source->reads.load() == 128);
    CHECK(second.source->reads.load() == 128);
}

TEST_CASE("raster streamer keeps every layer's work across repeated frames",
          "[qt][raster][stream]")
{
    Fixture first = makeFixture();
    Fixture second = makeFixture();
    second.layer.id = pci::SceneLayerId{2};
    first.source->blocked.store(true);
    second.source->blocked.store(true);
    pci::RasterTileStreamer streamer(roomyBudget, 1);

    const pci::RasterLodPlan firstPlan = planFor({{0, 0, 0}, {0, 1, 0}});
    const pci::RasterLodPlan secondPlan = planFor({{0, 2, 0}, {0, 3, 0}});
    const std::array<pci::RasterFrameLayer, 2> frame{
        pci::RasterFrameLayer{.plan = &firstPlan, .layer = &first.layer},
        pci::RasterFrameLayer{.plan = &secondPlan, .layer = &second.layer},
    };

    // A still camera over two layers must converge, not churn: the same plan
    // reconciled repeatedly neither re-requests nor cancels anything.
    for (int frameIndex = 0; frameIndex < 4; ++frameIndex) {
        streamer.reconcile(frame);
    }

    const pci::RasterStreamerMetrics metrics = streamer.metrics();
    CHECK(metrics.requested == 4);
    CHECK(metrics.cancelled == 0);
    CHECK(metrics.pending == 4);

    first.source->blocked.store(false);
    second.source->blocked.store(false);
    streamer.waitForIdle();
}

TEST_CASE("raster streamer coalesces wakeups", "[qt][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 2);

    std::atomic_int wakes{0};
    streamer.setWakeCallback([&wakes] {
        ++wakes;
    });

    streamer.reconcile(planFor({{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}}),
                       fixture.layer);
    streamer.waitForIdle();

    // Four completions must not mean four events: one queued event per tile
    // would flood the loop during a large refinement.
    CHECK(wakes.load() >= 1);
    CHECK(wakes.load() <= 4);
    const int beforeDrain = wakes.load();
    static_cast<void>(streamer.drainCompletions({}));

    // Draining rearms the flag so the next completion is announced again.
    streamer.reconcile(planFor({{0, 4, 0}}), fixture.layer);
    streamer.waitForIdle();
    CHECK(wakes.load() > beforeDrain);
}

TEST_CASE("raster streamer respects its pending-request ceiling",
          "[qt][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);
    fixture.source->blocked = true;

    std::vector<pci::RasterTileKey> many;
    many.reserve(pci::rasterMaximumPendingRequests * 2);
    for (std::uint32_t index = 0; index < pci::rasterMaximumPendingRequests * 2;
         ++index) {
        many.push_back({0, index % 4, index / 4});
    }
    streamer.reconcile(planFor(std::move(many)), fixture.layer);

    // Source size and catalog cardinality must not translate into queue
    // growth.
    const pci::RasterStreamerMetrics metrics = streamer.metrics();
    CHECK(metrics.pending == pci::rasterMaximumPendingRequests);
    CHECK(metrics.queued <= metrics.pending);

    fixture.source->blocked = false;
    streamer.shutdown();
}

TEST_CASE("raster streamer does not evict tiles that are on screen",
          "[qt][raster][stream]")
{
    Fixture fixture = makeFixture();
    // Room for roughly two tiles.
    pci::RasterTileStreamer streamer(pci::rasterStoredTileBytes * 5 / 2, 1);

    streamer.reconcile(planFor({{0, 0, 0}}), fixture.layer);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));

    const std::array<pci::RasterCacheKey, 1> onScreen{
        cacheKey(fixture.layer, {0, 0, 0})};
    streamer.reconcile(planFor({{0, 1, 0}, {0, 2, 0}, {0, 3, 0}}),
                       fixture.layer);
    streamer.waitForIdle(onScreen);
    static_cast<void>(streamer.drainCompletions(onScreen));

    // The protected tile survives; admission declines rather than evicting
    // what is being drawn.
    CHECK(streamer.cpuResident(onScreen.front()));
    CHECK(streamer.metrics().cpuBytes <= pci::rasterStoredTileBytes * 5 / 2);
}

} // namespace
TEST_CASE("raster streamer admits reads issued in an earlier epoch",
          "[qt][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);

    fixture.source->blocked = true;
    streamer.reconcile(planFor({{0, 0, 0}}), fixture.layer);
    while (fixture.source->reads.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Several frames pass while the read is in flight, each advancing the
    // request epoch. Reads outlive frames, so rejecting the result by epoch
    // would discard nearly every tile the streamer paid for; the cache key's
    // render generation is what guards correctness instead.
    for (int frame = 0; frame < 5; ++frame) {
        streamer.reconcile(planFor({{0, 0, 0}}), fixture.layer);
    }
    fixture.source->blocked = false;
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));

    CHECK(streamer.cpuResident(cacheKey(fixture.layer, {0, 0, 0})));
    CHECK(streamer.metrics().admitted == 1);
    // Five reconciles, one read: an in-flight key is not requested again.
    CHECK(fixture.source->reads.load() == 1);
}
