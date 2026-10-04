#include <pci/runtime/raster/RasterTileStreamer.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
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
        tile.profile = request.profile;
        if (request.profile == pci::RasterTilePayloadProfile::RenderElevation) {
            tile.elevation.resize(pci::rasterStoredTilePixels *
                                  pci::rasterStoredTilePixels);
            tile.hasValidElevation = true;
        }
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
    pci::RasterSourceId sourceId;
    pci::BindingGeneration bindingGeneration{1};
    std::uint64_t renderGeneration = 1;
};

[[nodiscard]] Fixture makeFixture()
{
    pci::RasterLayerMetadata metadata;
    metadata.width = 1024;
    metadata.height = 1024;
    metadata.geoTransform = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    metadata.defaultDisplay.sampleKind = pci::RasterSampleKind::ContinuousColor;
    metadata.defaultDisplay.displayRange =
        pci::RasterDisplayRange{.minimum = 0.0, .maximum = 100.0};
    pci::RasterLevel level;
    level.width = 1024;
    level.height = 1024;
    level.channelCount = 3;
    metadata.levels.push_back(level);

    auto source = std::make_shared<StreamerSource>(std::move(metadata));
    return {.source = std::move(source), .sourceId = pci::nextRasterSourceId()};
}

[[nodiscard]] pci::RasterRequestBatch
requestBatchFor(const std::vector<pci::RasterTileKey> &requests,
                const Fixture &fixture,
                const pci::RasterTilePayloadProfile profile =
                    pci::RasterTilePayloadProfile::ColorOnly,
                std::shared_ptr<const pci::RasterDecodeParameters> decode = {})
{
    return {
        .sourceId = fixture.sourceId,
        .bindingGeneration = fixture.bindingGeneration,
        .renderGeneration = fixture.renderGeneration,
        .source = fixture.source,
        .decode = std::move(decode),
        .profile = profile,
        .orderedRequests = requests,
    };
}

void reconcile(pci::RasterTileStreamer &streamer,
               const std::vector<pci::RasterTileKey> &requests,
               const Fixture &fixture,
               const pci::RasterTilePayloadProfile profile =
                   pci::RasterTilePayloadProfile::ColorOnly,
               std::shared_ptr<const pci::RasterDecodeParameters> decode = {})
{
    const pci::RasterRequestBatch batch =
        requestBatchFor(requests, fixture, profile, std::move(decode));
    streamer.reconcile(std::span{&batch, 1});
}

[[nodiscard]] pci::RasterCacheKey
cacheKey(const Fixture &fixture,
         const pci::RasterTileKey tile,
         const pci::RasterTilePayloadProfile profile =
             pci::RasterTilePayloadProfile::ColorOnly)
{
    return {.sourceId = fixture.sourceId,
            .renderGeneration = fixture.renderGeneration,
            .tile = tile,
            .profile = profile};
}

// One MiB per tile is far above a guttered RGBA tile, so budget pressure is
// opt-in rather than incidental.
constexpr std::uint64_t roomyBudget = 64ULL * 1024 * 1024;

TEST_CASE("raster streamer consumes runtime batches without document layers",
          "[runtime][raster][stream][architecture]")
{
    Fixture fixture = makeFixture();
    const std::vector<pci::RasterTileKey> requests{{0, 0, 0}};
    const pci::RasterRequestBatch batch{
        .sourceId = fixture.sourceId,
        .bindingGeneration = fixture.bindingGeneration,
        .renderGeneration = fixture.renderGeneration,
        .source = fixture.source,
        .orderedRequests = requests,
    };
    const std::shared_ptr<StreamerSource> source = fixture.source;
    fixture = {};

    pci::RasterTileStreamer streamer(roomyBudget, 1);
    streamer.reconcile(std::span{&batch, 1});
    streamer.waitForIdle();

    CHECK(streamer.drainCompletions({}).size() == 1);
    CHECK(source->reads.load() == 1);
}

