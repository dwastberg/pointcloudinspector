#include <pci/runtime/point/DecodedPageCache.h>

#include <pci/foundation/Hash.h>

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
    recency_.splice(recency_.end(), recency_, found->second.recency);
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

void DecodedPageCache::applyLookupEffect(const PointCloudSourceId sourceId,
                                         const PointCloudNodeId nodeId,
                                         const bool resident)
{
    const std::scoped_lock lock(mutex_);
    SourceState &source = sources_[sourceId];
    if (!resident) {
        ++misses_;
        ++source.misses;
        return;
    }
    ++hits_;
    ++source.hits;
    if (const auto found = entries_.find({sourceId, nodeId});
        found != entries_.end()) {
        found->second.lastUsed = ++clock_;
        recency_.splice(recency_.end(), recency_, found->second.recency);
    }
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
    Keys protectedKeys{Allocator<DecodedPageKey>{allocation_}};
    protectedKeys.reserve(protectedNodes.size());
    for (const PointCloudNodeId id : protectedNodes)
        protectedKeys.insert({sourceId, id});
    const auto [sourceEntry, newSource] = sources_.try_emplace(sourceId);
    SourceState &source = sourceEntry->second;
    auto existing = entries_.find(key);
    const bool replacement = existing != entries_.end();
    if (!replacement) {
        try {
            recency_.push_back(key);
            try {
                existing =
                    entries_
                        .emplace(key,
                                 Entry{.payload = std::move(payload),
                                       .bytes = bytes,
                                       .points = points,
                                       .lastUsed = clock_ + 1,
                                       .recency = std::prev(recency_.end())})
                        .first;
            } catch (...) {
                recency_.pop_back();
                throw;
            }
        } catch (...) {
            if (newSource)
                sources_.erase(sourceEntry);
            throw;
        }
    } else {
        residentBytes_ -= existing->second.bytes;
        residentPoints_ -= existing->second.points;
        source.residentBytes -= existing->second.bytes;
        source.residentPoints -= existing->second.points;
        existing->second.payload = std::move(payload);
        existing->second.bytes = bytes;
        existing->second.points = points;
        recency_.splice(recency_.end(), recency_, existing->second.recency);
        ++replacements_;
        ++source.replacements;
    }
    existing->second.lastUsed = ++clock_;
    ++insertions_;
    ++source.insertions;
    residentBytes_ += bytes;
    residentPoints_ += points;
    source.residentBytes += bytes;
    source.residentPoints += points;
    peakResidentBytes_ = std::max(peakResidentBytes_, residentBytes_);
    source.peakResidentBytes =
        std::max(source.peakResidentBytes, source.residentBytes);
    trimLocked(protectedKeys);
}

void DecodedPageCache::setPinned(const PointCloudSourceId sourceId,
                                 const std::span<const PointCloudNodeId> nodes)
{
    const std::scoped_lock lock(mutex_);
    Keys next(pinned_);
    std::erase_if(next, [sourceId](const auto &key) {
        return key.sourceId == sourceId;
    });
    for (const auto id : nodes)
        next.insert({sourceId, id});
    pinned_.swap(next);
    trimLocked({});
}

