#include "scene/DecodedPageCache.h"

#include "foundation/Hash.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace pci {

std::size_t
DecodedPageKeyHash::operator()(const DecodedPageKey &key) const noexcept
{
    const std::size_t source = std::hash<PointCloudSourceId>{}(key.sourceId);
    const std::size_t node = PointCloudNodeIdHash{}(key.nodeId);
    return hashCombine(source, node);
}

DecodedPageCache::DecodedPageCache(const std::uint64_t byteBudget)
    : byteBudget_(byteBudget)
{
    if (byteBudget == 0) {
        throw std::invalid_argument(
            "decoded page cache budget must be positive");
    }
}

PointCloudNodePayloadPtr
DecodedPageCache::find(const PointCloudSourceId sourceId,
                       const PointCloudNodeId nodeId)
{
    const std::scoped_lock lock(mutex_);
    const auto found = entries_.find({sourceId, nodeId});
    SourceState &source = sources_[sourceId];
    if (found == entries_.end()) {
        ++misses_;
        ++source.misses;
        return {};
    }
    ++hits_;
    ++source.hits;
    found->second.lastUsed = ++clock_;
    return found->second.payload;
}

PointCloudNodePayloadPtr
DecodedPageCache::peek(const PointCloudSourceId sourceId,
                       const PointCloudNodeId nodeId) const
{
    const std::scoped_lock lock(mutex_);
    const auto found = entries_.find({sourceId, nodeId});
    return found == entries_.end() ? PointCloudNodePayloadPtr{}
                                   : found->second.payload;
}

bool DecodedPageCache::contains(const PointCloudSourceId sourceId,
                                const PointCloudNodeId nodeId) const
{
    const std::scoped_lock lock(mutex_);
    return entries_.contains({sourceId, nodeId});
}

void DecodedPageCache::insert(
    const PointCloudSourceId sourceId,
    PointCloudNodePayloadPtr payload,
    const std::span<const PointCloudNodeId> protectedNodes)
{
    if (sourceId == PointCloudSourceId{} || !payload) {
        throw std::invalid_argument(
            "decoded page cache requires a source and payload");
    }
    const std::uint64_t bytes = pointCloudNodePayloadBytes(*payload);
    const std::uint64_t points = pointCloudNodePayloadPoints(*payload);
    const DecodedPageKey key{sourceId, payload->nodeId};
    const std::scoped_lock lock(mutex_);
    SourceState &source = sources_[sourceId];
    if (const auto existing = entries_.find(key); existing != entries_.end()) {
        eraseLocked(existing, false);
        ++replacements_;
        ++source.replacements;
    }
    ++insertions_;
    ++source.insertions;
    residentBytes_ += bytes;
    residentPoints_ += points;
    source.residentBytes += bytes;
    source.residentPoints += points;
    peakResidentBytes_ = std::max(peakResidentBytes_, residentBytes_);
    source.peakResidentBytes =
        std::max(source.peakResidentBytes, source.residentBytes);
    entries_.emplace(key,
                     Entry{
                         .payload = std::move(payload),
                         .bytes = bytes,
                         .points = points,
                         .lastUsed = ++clock_,
                     });
    std::unordered_set<DecodedPageKey, DecodedPageKeyHash> protectedKeys;
    protectedKeys.reserve(protectedNodes.size());
    for (const PointCloudNodeId id : protectedNodes) {
        protectedKeys.insert({sourceId, id});
    }
    trimLocked(protectedKeys);
}

void DecodedPageCache::setPinned(const PointCloudSourceId sourceId,
                                 const std::span<const PointCloudNodeId> nodes)
{
    const std::scoped_lock lock(mutex_);
    for (auto current = pinned_.begin(); current != pinned_.end();) {
        if (current->sourceId == sourceId) {
            current = pinned_.erase(current);
        } else {
            ++current;
        }
    }
    for (const PointCloudNodeId id : nodes) {
        pinned_.insert({sourceId, id});
    }
    trimLocked({});
}

void DecodedPageCache::trim(
    const PointCloudSourceId protectedSource,
    const std::span<const PointCloudNodeId> protectedNodes)
{
    std::unordered_set<DecodedPageKey, DecodedPageKeyHash> protectedKeys;
    protectedKeys.reserve(protectedNodes.size());
    for (const PointCloudNodeId id : protectedNodes) {
        protectedKeys.insert({protectedSource, id});
    }
    const std::scoped_lock lock(mutex_);
    trimLocked(protectedKeys);
}

void DecodedPageCache::removeSource(const PointCloudSourceId sourceId)
{
    const std::scoped_lock lock(mutex_);
    for (auto current = entries_.begin(); current != entries_.end();) {
        if (current->first.sourceId == sourceId) {
            const auto victim = current++;
            eraseLocked(victim, false);
        } else {
            ++current;
        }
    }
    for (auto current = pinned_.begin(); current != pinned_.end();) {
        if (current->sourceId == sourceId) {
            current = pinned_.erase(current);
        } else {
            ++current;
        }
    }
    sources_.erase(sourceId);
}