TEST_CASE("raster streamer rejects malformed runtime batches atomically",
          "[runtime][raster][stream][architecture]")
{
    Fixture fixture = makeFixture();
    const std::vector<pci::RasterTileKey> requests{{0, 0, 0}};
    pci::RasterRequestBatch batch = requestBatchFor(requests, fixture);
    batch.bindingGeneration = {};

    pci::RasterTileStreamer streamer(roomyBudget, 1);
    CHECK_THROWS_AS(streamer.reconcile(std::span{&batch, 1}),
                    std::invalid_argument);
    CHECK(streamer.metrics().requested == 0);
    CHECK(fixture.source->reads.load() == 0);
}

TEST_CASE("raster streamer admits completed reads once",
          "[runtime][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 2);

    reconcile(streamer, {{0, 0, 0}, {0, 1, 0}}, fixture);
    streamer.waitForIdle();
    const std::vector<pci::RasterCacheKey> admitted =
        streamer.drainCompletions({});

    CHECK(admitted.size() == 2);
    CHECK(streamer.cpuResident(cacheKey(fixture, {0, 0, 0})));
    CHECK(streamer.tile(cacheKey(fixture, {0, 0, 0})) != nullptr);
    CHECK(streamer.metrics().admitted == 2);

    // Reconciling the same plan must not re-read what is already decoded.
    reconcile(streamer, {{0, 0, 0}, {0, 1, 0}}, fixture);
    streamer.waitForIdle();
    CHECK(streamer.drainCompletions({}).empty());
    CHECK(fixture.source->reads.load() == 2);
}

TEST_CASE("raster CPU accounting includes active reads and completions",
          "[runtime][raster][stream][memory]")
{
    Fixture fixture = makeFixture();
    const std::uint64_t budget = pci::rasterStoredTileBytes * 3;
    pci::RasterTileStreamer streamer(budget, 2);
    fixture.source->blocked = true;

    reconcile(streamer, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}}, fixture);
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
          "[runtime][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 2);

    reconcile(streamer, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}}, fixture);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));

    // A burst of completions cannot stall a frame: uploads are handed over a
    // few at a time.
    CHECK(streamer.takeReadyUploads(2).size() == 2);
    CHECK(streamer.takeReadyUploads(10).size() == 2);
    CHECK(streamer.takeReadyUploads(10).empty());
}

TEST_CASE("raster streamer caches failures but never cancellations",
          "[runtime][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);

    fixture.source->failing = true;
    reconcile(streamer, {{0, 0, 0}}, fixture);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));

    CHECK(streamer.failed(cacheKey(fixture, {0, 0, 0})));
    CHECK(streamer.metrics().failed == 1);
    REQUIRE(streamer.lastError(fixture.sourceId).has_value());
    CHECK(*streamer.lastError(fixture.sourceId) == "stub tile failure");

    // A remembered failure is reported once rather than retried every frame.
    fixture.source->failing = false;
    const int readsAfterFailure = fixture.source->reads.load();
    reconcile(streamer, {{0, 0, 0}}, fixture);
    streamer.waitForIdle();
    CHECK(fixture.source->reads.load() == readsAfterFailure);
}

