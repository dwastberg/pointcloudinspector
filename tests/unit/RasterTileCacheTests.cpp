#include "raster/RasterTileCache.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

namespace {

constexpr pci::RasterSourceId firstSource{1};
constexpr pci::RasterSourceId secondSource{2};

[[nodiscard]] pci::RasterCacheKey
key(const std::uint32_t x,
    const std::uint64_t generation = 1,
    const pci::RasterSourceId source = firstSource)
{
    return {.sourceId = source,
            .renderGeneration = generation,
            .tile = pci::RasterTileKey{0, x, 0}};
}

[[nodiscard]] pci::RasterTileData tile(const std::size_t bytes = 1024)
{
    pci::RasterTileData data;
    data.rgba.resize(bytes);
    data.rgba.shrink_to_fit();
    return data;
}

[[nodiscard]] std::uint64_t entryBytes()
{
    return tile().byteSize();
}

TEST_CASE("raster tile cache admits and evicts by recency", "[unit][raster]")
{
    pci::RasterTileCache cache(entryBytes() * 3);

    CHECK(cache.insert(key(0), tile(), {}));
    CHECK(cache.insert(key(1), tile(), {}));
    CHECK(cache.insert(key(2), tile(), {}));
    CHECK(cache.size() == 3);
    CHECK(cache.residentBytes() <= cache.byteBudget());

    // Touching an entry makes it the most recent, so the untouched one is
    // evicted next.
    CHECK(cache.find(key(0)) != nullptr);
    CHECK(cache.insert(key(3), tile(), {}));
    CHECK(cache.size() == 3);
    CHECK(cache.contains(key(0)));
    CHECK_FALSE(cache.contains(key(1)));
    CHECK(cache.evictions() == 1);
}

TEST_CASE("raster tile cache never evicts a protected tile", "[unit][raster]")
{
    pci::RasterTileCache cache(entryBytes() * 2);
    CHECK(cache.insert(key(0), tile(), {}));
    CHECK(cache.insert(key(1), tile(), {}));

    // Both resident tiles are on screen this frame, so admission must fail
    // rather than evict what is being drawn.
    const std::array<pci::RasterCacheKey, 2> onScreen{key(0), key(1)};
    CHECK_FALSE(cache.insert(key(2), tile(), onScreen));
    CHECK(cache.contains(key(0)));
    CHECK(cache.contains(key(1)));
    CHECK_FALSE(cache.contains(key(2)));
    CHECK(cache.residentBytes() <= cache.byteBudget());

    // Protecting only one leaves the other evictable.
    const std::array<pci::RasterCacheKey, 1> single{key(1)};
    CHECK(cache.insert(key(2), tile(), single));
    CHECK(cache.contains(key(1)));
    CHECK_FALSE(cache.contains(key(0)));
}

TEST_CASE("raster tile cache refuses a tile that can never fit",
          "[unit][raster]")
{
    pci::RasterTileCache cache(entryBytes() / 2);
    // Admitting this would put the cache permanently over budget, so it is
    // declined rather than accepted and immediately evicted.
    CHECK_FALSE(cache.insert(key(0), tile(), {}));
    CHECK(cache.size() == 0);
    CHECK(cache.residentBytes() == 0);
}

TEST_CASE("raster tile cache evicts immediately when the budget drops",
          "[unit][raster]")
{
    pci::RasterTileCache cache(entryBytes() * 4);
    for (std::uint32_t index = 0; index < 4; ++index) {
        CHECK(cache.insert(key(index), tile(), {}));
    }
    REQUIRE(cache.size() == 4);

    // Lowering a live budget is exactly what leaves residentBytes_ above
    // byteBudget_. The cache must come back under the limit at once, not at
    // the next admission, or the following insert would compare against a
    // wrapped subtraction and silently admit over budget.
    cache.setByteBudget(entryBytes() * 2, {});
    CHECK(cache.residentBytes() <= cache.byteBudget());
    CHECK(cache.size() == 2);

    // The invariant still holds for the very next admission.
    CHECK(cache.insert(key(9), tile(), {}));
    CHECK(cache.residentBytes() <= cache.byteBudget());
    CHECK(cache.size() == 2);
}

TEST_CASE("raster tile cache honours protection while shrinking",
          "[unit][raster]")
{
    pci::RasterTileCache cache(entryBytes() * 4);
    for (std::uint32_t index = 0; index < 4; ++index) {
        CHECK(cache.insert(key(index), tile(), {}));
    }

    // A tile drawn this frame survives a budget cut even when that leaves the
    // cache over its new limit; the alternative is a hole on screen.
    const std::array<pci::RasterCacheKey, 3> onScreen{key(0), key(1), key(2)};
    cache.setByteBudget(entryBytes(), onScreen);
    CHECK(cache.size() == 3);
    CHECK(cache.contains(key(0)));
    CHECK_FALSE(cache.contains(key(3)));
}

TEST_CASE("raster tile cache separates sources and generations",
          "[unit][raster]")
{
    pci::RasterTileCache cache(entryBytes() * 8);
    CHECK(cache.insert(key(0, 1, firstSource), tile(), {}));
    CHECK(cache.insert(key(0, 2, firstSource), tile(), {}));
    CHECK(cache.insert(key(0, 1, secondSource), tile(), {}));
    CHECK(cache.size() == 3);

    // A tile decoded for an obsolete range or ramp is a different entry, not a
    // replacement, so a stale result can never be served as current.
    CHECK(cache.contains(key(0, 1, firstSource)));
    CHECK(cache.contains(key(0, 2, firstSource)));

    // Removing a layer releases its tiles at every generation and leaves other
    // sources untouched.
    CHECK(cache.removeSource(firstSource) == 2);
    CHECK(cache.size() == 1);
    CHECK(cache.contains(key(0, 1, secondSource)));
    CHECK(cache.residentBytes() == entryBytes());
}

TEST_CASE("raster tile cache replaces an existing key in place",
          "[unit][raster]")
{
    pci::RasterTileCache cache(entryBytes() * 2);
    CHECK(cache.insert(key(0), tile(), {}));
    const std::uint64_t before = cache.residentBytes();

    CHECK(cache.insert(key(0), tile(), {}));
    CHECK(cache.size() == 1);
    // Re-inserting must not double-count the entry's bytes.
    CHECK(cache.residentBytes() == before);
}

TEST_CASE("raster tile cache clears to an empty accounting", "[unit][raster]")
{
    pci::RasterTileCache cache(entryBytes() * 4);
    CHECK(cache.insert(key(0), tile(), {}));
    CHECK(cache.insert(key(1), tile(), {}));

    cache.clear();
    CHECK(cache.size() == 0);
    CHECK(cache.residentBytes() == 0);
    CHECK(cache.find(key(0)) == nullptr);
    CHECK(cache.insert(key(0), tile(), {}));
}

} // namespace
