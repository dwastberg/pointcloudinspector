#include <pci/runtime/raster/RasterTileCache.h>

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace pci {

RasterTileCache::RasterTileCache(const std::uint64_t byteBudget)
    : byteBudget_(byteBudget)
{
}

bool RasterTileCache::contains(const RasterCacheKey &key) const noexcept
{
    return entries_.contains(key);
}

const RasterTileData *RasterTileCache::find(const RasterCacheKey &key)
{
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return nullptr;
    }
    recency_.splice(recency_.end(), recency_, found->second.recency);
    return &found->second.tile;
}

bool RasterTileCache::insert(
    const RasterCacheKey &key,
    RasterTileData tile,
    const std::span<const RasterCacheKey> protectedKeys)
{
    if (const auto existing = entries_.find(key); existing != entries_.end()) {
        eraseEntry(existing);
    }
    const std::uint64_t incoming = tile.byteSize();
    if (!makeRoom(incoming, protectedKeys)) {
        return false;
    }

    recency_.push_back(key);
    auto recency = std::prev(recency_.end());
    entries_.emplace(key,
                     Entry{
                         .tile = std::move(tile),
                         .accountedBytes = incoming,
                         .recency = recency,
                     });
    residentBytes_ += incoming;
    return true;
}

bool RasterTileCache::makeRoom(
    const std::uint64_t incoming,
    const std::span<const RasterCacheKey> protectedKeys)
{
    // Never write incoming + residentBytes_ > byteBudget_: that addition can
    // overflow. Guarding incoming > byteBudget_ first makes the subtraction
    // below safe.
    //
    // The naive rearrangement incoming > byteBudget_ - residentBytes_ is *not*
    // equivalent and is unsafe here: lowering a budget at runtime is exactly
    // what makes residentBytes_ > byteBudget_, and that subtraction would then
    // wrap to a huge value, skip the loop, and admit over budget with no
    // assertion failure because the counters stay internally consistent.
    if (incoming > byteBudget_) {
        return false;
    }
    if (residentBytes_ <= byteBudget_ - incoming) {
        return true; // the common case touches nothing
    }

    // Hashed once per call rather than scanned per candidate. The pathological
    // case is a full cache whose resident tiles are all on screen: the walk
    // below then reaches the end of the recency list, and a linear membership
    // test inside it would make that walk quadratic in the protected set on
    // every admission.
    const std::unordered_set<RasterCacheKey> protectedSet(protectedKeys.begin(),
                                                          protectedKeys.end());
    while (residentBytes_ > byteBudget_ - incoming) {
        const auto victim = std::ranges::find_if(
            recency_, [&protectedSet](const RasterCacheKey &candidate) {
                return !protectedSet.contains(candidate);
            });
        if (victim == recency_.end()) {
            return false;
        }
        const auto entry = entries_.find(*victim);
        if (entry == entries_.end()) {
            recency_.erase(victim);
            continue;
        }
        eraseEntry(entry);
        ++evictions_;
    }
    return true;
}

void RasterTileCache::setByteBudget(
    const std::uint64_t bytes,
    const std::span<const RasterCacheKey> protectedKeys)
{
    byteBudget_ = bytes;
    static_cast<void>(makeRoom(0, protectedKeys)); // restore the invariant now
}

std::size_t RasterTileCache::removeSource(const RasterSourceId sourceId)
{
    std::size_t removed = 0;
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        if (entry->first.sourceId != sourceId) {
            ++entry;
            continue;
        }
        const auto next = std::next(entry);
        eraseEntry(entry);
        entry = next;
        ++removed;
    }
    return removed;
}

void RasterTileCache::clear() noexcept
{
    entries_.clear();
    recency_.clear();
    residentBytes_ = 0;
}

void RasterTileCache::eraseEntry(
    const std::unordered_map<RasterCacheKey, Entry>::iterator entry)
{
    residentBytes_ -= std::min(residentBytes_, entry->second.accountedBytes);
    recency_.erase(entry->second.recency);
    entries_.erase(entry);
}

std::uint64_t RasterTileCache::byteBudget() const noexcept
{
    return byteBudget_;
}

std::uint64_t RasterTileCache::residentBytes() const noexcept
{
    return residentBytes_;
}

std::uint64_t RasterTileCache::evictions() const noexcept
{
    return evictions_;
}

std::size_t RasterTileCache::size() const noexcept
{
    return entries_.size();
}

} // namespace pci