TEST_CASE("raster streamer drops cancelled reads without poisoning them",
          "[runtime][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);

    fixture.source->blocked = true;
    reconcile(streamer, {{0, 0, 0}}, fixture);
    while (fixture.source->reads.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Shutting down cancels the in-flight read. Cancellation is not a failure:
    // it must not enter the negative cache, or a cancelled pan would poison
    // the tiles the next frame needs.
    fixture.source->blocked = false;
    streamer.shutdown();
    CHECK_FALSE(streamer.failed(cacheKey(fixture, {0, 0, 0})));
    CHECK_FALSE(streamer.lastError(fixture.sourceId).has_value());
}

TEST_CASE("raster streamer drops queued work the camera moved away from",
          "[runtime][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);

    fixture.source->blocked = true;
    reconcile(streamer, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}}, fixture);
    while (fixture.source->reads.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(streamer.metrics().queued > 0);

    // The next plan wants an entirely different region; the queue tracks the
    // current view rather than its history.
    reconcile(streamer, {{0, 9, 9}}, fixture);
    CHECK(streamer.metrics().cancelled > 0);

    fixture.source->blocked = false;
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));
    CHECK(streamer.cpuResident(cacheKey(fixture, {0, 9, 9})));
    CHECK_FALSE(streamer.cpuResident(cacheKey(fixture, {0, 3, 0})));
    CHECK(streamer.metrics().pending == 0);
}

TEST_CASE("raster streamer camera churn releases cancelled queue slots",
          "[runtime][raster][stream][churn]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);
    fixture.source->blocked = true;

    reconcile(streamer, {{0, 0, 0}, {0, 1, 0}}, fixture);
    while (fixture.source->reads.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Only one current queued key plus the active read may survive each pan.
    // Leaving cancelled keys in the dedupe set eventually fills all 256 slots
    // and permanently prevents the current view from scheduling work.
    for (std::uint32_t frame = 1; frame < 400; ++frame) {
        reconcile(streamer, {{0, frame % 4, frame / 4}}, fixture);
        CAPTURE(frame);
        CHECK(streamer.metrics().pending <= 2);
    }

    fixture.source->blocked = false;
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));
    CHECK(streamer.metrics().pending == 0);
}

TEST_CASE("raster streamer separates render generations",
          "[runtime][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);

    reconcile(streamer, {{0, 0, 0}}, fixture);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));
    CHECK(streamer.cpuResident(cacheKey(fixture, {0, 0, 0})));

    // A range or ramp change bumps the generation, so the old decode cannot be
    // served as current and the tile is read again.
    Fixture restyled = fixture;
    restyled.renderGeneration = 2;
    reconcile(streamer, {{0, 0, 0}}, restyled);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));

    CHECK(streamer.cpuResident(cacheKey(restyled, {0, 0, 0})));
    CHECK(fixture.source->reads.load() == 2);
}

TEST_CASE("raster streamer keeps color and elevation payloads distinct",
          "[runtime][raster][stream][surface]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);
    const std::vector<pci::RasterTileKey> requests{{0, 0, 0}};

    reconcile(streamer, requests, fixture);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));
    const pci::RasterCacheKey color = cacheKey(fixture, {0, 0, 0});
    REQUIRE(streamer.cpuResident(color));
    REQUIRE(streamer.tile(color) != nullptr);
    CHECK(streamer.tile(color)->elevation.empty());

    reconcile(streamer,
              requests,
              fixture,
              pci::RasterTilePayloadProfile::RenderElevation);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));
    const pci::RasterCacheKey elevation = cacheKey(
        fixture, {0, 0, 0}, pci::RasterTilePayloadProfile::RenderElevation);
    REQUIRE(streamer.cpuResident(elevation));
    REQUIRE(streamer.tile(elevation) != nullptr);
    CHECK_FALSE(streamer.tile(elevation)->elevation.empty());
    CHECK(streamer.cpuResident(color));
    CHECK(fixture.source->reads.load() == 2);
}

TEST_CASE("raster streamer releases a removed layer",
          "[runtime][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 2);

    reconcile(streamer, {{0, 0, 0}, {0, 1, 0}}, fixture);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));
    REQUIRE(streamer.metrics().cpuBytes > 0);

    streamer.releaseSource(fixture.sourceId);
    CHECK_FALSE(streamer.cpuResident(cacheKey(fixture, {0, 0, 0})));
    CHECK(streamer.metrics().cpuBytes == 0);
    CHECK(streamer.takeReadyUploads(10).empty());
}