void DecodedPageCache::replaceSourceRoot(
    const PointCloudSourceId sourceId,
    PointCloudNodePayloadPtr root,
    const std::span<const PointCloudNodeId> pins)
{
    if (sourceId == PointCloudSourceId{} || !root ||
        root->nodeId != rootPointCloudNode) {
        throw std::invalid_argument(
            "decoded page replacement requires a source root");
    }
    const std::uint64_t rootBytes = pointCloudNodePayloadBytes(*root);
    const std::uint64_t rootPoints = pointCloudNodePayloadPoints(*root);

    const std::scoped_lock lock(mutex_);
    auto entries = entries_;
    auto pinned = pinned_;
    auto sources = sources_;
    std::uint64_t residentBytes = residentBytes_;
    std::uint64_t residentPoints = residentPoints_;

    for (auto current = entries.begin(); current != entries.end();) {
        if (current->first.sourceId == sourceId) {
            residentBytes -= current->second.bytes;
            residentPoints -= current->second.points;
            current = entries.erase(current);
        } else {
            ++current;
        }
    }
    for (auto current = pinned.begin(); current != pinned.end();) {
        if (current->sourceId == sourceId) {
            current = pinned.erase(current);
        } else {
            ++current;
        }
    }
    sources.erase(sourceId);

    SourceState state;
    state.insertions = 1;
    state.residentBytes = rootBytes;
    state.peakResidentBytes = rootBytes;
    state.residentPoints = rootPoints;
    sources.emplace(sourceId, state);
    entries.emplace(DecodedPageKey{sourceId, rootPointCloudNode},
                    Entry{.payload = std::move(root),
                          .bytes = rootBytes,
                          .points = rootPoints,
                          .lastUsed = clock_ + 1});
    for (const PointCloudNodeId id : pins) {
        pinned.insert({sourceId, id});
    }
    residentBytes += rootBytes;
    residentPoints += rootPoints;

    entries_.swap(entries);
    pinned_.swap(pinned);
    sources_.swap(sources);
    residentBytes_ = residentBytes;
    residentPoints_ = residentPoints;
    peakResidentBytes_ = std::max(peakResidentBytes_, residentBytes_);
    ++clock_;
    ++insertions_;
}

void DecodedPageCache::clear()
{
    const std::scoped_lock lock(mutex_);
    entries_.clear();
    pinned_.clear();
    sources_.clear();
    residentBytes_ = 0;
    residentPoints_ = 0;
}

void DecodedPageCache::setByteBudget(const std::uint64_t byteBudget)
{
    if (byteBudget == 0) {
        throw std::invalid_argument(
            "decoded page cache budget must be positive");
    }
    const std::scoped_lock lock(mutex_);
    byteBudget_ = byteBudget;
    trimLocked({});
}

std::uint64_t DecodedPageCache::byteBudget() const
{
    const std::scoped_lock lock(mutex_);
    return byteBudget_;
}

std::uint64_t DecodedPageCache::residentBytes() const
{
    const std::scoped_lock lock(mutex_);
    return residentBytes_;
}

std::uint64_t DecodedPageCache::residentPoints() const
{
    const std::scoped_lock lock(mutex_);
    return residentPoints_;
}

std::uint64_t
DecodedPageCache::residentBytes(const PointCloudSourceId sourceId) const
{
    const std::scoped_lock lock(mutex_);
    const auto source = sources_.find(sourceId);
    return source == sources_.end() ? 0 : source->second.residentBytes;
}

std::uint64_t
DecodedPageCache::residentPoints(const PointCloudSourceId sourceId) const
{
    const std::scoped_lock lock(mutex_);
    const auto source = sources_.find(sourceId);
    return source == sources_.end() ? 0 : source->second.residentPoints;
}

std::size_t DecodedPageCache::size() const
{
    const std::scoped_lock lock(mutex_);
    return entries_.size();
}

DecodedCacheMetrics DecodedPageCache::metrics() const
{
    const std::scoped_lock lock(mutex_);
    return {
        .hits = hits_,
        .misses = misses_,
        .insertions = insertions_,
        .replacements = replacements_,
        .evictions = evictions_,
        .residentBytes = residentBytes_,
        .peakResidentBytes = peakResidentBytes_,
        .residentPoints = residentPoints_,
        .byteBudget = byteBudget_,
    };
}

DecodedCacheMetrics
DecodedPageCache::metrics(const PointCloudSourceId sourceId) const
{
    const std::scoped_lock lock(mutex_);
    return metricsLocked(sourceId);
}

void DecodedPageCache::trimLocked(
    const std::unordered_set<DecodedPageKey, DecodedPageKeyHash> &protectedKeys)
{
    while (residentBytes_ > byteBudget_) {
        auto victim = entries_.end();
        std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
        for (auto current = entries_.begin(); current != entries_.end();
             ++current) {
            if (pinned_.contains(current->first) ||
                protectedKeys.contains(current->first) ||
                current->second.payload.use_count() > 1) {
                continue;
            }
            if (current->second.lastUsed < oldest) {
                oldest = current->second.lastUsed;
                victim = current;
            }
        }
        if (victim == entries_.end()) {
            break;
        }
        eraseLocked(victim, true);
    }
}

void DecodedPageCache::eraseLocked(
    std::unordered_map<DecodedPageKey, Entry, DecodedPageKeyHash>::iterator
        entry,
    const bool eviction)
{
    residentBytes_ -= entry->second.bytes;
    residentPoints_ -= entry->second.points;
    SourceState &source = sources_[entry->first.sourceId];
    source.residentBytes -= entry->second.bytes;
    source.residentPoints -= entry->second.points;
    if (eviction) {
        ++evictions_;
        ++source.evictions;
    }
    entries_.erase(entry);
}

DecodedCacheMetrics
DecodedPageCache::metricsLocked(const PointCloudSourceId sourceId) const
{
    const auto found = sources_.find(sourceId);
    if (found == sources_.end()) {
        return {.byteBudget = byteBudget_};
    }
    const SourceState &source = found->second;
    return {
        .hits = source.hits,
        .misses = source.misses,
        .insertions = source.insertions,
        .replacements = source.replacements,
        .evictions = source.evictions,
        .residentBytes = source.residentBytes,
        .peakResidentBytes = source.peakResidentBytes,
        .residentPoints = source.residentPoints,
        .byteBudget = byteBudget_,
    };
}

} // namespace pci
