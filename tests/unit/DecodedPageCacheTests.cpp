#include <pci/runtime/point/DecodedPageCache.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <tuple>
#include <vector>

namespace pci {
struct DecodedPageCacheTestAccess {
    static void fail(DecodedPageCache &cache, std::size_t nth)
    {
        cache.allocation_->attempts = 0;
        cache.allocation_->failAt = nth;
    }
    static std::size_t inspections(const DecodedPageCache &cache)
    {
        return cache.lastTrimInspections_;
    }
    static std::size_t allocations(const DecodedPageCache &cache)
    {
        return cache.allocation_->outstanding;
    }
    static bool consistent(const DecodedPageCache &cache)
    {
        if (cache.recency_.size() != cache.entries_.size())
            return false;
        for (auto it = cache.recency_.begin(); it != cache.recency_.end();
             ++it) {
            const auto found = cache.entries_.find(*it);
            if (found == cache.entries_.end() || found->second.recency != it)
                return false;
        }
        return true;
    }
    static auto state(const DecodedPageCache &cache)
    {
        std::vector<std::tuple<PointCloudSourceId,
                               PointCloudNodeId,
                               const PointCloudNodePayload *,
                               std::uint64_t,
                               std::uint64_t,
                               std::uint64_t>>
            entries;
        for (const auto &[key, entry] : cache.entries_)
            entries.emplace_back(key.sourceId,
                                 key.nodeId,
                                 entry.payload.get(),
                                 entry.bytes,
                                 entry.points,
                                 entry.lastUsed);
        std::ranges::sort(entries);
        std::vector<std::pair<PointCloudSourceId, PointCloudNodeId>> pins;
        for (const auto &key : cache.pinned_)
            pins.emplace_back(key.sourceId, key.nodeId);
        std::ranges::sort(pins);
        std::vector<std::pair<PointCloudSourceId, PointCloudNodeId>> recency;
        for (const auto &key : cache.recency_)
            recency.emplace_back(key.sourceId, key.nodeId);
        std::vector<std::pair<PointCloudSourceId, PointCloudNodeId>>
            protectedKeys;
        for (const auto &key : cache.frameProtection_)
            protectedKeys.emplace_back(key.sourceId, key.nodeId);
        std::ranges::sort(protectedKeys);
        std::vector<std::pair<PointCloudSourceId, std::array<std::uint64_t, 8>>>
            sources;
        for (const auto &[id, source] : cache.sources_)
            sources.push_back({id,
                               {source.hits,
                                source.misses,
                                source.insertions,
                                source.replacements,
                                source.evictions,
                                source.residentBytes,
                                source.peakResidentBytes,
                                source.residentPoints}});
        std::ranges::sort(sources);
        return std::tuple{entries,
                          pins,
                          recency,
                          protectedKeys,
                          sources,
                          std::array{cache.residentBytes_,
                                     cache.residentPoints_,
                                     cache.peakResidentBytes_,
                                     cache.clock_,
                                     cache.hits_,
                                     cache.misses_,
                                     cache.insertions_,
                                     cache.replacements_,
                                     cache.evictions_}};
    }
};
} // namespace pci
namespace {
using Access = pci::DecodedPageCacheTestAccess;
constexpr pci::PointCloudSourceId source{1};
constexpr pci::PointCloudSourceId other{2};
pci::PointCloudNodePayloadPtr page(pci::PointCloudNodeId id = {})
{
    auto payload = std::make_shared<pci::PointCloudNodePayload>();
    auto block = std::make_shared<pci::PointBlock>();
    block->points.resize(1);
    payload->nodeId = id;
    payload->blocks.push_back(std::move(block));
    return payload;
}
void populate(pci::DecodedPageCache &cache)
{
    for (const auto id : {source, other}) {
        cache.insert(id, page());
        cache.insert(id, page(pci::childNodeId({}, 0)));
        cache.setPinned(id, std::array{pci::rootPointCloudNode});
    }
    static_cast<void>(cache.find(source, {}));
}
} // namespace

TEST_CASE("decoded cache bulk trim inspects at most each initial entry once",
          "[cache][complexity][unit]")
{
    for (std::uint32_t count : {8U, 32U, 128U}) {
        pci::DecodedPageCache cache(1024 * 1024);
        for (std::uint32_t i = 0; i < count; ++i)
            cache.insert(source, page({.level = 8, .x = i}));
        cache.trim();
        CHECK(Access::inspections(cache) == 0);
        cache.setByteBudget(1);
        CHECK(cache.size() == 0);
        CHECK(cache.metrics().evictions == count);
        CHECK(Access::inspections(cache) <= count);
    }
}