TEST_CASE("raster streamer drops completions for a released source",
          "[runtime][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 2);

    // Reads that finish after their layer is gone must not sit in the
    // completion queue holding decoded bytes that no cache counter reports.
    reconcile(streamer, {{0, 0, 0}, {0, 1, 0}}, fixture);
    streamer.waitForIdle();
    streamer.releaseSource(fixture.sourceId);

    CHECK(streamer.drainCompletions({}).empty());
    CHECK(streamer.metrics().cpuBytes == 0);
    CHECK(streamer.metrics().pendingUploads == 0);
}

TEST_CASE("raster streamer rejects a read that finishes after source release",
          "[runtime][raster][stream][cancellation]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);
    fixture.source->blocked = true;

    reconcile(streamer, {{0, 0, 0}, {0, 1, 0}}, fixture);
    while (fixture.source->reads.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    streamer.releaseSource(fixture.sourceId);
    fixture.source->blocked = false;
    streamer.waitForIdle();

    CHECK(streamer.drainCompletions({}).empty());
    CHECK_FALSE(streamer.cpuResident(cacheKey(fixture, {0, 0, 0})));
    CHECK(streamer.metrics().pending == 0);
    CHECK(streamer.metrics().cpuBytes == 0);
}

TEST_CASE("raster streamer retries uploads rejected by temporary GPU pressure",
          "[runtime][raster][stream][cache]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);
    reconcile(streamer, {{0, 0, 0}}, fixture);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));

    const std::vector<pci::RasterCacheKey> first = streamer.takeReadyUploads(1);
    REQUIRE(first.size() == 1);
    streamer.requeueReadyUploads(first);
    CHECK(streamer.metrics().pendingUploads == 1);
    CHECK(streamer.takeReadyUploads(1) == first);
}

TEST_CASE("raster streamer reconciles every layer of a frame at once",
          "[runtime][raster][stream]")
{
    Fixture first = makeFixture();
    Fixture second = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 2);

    const std::vector<pci::RasterTileKey> firstRequests{{0, 0, 0}, {0, 1, 0}};
    const std::vector<pci::RasterTileKey> secondRequests{{0, 2, 0}, {0, 3, 0}};
    const std::array<pci::RasterRequestBatch, 2> frame{
        requestBatchFor(firstRequests, first),
        requestBatchFor(secondRequests, second),
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
          "[runtime][raster][stream][fairness]")
{
    Fixture first = makeFixture();
    Fixture second = makeFixture();
    first.source->blocked.store(true);
    second.source->blocked.store(true);
    pci::RasterTileStreamer streamer(256ULL * 1024 * 1024, 1);

    std::vector<pci::RasterTileKey> firstKeys;
    std::vector<pci::RasterTileKey> secondKeys;
    for (std::uint32_t index = 0; index < 300; ++index) {
        firstKeys.push_back({0, index % 20, index / 20});
        secondKeys.push_back({0, index % 20, index / 20});
    }
    const std::array<pci::RasterRequestBatch, 2> frame{
        requestBatchFor(firstKeys, first),
        requestBatchFor(secondKeys, second),
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
          "[runtime][raster][stream]")
{
    Fixture first = makeFixture();
    Fixture second = makeFixture();
    first.source->blocked.store(true);
    second.source->blocked.store(true);
    pci::RasterTileStreamer streamer(roomyBudget, 1);

    const std::vector<pci::RasterTileKey> firstRequests{{0, 0, 0}, {0, 1, 0}};
    const std::vector<pci::RasterTileKey> secondRequests{{0, 2, 0}, {0, 3, 0}};
    const std::array<pci::RasterRequestBatch, 2> frame{
        requestBatchFor(firstRequests, first),
        requestBatchFor(secondRequests, second),
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

TEST_CASE("raster streamer coalesces wakeups", "[runtime][raster][stream]")
{
    Fixture fixture = makeFixture();
    std::atomic_int wakes{0};
    std::mutex wakeMutex;
    std::condition_variable announced;
    // Callback state outlives the streamer's joining destructor.
    pci::RasterTileStreamer streamer(roomyBudget, 2);
    streamer.setWakeCallback([&] {
        {
            const std::scoped_lock lock(wakeMutex);
            ++wakes;
        }
        announced.notify_all();
    });
    const auto awaitAnnouncementAfter = [&](int previous) {
        std::unique_lock lock(wakeMutex);
        return announced.wait_for(lock, std::chrono::seconds(2), [&] {
            return wakes.load() > previous;
        });
    };

    reconcile(streamer, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}}, fixture);
    streamer.waitForIdle();

    // waitForIdle covers reads; the worker announces after releasing its read
    // count.
    REQUIRE(awaitAnnouncementAfter(0));

    // Four completions must not mean four events: one queued event per tile
    // would flood the loop during a large refinement.
    CHECK(wakes.load() >= 1);
    CHECK(wakes.load() <= 4);
    const int beforeDrain = wakes.load();
    static_cast<void>(streamer.drainCompletions({}));

    // Draining rearms the flag so the next completion is announced again.
    reconcile(streamer, {{0, 4, 0}}, fixture);
    streamer.waitForIdle();
    REQUIRE(awaitAnnouncementAfter(beforeDrain));
    CHECK(wakes.load() > beforeDrain);
}

TEST_CASE("raster streamer respects its pending-request ceiling",
          "[runtime][raster][stream]")
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
    reconcile(streamer, many, fixture);

    // Source size and catalog cardinality must not translate into queue
    // growth.
    const pci::RasterStreamerMetrics metrics = streamer.metrics();
    CHECK(metrics.pending == pci::rasterMaximumPendingRequests);
    CHECK(metrics.queued <= metrics.pending);

    fixture.source->blocked = false;
    streamer.shutdown();
}

