#pragma once

#include "raster/RasterTileSource.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <span>
#include <unordered_map>

namespace pci {

struct RasterCacheKey {
    RasterSourceId sourceId;
    // Bumped when a style change alters decoded pixels, so tiles decoded for
    // an obsolete range or ramp can never be mistaken for current ones.
    std::uint64_t renderGeneration = 0;
    RasterTileKey tile;
    bool operator==(const RasterCacheKey &) const = default;
};

} // namespace pci

template <> struct std::hash<pci::RasterCacheKey> {
    [[nodiscard]] std::size_t
    operator()(const pci::RasterCacheKey &key) const noexcept;
};

namespace pci {

// Byte-accounted LRU for decoded CPU tiles. Tiles drawn this frame and their
// fallback ancestors are protected: admission must never evict what is on
// screen.
class RasterTileCache {
public:
    explicit RasterTileCache(std::uint64_t byteBudget);

    // Returns false when the tile could not be admitted, which happens when it
    // cannot ever fit or when only protected tiles remain. The caller skips
    // admission rather than exceeding the budget.
    [[nodiscard]] bool insert(const RasterCacheKey &key,
                              RasterTileData tile,
                              std::span<const RasterCacheKey> protectedKeys);
    // Touches the entry's recency.
    [[nodiscard]] const RasterTileData *find(const RasterCacheKey &key);
    [[nodiscard]] bool contains(const RasterCacheKey &key) const noexcept;

    // Evicts down to the new limit immediately rather than waiting for the
    // next insert. A live decrease is exactly what leaves the cache over
    // budget, and deferring would let the next admission compare against a
    // wrapped subtraction.
    void setByteBudget(std::uint64_t bytes,
                       std::span<const RasterCacheKey> protectedKeys);

    // Removing a layer releases its decoded tiles regardless of generation.
    std::size_t removeSource(RasterSourceId sourceId);
    void clear() noexcept;

    [[nodiscard]] std::uint64_t byteBudget() const noexcept;
    [[nodiscard]] std::uint64_t residentBytes() const noexcept;
    [[nodiscard]] std::uint64_t evictions() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Entry {
        RasterTileData tile;
        std::uint64_t accountedBytes = 0;
        std::list<RasterCacheKey>::iterator recency;
    };

    [[nodiscard]] bool makeRoom(std::uint64_t incoming,
                                std::span<const RasterCacheKey> protectedKeys);
    void eraseEntry(std::unordered_map<RasterCacheKey, Entry>::iterator entry);

    std::uint64_t byteBudget_ = 0;
    std::uint64_t residentBytes_ = 0;
    std::uint64_t evictions_ = 0;
    // Front is least recently used.
    std::list<RasterCacheKey> recency_;
    std::unordered_map<RasterCacheKey, Entry> entries_;
};

} // namespace pci