TEST_CASE("decoded cache root preparation preserves complete state on "
          "allocation failure",
          "[cache][allocation][compatibility][unit]")
{
    for (int scenario = 0; scenario < 3; ++scenario) {
        bool succeeded = false;
        for (std::size_t nth = 1; nth < 256 && !succeeded; ++nth) {
            pci::DecodedPageCache cache(1024 * 1024);
            populate(cache);
            const auto lease = cache.peek(other, {});
            auto replacement = scenario == 0 ? cache.peek(source, {}) : page();
            const auto id = scenario == 2 ? pci::PointCloudSourceId{3} : source;
            const auto before = Access::state(cache);
            const auto allocations = Access::allocations(cache);
            Access::fail(cache, nth);
            bool failed = false;
            try {
                auto prepared = cache.prepareSourceRoot(
                    id, replacement, std::array{pci::rootPointCloudNode});
                // No allocation is allowed after preparation, even when the
                // very next allocator request would throw.
                Access::fail(cache, 1);
                prepared->commit();
                succeeded = true;
            } catch (const std::bad_alloc &) {
                failed = true;
            }
            Access::fail(cache, 0);
            CHECK(Access::consistent(cache));
            if (failed) {
                CHECK(Access::state(cache) == before);
                CHECK(Access::allocations(cache) == allocations);
            } else {
                CHECK(cache.peek(id, {}) == replacement);
                CHECK(cache.peek(other, {}) == lease);
                auto clone = cache.clone();
                clone->removeSource(id);
                clone->trim();
                clone->clear();
                CHECK(clone->size() == 0);
                cache.removeSource(id);
                cache.trim();
                cache.clear();
                CHECK(lease->blocks.size() == 1);
            }
        }
        REQUIRE(succeeded);
    }
}

TEST_CASE("decoded cache ineligible traversal preserves recency and leases",
          "[cache][compatibility][unit]")
{
    pci::DecodedPageCache cache(1024 * 1024);
    const auto child = pci::childNodeId({}, 0);
    populate(cache);
    const auto lease = cache.peek(source, child);
    cache.setFrameProtection(std::array{pci::DecodedPageKey{other, child}});
    const auto before = Access::state(cache);
    cache.setByteBudget(1);
    CHECK(Access::inspections(cache) == 4);
    CHECK(Access::state(cache) == before);
    cache.setFrameProtection({});
    cache.trim();
    CHECK_FALSE(cache.contains(other, child));
    CHECK(cache.contains(source, child));
    CHECK(cache.metrics().evictions == 1);
}

TEST_CASE(
    "decoded cache insertion and pin allocation failures preserve live state",
    "[cache][allocation][unit]")
{
    for (int mutation = 0; mutation < 4; ++mutation) {
        bool succeeded = false;
        for (std::size_t nth = 1; nth < 128 && !succeeded; ++nth) {
            pci::DecodedPageCache cache(1024 * 1024);
            populate(cache);
            const auto lease = cache.peek(other, {});
            const auto before = Access::state(cache);
            auto payload = page(mutation == 1 ? pci::rootPointCloudNode
                                              : pci::childNodeId({}, 1));
            Access::fail(cache, nth);
            bool failed = false;
            try {
                if (mutation == 3)
                    cache.setPinned(source,
                                    std::array{pci::childNodeId({}, 0),
                                               pci::childNodeId({}, 1)});
                else
                    cache.insert(mutation == 2 ? pci::PointCloudSourceId{3}
                                               : source,
                                 std::move(payload),
                                 std::array{pci::rootPointCloudNode});
                succeeded = true;
            } catch (const std::bad_alloc &) {
                failed = true;
            }
            Access::fail(cache, 0);
            CHECK(Access::consistent(cache));
            if (failed)
                CHECK(Access::state(cache) == before);
            CHECK(cache.peek(other, {}) == lease);
            cache.setByteBudget(1);
            CHECK(Access::consistent(cache));
            cache.removeSource(source);
            cache.clear();
            CHECK(Access::consistent(cache));
        }
        REQUIRE(succeeded);
    }
}

TEST_CASE(
    "decoded cache touch replacement and skipped pages preserve victim order",
    "[cache][compatibility][unit]")
{
    pci::DecodedPageCache cache(1024 * 1024);
    const auto first = pci::childNodeId({}, 0);
    const auto second = pci::childNodeId({}, 1);
    const auto third = pci::childNodeId({}, 2);
    cache.insert(source, page(first));
    cache.insert(other, page(second));
    cache.insert(source, page(third));
    const auto bytes = cache.residentBytes() / 3;
    static_cast<void>(cache.find(source, first));
    static_cast<void>(cache.peek(other, second)); // Must not touch.
    cache.setPinned(other, std::array{second});
    cache.setByteBudget(bytes * 2);
    CHECK_FALSE(cache.contains(source, third));
    CHECK(cache.contains(other, second));
    cache.setPinned(other, {});
    cache.setByteBudget(bytes);
    CHECK_FALSE(cache.contains(other, second));
    CHECK(cache.contains(source, first));
    cache.setByteBudget(bytes * 3);
    cache.insert(other, page(second));
    cache.insert(source, page(first)); // Replacement becomes newest.
    cache.setByteBudget(bytes);
    CHECK(cache.contains(source, first));
    CHECK_FALSE(cache.contains(other, second));
    CHECK(cache.metrics().replacements == 1);
    CHECK(Access::consistent(cache));
}