TEST_CASE("raster streamer does not evict tiles that are on screen",
          "[runtime][raster][stream]")
{
    Fixture fixture = makeFixture();
    // Room for roughly two tiles.
    pci::RasterTileStreamer streamer(pci::rasterStoredTileBytes * 5 / 2, 1);

    reconcile(streamer, {{0, 0, 0}}, fixture);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));

    const std::array<pci::RasterCacheKey, 1> onScreen{
        cacheKey(fixture, {0, 0, 0})};
    reconcile(streamer, {{0, 1, 0}, {0, 2, 0}, {0, 3, 0}}, fixture);
    streamer.waitForIdle(onScreen);
    static_cast<void>(streamer.drainCompletions(onScreen));

    // The protected tile survives; admission declines rather than evicting
    // what is being drawn.
    CHECK(streamer.cpuResident(onScreen.front()));
    CHECK(streamer.metrics().cpuBytes <= pci::rasterStoredTileBytes * 5 / 2);
}

TEST_CASE("raster streamer admits reads issued in an earlier epoch",
          "[runtime][raster][stream]")
{
    Fixture fixture = makeFixture();
    pci::RasterTileStreamer streamer(roomyBudget, 1);

    fixture.source->blocked = true;
    reconcile(streamer, {{0, 0, 0}}, fixture);
    while (fixture.source->reads.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Several frames pass while the read is in flight, each advancing the
    // request epoch. Reads outlive frames, so rejecting the result by epoch
    // would discard nearly every tile the streamer paid for; the cache key's
    // render generation is what guards correctness instead.
    for (int frame = 0; frame < 5; ++frame) {
        reconcile(streamer, {{0, 0, 0}}, fixture);
    }
    fixture.source->blocked = false;
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));

    CHECK(streamer.cpuResident(cacheKey(fixture, {0, 0, 0})));
    CHECK(streamer.metrics().admitted == 1);
    // Five reconciles, one read: an in-flight key is not requested again.
    CHECK(fixture.source->reads.load() == 1);
}

} // namespace