void DecodedPageCache::trim(
    const PointCloudSourceId protectedSource,
    const std::span<const PointCloudNodeId> protectedNodes)
{
    const std::scoped_lock lock(mutex_);
    Keys protectedKeys{Allocator<DecodedPageKey>{allocation_}};
    protectedKeys.reserve(protectedNodes.size());
    for (const PointCloudNodeId id : protectedNodes) {
        protectedKeys.insert({protectedSource, id});
    }
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

struct DecodedPageCache::RootReplacement::State {
    DecodedPageCache &owner;
    std::unique_lock<std::mutex> lock;
    decltype(DecodedPageCache::recency_) recency;
    decltype(DecodedPageCache::entries_) entries;
    decltype(DecodedPageCache::pinned_) pinned;
    decltype(DecodedPageCache::sources_) sources;
    std::uint64_t residentBytes;
    std::uint64_t residentPoints;

    explicit State(DecodedPageCache &cache)
        : owner(cache)
        , lock(cache.mutex_)
        , recency(cache.recency_)
        , entries(cache.entries_)
        , pinned(cache.pinned_)
        , sources(cache.sources_)
        , residentBytes(cache.residentBytes_)
        , residentPoints(cache.residentPoints_)
    {
        for (auto it = recency.begin(); it != recency.end(); ++it)
            entries.at(*it).recency = it;
    }
};

DecodedPageCache::RootReplacement::RootReplacement(std::unique_ptr<State> state)
    : state_(std::move(state))
{
}
DecodedPageCache::RootReplacement::~RootReplacement() = default;

void DecodedPageCache::RootReplacement::commit() noexcept
{
    if (!state_ || !state_->lock.owns_lock()) {
        return;
    }
    auto &cache = state_->owner;
    cache.recency_.swap(state_->recency);
    cache.entries_.swap(state_->entries);
    cache.pinned_.swap(state_->pinned);
    cache.sources_.swap(state_->sources);
    cache.residentBytes_ = state_->residentBytes;
    cache.residentPoints_ = state_->residentPoints;
    cache.peakResidentBytes_ =
        std::max(cache.peakResidentBytes_, cache.residentBytes_);
    ++cache.clock_;
    ++cache.insertions_;
    state_->lock.unlock();
}

void DecodedPageCache::replaceSourceRoot(
    const PointCloudSourceId sourceId,
    PointCloudNodePayloadPtr root,
    const std::span<const PointCloudNodeId> pins)
{
    prepareSourceRoot(sourceId, std::move(root), pins)->commit();
}

std::unique_ptr<DecodedPageCache::RootReplacement>
DecodedPageCache::prepareSourceRoot(
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

    auto state = std::make_unique<RootReplacement::State>(*this);
    auto &entries = state->entries;
    auto &pinned = state->pinned;
    auto &sources = state->sources;
    auto &residentBytes = state->residentBytes;
    auto &residentPoints = state->residentPoints;

    for (auto current = entries.begin(); current != entries.end();) {
        if (current->first.sourceId == sourceId) {
            residentBytes -= current->second.bytes;
            residentPoints -= current->second.points;
            state->recency.erase(current->second.recency);
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

    SourceState sourceState;
    sourceState.insertions = 1;
    sourceState.residentBytes = rootBytes;
    sourceState.peakResidentBytes = rootBytes;
    sourceState.residentPoints = rootPoints;
    sources.emplace(sourceId, sourceState);
    state->recency.push_back({sourceId, rootPointCloudNode});
    entries.emplace(DecodedPageKey{sourceId, rootPointCloudNode},
                    Entry{.payload = std::move(root),
                          .bytes = rootBytes,
                          .points = rootPoints,
                          .lastUsed = clock_ + 1,
                          .recency = std::prev(state->recency.end())});
    for (const PointCloudNodeId id : pins) {
        pinned.insert({sourceId, id});
    }
    residentBytes += rootBytes;
    residentPoints += rootPoints;

    return std::unique_ptr<RootReplacement>(
        new RootReplacement(std::move(state)));
}

std::shared_ptr<DecodedPageCache> DecodedPageCache::clone() const
{
    const std::scoped_lock lock(mutex_);
    auto result = std::make_shared<DecodedPageCache>(byteBudget_);
    result->recency_ = Recency(recency_, result->recency_.get_allocator());
    result->entries_ = Entries(entries_, result->entries_.get_allocator());
    for (auto it = result->recency_.begin(); it != result->recency_.end(); ++it)
        result->entries_.at(*it).recency = it;
    result->pinned_ = Keys(pinned_, result->pinned_.get_allocator());
    result->sources_ = Sources(sources_, result->sources_.get_allocator());
    result->residentBytes_ = residentBytes_;
    result->residentPoints_ = residentPoints_;
    result->peakResidentBytes_ = peakResidentBytes_;
    result->clock_ = clock_;
    result->hits_ = hits_;
    result->misses_ = misses_;
    result->insertions_ = insertions_;
    result->replacements_ = replacements_;
    result->evictions_ = evictions_;
    return result;
}

void DecodedPageCache::clear()
{
    const std::scoped_lock lock(mutex_);
    entries_.clear();
    recency_.clear();
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

void DecodedPageCache::setFrameProtection(std::span<const DecodedPageKey> keys)
{
    const std::scoped_lock lock(mutex_);
    Keys next(keys.begin(),
              keys.end(),
              0,
              DecodedPageKeyHash{},
              std::equal_to<DecodedPageKey>{},
              Allocator<DecodedPageKey>{allocation_});
    frameProtection_.swap(next);
}

void DecodedPageCache::trimLocked(const Keys &protectedKeys)
{
    lastTrimInspections_ = 0;
    auto candidate = recency_.begin();
    while (residentBytes_ > byteBudget_ && candidate != recency_.end()) {
        const auto entry = entries_.find(*candidate++);
        ++lastTrimInspections_;
        if (pinned_.contains(entry->first) ||
            protectedKeys.contains(entry->first) ||
            frameProtection_.contains(entry->first) ||
            entry->second.payload.use_count() > 1)
            continue;
        eraseLocked(entry, true);
    }
}

void DecodedPageCache::eraseLocked(Entries::iterator entry, const bool eviction)
{
    residentBytes_ -= entry->second.bytes;
    residentPoints_ -= entry->second.points;
    SourceState &source = sources_.find(entry->first.sourceId)->second;
    source.residentBytes -= entry->second.bytes;
    source.residentPoints -= entry->second.points;
    if (eviction) {
        ++evictions_;
        ++source.evictions;
    }
    recency_.erase(entry->second.recency);
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
