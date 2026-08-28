#include "scene/PointCloudScene.h"

#include "foundation/CheckedArithmetic.h"
#include "scene/BlockPartitioner.h"
#include "scene/RasterColorizedPointSource.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace pci {

struct PointCloudSceneInvalidationState {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::function<void()>> callbacks;
    std::uint64_t nextId = 1;
};

struct PointCloudScene::HierarchyDecodeRequest {
    TaskScheduler::TaskId taskId;
    std::stop_source stop;
    std::atomic_bool finished = false;
};

struct PointCloudScene::Storage {
    struct FlatSceneStorage {
        std::vector<SceneBlock> blocks;
        std::uint64_t totalPoints = 0;
        std::uint64_t residentBytes = 0;
        PointMemoryBudget::ReservationPtr residentMemoryReservation;
        std::uint64_t nextBlockId = 1;
        Bounds3d blockBounds;
        std::shared_ptr<std::vector<std::uint32_t>> displacedColors;
        PointMemoryBudget::ReservationPtr colorReservation;
    };

    struct HierarchicalSceneStorage {
        PointCloudDataSourcePtr dataSource;
        PointCloudNodePayloadPtr rootPayload;
        PointCloudDataSourcePtr baseDataSource;
        PointCloudNodePayloadPtr sourceRootPayload;
        std::uint64_t rootPayloadRevision = 0;
        PointCloudSourceId sourceId;
        HierarchyResidencyCoordinatorPtr standaloneResidencyCoordinator;
        HierarchyResidencyCoordinator::ParticipantPtr residencyParticipant;
        DecodedPageCachePtr decodedPageCache;
        std::shared_ptr<TaskScheduler> decodeScheduler;
        bool sharedDocumentResources = false;
        std::unordered_map<PointCloudNodeId,
                           std::shared_ptr<HierarchyDecodeRequest>,
                           PointCloudNodeIdHash>
            decodeRequests;
        std::unordered_set<PointCloudNodeId, PointCloudNodeIdHash> failedNodes;
        std::unordered_set<PointCloudNodeId, PointCloudNodeIdHash> pinnedNodes;
        std::size_t outstandingDecodeCallbacks = 0;
        bool suspendPinnedResubmission = false;
        std::string hierarchyError;
        std::uint64_t decodeRequestsQueued = 0;
        std::uint64_t decodeRequestsStarted = 0;
        std::uint64_t decodeRequestsCompleted = 0;
        std::uint64_t decodeRequestsCancelled = 0;
        std::uint64_t decodeRequestsFailed = 0;
    };

    explicit Storage(FlatSceneStorage flat)
        : value(std::move(flat))
    {
    }

    explicit Storage(HierarchicalSceneStorage hierarchical)
        : value(std::move(hierarchical))
    {
    }

    std::variant<FlatSceneStorage, HierarchicalSceneStorage> value;
};

namespace {

std::uint64_t estimatedDecodeAllowance(const std::uint64_t pointCount) noexcept
{
    constexpr std::uint64_t bytesPerPoint =
        sizeof(PointSample) + sizeof(GpuPoint) + sizeof(PointAttributes);
    return saturatingMultiply(pointCount, bytesPerPoint);
}

std::uint64_t
maximumRootBytesPerPoint(const PointCloudNodePayload &payload) noexcept
{
    std::uint64_t result = 0;
    for (const PointBlockPtr &block : payload.blocks) {
        if (!block || block->points.empty()) {
            continue;
        }
        result = std::max<std::uint64_t>(
            result,
            sizeof(GpuPoint) + (block->attributes.size() == block->points.size()
                                    ? sizeof(PointAttributes)
                                    : 0));
    }
    return result;
}

PointCloudNodePayloadPtr
reduceRootPayloadToCount(const PointCloudNodePayload &source,
                         const std::uint64_t targetPoints)
{
    const std::uint64_t totalPoints = pointCloudNodePayloadPoints(source);
    if (targetPoints == 0 || targetPoints > totalPoints) {
        throw std::invalid_argument("invalid hierarchy root point count");
    }

    std::vector<std::uint64_t> selected;
    selected.reserve(static_cast<std::size_t>(targetPoints));
    for (std::uint64_t rank = 0; rank < targetPoints; ++rank) {
        const long double position = static_cast<long double>(rank) *
                                     static_cast<long double>(totalPoints) /
                                     static_cast<long double>(targetPoints);
        selected.push_back(static_cast<std::uint64_t>(position));
    }

    std::vector<std::size_t> selectedPerBlock(source.blocks.size(), 0);
    std::size_t blockIndex = 0;
    std::uint64_t blockBegin = 0;
    for (const std::uint64_t point : selected) {
        while (blockIndex < source.blocks.size()) {
            const PointBlockPtr &block = source.blocks[blockIndex];
            const std::uint64_t blockEnd =
                blockBegin + (block ? block->points.size() : 0);
            if (point < blockEnd) {
                ++selectedPerBlock[blockIndex];
                break;
            }
            blockBegin = blockEnd;
            ++blockIndex;
        }
    }

    std::vector<std::shared_ptr<PointBlock>> mutableBlocks(
        source.blocks.size());
    for (std::size_t index = 0; index < source.blocks.size(); ++index) {
        const PointBlockPtr &block = source.blocks[index];
        if (!block || selectedPerBlock[index] == 0) {
            continue;
        }
        auto reduced = std::make_shared<PointBlock>();
        reduced->origin = block->origin;
        reduced->scale = block->scale;
        // Retain conservative source-block bounds. Root sampling must not make
        // later detail regions disappear from frustum selection.
        reduced->bounds = block->bounds;
        reduced->intensityMinimum = block->intensityMinimum;
        reduced->intensityMaximum = block->intensityMaximum;
        reduced->points.reserve(selectedPerBlock[index]);
        if (block->attributes.size() == block->points.size()) {
            reduced->attributes.reserve(selectedPerBlock[index]);
        }
        mutableBlocks[index] = std::move(reduced);
    }

    blockIndex = 0;
    blockBegin = 0;
    for (const std::uint64_t point : selected) {
        while (blockIndex < source.blocks.size()) {
            const PointBlockPtr &block = source.blocks[blockIndex];
            const std::uint64_t blockEnd =
                blockBegin + (block ? block->points.size() : 0);
            if (point < blockEnd) {
                const std::size_t local =
                    static_cast<std::size_t>(point - blockBegin);
                mutableBlocks[blockIndex]->points.push_back(
                    block->points[local]);
                if (block->attributes.size() == block->points.size()) {
                    mutableBlocks[blockIndex]->attributes.push_back(
                        block->attributes[local]);
                }
                break;
            }
            blockBegin = blockEnd;
            ++blockIndex;
        }
    }

    auto result = std::make_shared<PointCloudNodePayload>();
    result->nodeId = rootPointCloudNode;
    result->sourcePointCount = source.sourcePointCount;
    for (std::shared_ptr<PointBlock> &block : mutableBlocks) {
        if (block) {
            result->blocks.push_back(std::move(block));
        }
    }
    return result;
}

PointCloudNodePayloadPtr reducedRootPayload(const PointCloudNodePayload &source,
                                            const std::uint64_t maximumBytes)
{
    const std::uint64_t totalPoints = pointCloudNodePayloadPoints(source);
    const std::uint64_t bytesPerPoint = maximumRootBytesPerPoint(source);
    if (totalPoints == 0 || bytesPerPoint == 0 ||
        maximumBytes < bytesPerPoint) {
        throw std::length_error(
            "hierarchy root budget cannot retain one preview point");
    }
    std::uint64_t targetPoints =
        std::min(totalPoints, maximumBytes / bytesPerPoint);

    while (targetPoints > 0) {
        PointCloudNodePayloadPtr result =
            reduceRootPayloadToCount(source, targetPoints);
        const std::uint64_t actualBytes = pointCloudNodePayloadBytes(*result);
        if (actualBytes <= maximumBytes) {
            return result;
        }
        const std::uint64_t excess = actualBytes - maximumBytes;
        const std::uint64_t reduction = std::max<std::uint64_t>(
            1, (excess + bytesPerPoint - 1) / bytesPerPoint);
        targetPoints = reduction >= targetPoints ? 0 : targetPoints - reduction;
    }
    throw std::length_error(
        "hierarchy root budget cannot retain one preview point");
}

struct ReducedRootPair {
    PointCloudNodePayloadPtr source;
    PointCloudNodePayloadPtr colored;
    std::uint64_t bytes = 0;
};

ReducedRootPair reducedRootPayloadPair(const PointCloudNodePayload &source,
                                       const PointCloudNodePayload &colored,
                                       const std::uint64_t maximumBytes)
{
    const std::uint64_t totalPoints = pointCloudNodePayloadPoints(source);
    if (totalPoints == 0 ||
        totalPoints != pointCloudNodePayloadPoints(colored)) {
        throw std::logic_error(
            "paired hierarchy roots have different point counts");
    }
    const std::uint64_t bytesPerPoint = saturatingAdd(
        maximumRootBytesPerPoint(source), maximumRootBytesPerPoint(colored));
    if (bytesPerPoint == 0 || maximumBytes < bytesPerPoint) {
        throw std::length_error(
            "hierarchy root budget cannot retain one colored preview point");
    }
    std::uint64_t targetPoints =
        std::min(totalPoints, maximumBytes / bytesPerPoint);
    while (targetPoints > 0) {
        ReducedRootPair result{
            .source = reduceRootPayloadToCount(source, targetPoints),
            .colored = reduceRootPayloadToCount(colored, targetPoints),
        };
        result.bytes =
            saturatingAdd(pointCloudNodePayloadBytes(*result.source),
                          pointCloudNodePayloadBytes(*result.colored));
        if (result.bytes <= maximumBytes) {
            return result;
        }
        const std::uint64_t excess = result.bytes - maximumBytes;
        const std::uint64_t reduction = std::max<std::uint64_t>(
            1, (excess + bytesPerPoint - 1) / bytesPerPoint);
        targetPoints = reduction >= targetPoints ? 0 : targetPoints - reduction;
    }
    throw std::length_error(
        "hierarchy root budget cannot retain one colored preview point");
}

PointCloudSourceId nextPointCloudSourceId()
{
    static std::atomic_uint64_t next = 1;
    const std::uint64_t value = next.fetch_add(1);
    if (value == 0 || value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("point-cloud source ids are exhausted");
    }
    return PointCloudSourceId{value};
}

} // namespace

PointCloudSceneInvalidationSubscription::
    PointCloudSceneInvalidationSubscription(
        std::weak_ptr<PointCloudSceneInvalidationState> state,
        const std::uint64_t id) noexcept
    : state_(std::move(state))
    , id_(id)
{
}

PointCloudSceneInvalidationSubscription::
    ~PointCloudSceneInvalidationSubscription()
{
    reset();
}

PointCloudSceneInvalidationSubscription::
    PointCloudSceneInvalidationSubscription(
        PointCloudSceneInvalidationSubscription &&other) noexcept
    : state_(std::move(other.state_))
    , id_(std::exchange(other.id_, 0))
{
}

PointCloudSceneInvalidationSubscription &
PointCloudSceneInvalidationSubscription::operator=(
    PointCloudSceneInvalidationSubscription &&other) noexcept
{
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

void PointCloudSceneInvalidationSubscription::reset() noexcept
{
    if (id_ != 0) {
        if (const auto state = state_.lock()) {
            const std::scoped_lock lock(state->mutex);
            state->callbacks.erase(id_);
        }
    }
    state_.reset();
    id_ = 0;
}

PointCloudSceneInvalidationSubscription::operator bool() const noexcept
{
    return id_ != 0 && !state_.expired();
}

PointCloudScene::PointCloudScene(PointCloudMetadata metadata)
    : metadata_(std::move(metadata))
    , storage_(std::make_unique<Storage>(Storage::FlatSceneStorage{}))
    , invalidationState_(std::make_shared<PointCloudSceneInvalidationState>())
{
}

PointCloudScene::PointCloudScene(PointCloudMetadata metadata,
                                 PointCloudDataSourcePtr dataSource,
                                 PointCloudNodePayloadPtr rootPayload,
                                 const std::uint64_t decodedByteBudget,
                                 const bool loadingComplete)
    : metadata_(std::move(metadata))
    , invalidationState_(std::make_shared<PointCloudSceneInvalidationState>())
{
    if (!dataSource || !rootPayload ||
        rootPayload->nodeId != rootPointCloudNode) {
        throw std::invalid_argument(
            "hierarchical scene requires a source and root payload");
    }
    Storage::HierarchicalSceneStorage hierarchical;
    hierarchical.dataSource = std::move(dataSource);
    hierarchical.rootPayload = std::move(rootPayload);
    hierarchical.sourceId = nextPointCloudSourceId();
    hierarchical.standaloneResidencyCoordinator =
        std::make_shared<HierarchyResidencyCoordinator>(decodedByteBudget, 1);
    hierarchical.residencyParticipant =
        hierarchical.standaloneResidencyCoordinator->registerParticipant(
            pointCloudNodePayloadBytes(*hierarchical.rootPayload));
    hierarchical.decodedPageCache = std::make_shared<DecodedPageCache>(
        hierarchical.residencyParticipant->byteBudget());
    hierarchical.decodeScheduler = std::make_shared<TaskScheduler>(
        1,
        std::max<std::uint64_t>(
            1,
            std::min<std::uint64_t>(TaskScheduler::defaultActiveByteBudget,
                                    decodedByteBudget)));
    hierarchical.decodedPageCache->insert(hierarchical.sourceId,
                                          hierarchical.rootPayload);
    const std::array root{rootPointCloudNode};
    hierarchical.decodedPageCache->setPinned(hierarchical.sourceId, root);
    updateStatistics(*hierarchical.rootPayload);
    hierarchical.rootPayloadRevision = 1;
    storage_ = std::make_unique<Storage>(std::move(hierarchical));
    revision_ = 1;
    loadingComplete_ = loadingComplete;
}

PointCloudScene::~PointCloudScene()
{
    cancelAndWaitForDecodes();
    if (auto *hierarchical =
            std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value)) {
        if (hierarchical->decodedPageCache &&
            hierarchical->sourceId != PointCloudSourceId{}) {
            hierarchical->decodedPageCache->removeSource(
                hierarchical->sourceId);
        }
        hierarchical->residencyParticipant.reset();
    }
}

const PointCloudMetadata &PointCloudScene::metadata() const noexcept
{
    return metadata_;
}

void PointCloudScene::addBlock(PointBlockPtr block)
{
    if (!block || block->points.empty()) {
        throw std::invalid_argument("scene blocks must contain points");
    }
    {
        const std::scoped_lock lock(mutex_);
        auto *flat = std::get_if<Storage::FlatSceneStorage>(&storage_->value);
        if (!flat) {
            throw std::logic_error(
                "hierarchical scenes do not accept flat blocks");
        }
        const std::uint64_t blockBytes =
            block->points.capacity() * sizeof(GpuPoint) +
            block->attributes.capacity() * sizeof(PointAttributes);
        if (flat->residentMemoryReservation &&
            (flat->residentBytes > flat->residentMemoryReservation->bytes() ||
             blockBytes > flat->residentMemoryReservation->bytes() -
                              flat->residentBytes)) {
            throw std::length_error(
                "flat scene exceeded its admitted resident-memory reservation");
        }
        if (flat->blocks.empty()) {
            flat->blockBounds = block->bounds;
        } else {
            flat->blockBounds.extend(block->bounds);
        }
        flat->totalPoints += block->points.size();
        flat->residentBytes += blockBytes;
        if (!haveIntensityStatistics_) {
            intensityMinimum_ = block->intensityMinimum;
            intensityMaximum_ = block->intensityMaximum;
            haveIntensityStatistics_ = true;
        } else {
            intensityMinimum_ =
                std::min(intensityMinimum_, block->intensityMinimum);
            intensityMaximum_ =
                std::max(intensityMaximum_, block->intensityMaximum);
        }
        updateClassifications(*block);
        ++revision_;
        flat->blocks.push_back({
            .id = flat->nextBlockId++,
            .block = std::move(block),
        });
    }
    notifyInvalidated();
}

void PointCloudScene::setResidentMemoryReservation(
    PointMemoryBudget::ReservationPtr reservation)
{
    const std::scoped_lock lock(mutex_);
    auto *flat = std::get_if<Storage::FlatSceneStorage>(&storage_->value);
    if (!flat) {
        throw std::logic_error(
            "hierarchical scenes do not use flat-memory reservations");
    }
    if (flat->residentMemoryReservation) {
        throw std::logic_error("flat scene already owns a memory reservation");
    }
    if (reservation && flat->residentBytes > reservation->bytes()) {
        throw std::length_error(
            "flat scene payload is larger than its memory reservation");
    }
    flat->residentMemoryReservation = std::move(reservation);
}

std::vector<PointBlockPtr> PointCloudScene::blocks() const
{
    const std::scoped_lock lock(mutex_);
    if (const auto *hierarchical =
            std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value)) {
        return hierarchical->rootPayload->blocks;
    }
    const auto &flat = std::get<Storage::FlatSceneStorage>(storage_->value);
    std::vector<PointBlockPtr> result;
    result.reserve(flat.blocks.size());
    std::ranges::transform(
        flat.blocks, std::back_inserter(result), &SceneBlock::block);
    return result;
}

std::vector<SceneBlock> PointCloudScene::blockEntries() const
{
    const std::scoped_lock lock(mutex_);
    if (const auto *flat =
            std::get_if<Storage::FlatSceneStorage>(&storage_->value)) {
        return flat->blocks;
    }
    return {};
}

std::uint64_t PointCloudScene::totalPointCount() const
{
    const std::scoped_lock lock(mutex_);
    if (const auto *flat =
            std::get_if<Storage::FlatSceneStorage>(&storage_->value)) {
        return flat->totalPoints;
    }
    return metadata_.sourcePointCount;
}

std::uint64_t PointCloudScene::revision() const
{
    return revision_.load(std::memory_order_acquire);
}

Bounds3d PointCloudScene::bounds() const
{
    const std::scoped_lock lock(mutex_);
    const auto *flat = std::get_if<Storage::FlatSceneStorage>(&storage_->value);
    return !flat || flat->blocks.empty() ? metadata_.sourceBounds
                                         : flat->blockBounds;
}

PointCloudScalarRanges PointCloudScene::scalarRanges() const
{
    const std::scoped_lock lock(mutex_);
    return completeScalarRangesLocked();
}

PointClassificationFilter PointCloudScene::presentClassifications() const
{
    const std::scoped_lock lock(mutex_);
    return presentClassifications_;
}

std::uint16_t PointCloudScene::intensityMinimum() const
{
    const std::scoped_lock lock(mutex_);
    return intensityMinimum_;
}

std::uint16_t PointCloudScene::intensityMaximum() const
{
    const std::scoped_lock lock(mutex_);
    return intensityMaximum_;
}

PointCloudSceneSnapshot PointCloudScene::snapshot() const
{
    const std::scoped_lock lock(mutex_);
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    const auto *flat = std::get_if<Storage::FlatSceneStorage>(&storage_->value);
    PointCloudSceneSnapshot result{
        .revision = revision_.load(std::memory_order_relaxed),
        .rootPayloadRevision =
            hierarchical ? hierarchical->rootPayloadRevision : 0,
        .hierarchical = hierarchical != nullptr,
        .loadingComplete = loadingComplete_,
        .sourcePointCount = metadata_.sourcePointCount,
        .retainedFlatPointCount = flat ? flat->totalPoints : 0,
        .decodedResidentPointCount =
            hierarchical ? hierarchical->decodedPageCache->residentPoints(
                               hierarchical->sourceId)
                         : flat->totalPoints,
        .decodedResidentBytes =
            hierarchical ? hierarchical->decodedPageCache->residentBytes(
                               hierarchical->sourceId)
                         : flat->residentBytes,
        .bounds = !flat || flat->blocks.empty() ? metadata_.sourceBounds
                                                : flat->blockBounds,
        .scalarRanges = completeScalarRangesLocked(),
        .intensityMinimum = intensityMinimum_,
        .intensityMaximum = intensityMaximum_,
        .flatBlocks = {},
    };
    if (hierarchical) {
        return result;
    }

    result.flatBlocks.reserve(flat->blocks.size());
    for (const SceneBlock &entry : flat->blocks) {
        const auto center = entry.block->bounds.center();
        const Vec3d diagonal{
            entry.block->bounds.maximum[0] - entry.block->bounds.minimum[0],
            entry.block->bounds.maximum[1] - entry.block->bounds.minimum[1],
            entry.block->bounds.maximum[2] - entry.block->bounds.minimum[2],
        };
        result.flatBlocks.push_back({
            .id = entry.id,
            .block = entry.block,
            .center = {center[0], center[1], center[2]},
            .radius = length(diagonal) * 0.5,
            .pointCount = entry.block->points.size(),
            .byteSize =
                entry.block->points.capacity() * sizeof(GpuPoint) +
                entry.block->attributes.capacity() * sizeof(PointAttributes),
        });
    }
    return result;
}

void PointCloudScene::markLoadingComplete()
{
    {
        const std::scoped_lock lock(mutex_);
        if (loadingComplete_) {
            return;
        }
        loadingComplete_ = true;
        ++revision_;
    }
    notifyInvalidated();
}

PointCloudSceneInvalidationSubscription
PointCloudScene::subscribeInvalidation(std::function<void()> callback)
{
    if (!callback) {
        return {};
    }
    const auto state = invalidationState_;
    const std::scoped_lock lock(state->mutex);
    const std::uint64_t id = state->nextId++;
    state->callbacks.emplace(id, std::move(callback));
    return PointCloudSceneInvalidationSubscription(state, id);
}

void PointCloudScene::notifyInvalidated() noexcept
{
    std::vector<std::function<void()>> callbacks;
    try {
        const auto state = invalidationState_;
        {
            const std::scoped_lock lock(state->mutex);
            callbacks.reserve(state->callbacks.size());
            for (const auto &[id, callback] : state->callbacks) {
                callbacks.push_back(callback);
            }
        }
        for (const auto &callback : callbacks) {
            try {
                callback();
            } catch (...) {
                // An observer must never make a committed scene mutation fail.
            }
        }
    } catch (...) {
        // Invalidation is advisory; revision polling on the next requested
        // frame remains a safe fallback if notification allocation fails.
    }
}

bool PointCloudScene::hierarchical() const noexcept
{
    return std::holds_alternative<Storage::HierarchicalSceneStorage>(
        storage_->value);
}

bool PointCloudScene::detailLimited() const noexcept
{
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    return hierarchical && hierarchical->dataSource->detailLimited();
}

PointCloudNode PointCloudScene::rootNode() const
{
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    if (!hierarchical) {
        throw std::logic_error("scene has no hierarchy");
    }
    return hierarchical->dataSource->rootNode();
}

PointCloudNode PointCloudScene::node(const PointCloudNodeId id) const
{
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    if (!hierarchical) {
        throw std::logic_error("scene has no hierarchy");
    }
    return hierarchical->dataSource->node(id);
}

PointCloudNodePayloadPtr PointCloudScene::nodePayload(const PointCloudNodeId id)
{
    DecodedPageCachePtr cache;
    PointCloudSourceId sourceId;
    {
        const std::scoped_lock lock(mutex_);
        if (const auto *hierarchical =
                std::get_if<Storage::HierarchicalSceneStorage>(
                    &storage_->value)) {
            cache = hierarchical->decodedPageCache;
            sourceId = hierarchical->sourceId;
        }
    }
    return cache ? cache->find(sourceId, id) : PointCloudNodePayloadPtr{};
}

PointCloudNodePayloadPtr
PointCloudScene::peekNodePayload(const PointCloudNodeId id) const
{
    DecodedPageCachePtr cache;
    PointCloudSourceId sourceId;
    {
        const std::scoped_lock lock(mutex_);
        if (const auto *hierarchical =
                std::get_if<Storage::HierarchicalSceneStorage>(
                    &storage_->value)) {
            cache = hierarchical->decodedPageCache;
            sourceId = hierarchical->sourceId;
        }
    }
    return cache ? cache->peek(sourceId, id) : PointCloudNodePayloadPtr{};
}

std::optional<PointCloudFullDetailInfo> PointCloudScene::fullDetailInfo() const
{
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    return hierarchical ? hierarchical->dataSource->fullDetailInfo()
                        : std::nullopt;
}

std::uint64_t PointCloudScene::minimumRootPayloadBytes() const
{
    if (!hierarchical()) {
        return 0;
    }
    const std::scoped_lock lock(mutex_);
    const auto &storage =
        std::get<Storage::HierarchicalSceneStorage>(storage_->value);
    const std::uint64_t active = maximumRootBytesPerPoint(*storage.rootPayload);
    return storage.sourceRootPayload
               ? saturatingAdd(
                     active,
                     maximumRootBytesPerPoint(*storage.sourceRootPayload))
               : active;
}

std::uint64_t
PointCloudScene::limitRootPayloadBytes(const std::uint64_t maximumBytes)
{
    if (!hierarchical()) {
        return 0;
    }
    PointCloudNodePayloadPtr currentRoot;
    PointCloudNodePayloadPtr currentSourceRoot;
    {
        const std::scoped_lock lock(mutex_);
        const auto &storage =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        currentRoot = storage.rootPayload;
        currentSourceRoot = storage.sourceRootPayload;
    }
    const std::uint64_t currentBytes = saturatingAdd(
        pointCloudNodePayloadBytes(*currentRoot),
        currentSourceRoot ? pointCloudNodePayloadBytes(*currentSourceRoot) : 0);
    if (currentBytes <= maximumBytes) {
        return currentBytes;
    }
    PointCloudNodePayloadPtr reduced;
    PointCloudNodePayloadPtr reducedSource;
    std::uint64_t reducedBytes = 0;
    if (currentSourceRoot) {
        ReducedRootPair pair = reducedRootPayloadPair(
            *currentSourceRoot, *currentRoot, maximumBytes);
        reducedSource = std::move(pair.source);
        reduced = std::move(pair.colored);
        reducedBytes = pair.bytes;
    } else {
        reduced = reducedRootPayload(*currentRoot, maximumBytes);
        reducedBytes = pointCloudNodePayloadBytes(*reduced);
    }

    DecodedPageCachePtr cache;
    HierarchyResidencyCoordinator::ParticipantPtr participant;
    PointCloudSourceId sourceId;
    {
        const std::scoped_lock lock(mutex_);
        auto &hierarchicalStorage =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        if (hierarchicalStorage.rootPayload != currentRoot ||
            hierarchicalStorage.sourceRootPayload != currentSourceRoot) {
            return saturatingAdd(
                pointCloudNodePayloadBytes(*hierarchicalStorage.rootPayload),
                hierarchicalStorage.sourceRootPayload
                    ? pointCloudNodePayloadBytes(
                          *hierarchicalStorage.sourceRootPayload)
                    : 0);
        }
        hierarchicalStorage.rootPayload = reduced;
        if (currentSourceRoot) {
            hierarchicalStorage.sourceRootPayload = reducedSource;
        }
        ++hierarchicalStorage.rootPayloadRevision;
        cache = hierarchicalStorage.decodedPageCache;
        participant = hierarchicalStorage.residencyParticipant;
        sourceId = hierarchicalStorage.sourceId;
        ++revision_;
    }
    participant->setRetainedRootBytes(reducedBytes);
    const std::array protectedRoot{rootPointCloudNode};
    cache->insert(sourceId, std::move(reduced), protectedRoot);
    notifyInvalidated();
    return reducedBytes;
}

void PointCloudScene::requestNodes(
    const std::span<const PointCloudNodeId> nodes)
{
    if (!hierarchical()) {
        return;
    }
    std::vector<
        std::pair<std::shared_ptr<TaskScheduler>, TaskScheduler::TaskId>>
        cancellations;
    {
        const std::scoped_lock lock(mutex_);
        auto &hierarchicalStorage =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        std::unordered_set<PointCloudNodeId, PointCloudNodeIdHash> desired =
            hierarchicalStorage.pinnedNodes;
        desired.insert(nodes.begin(), nodes.end());
        for (const auto &[id, request] : hierarchicalStorage.decodeRequests) {
            if (!desired.contains(id)) {
                request->stop.request_stop();
                cancellations.emplace_back(hierarchicalStorage.decodeScheduler,
                                           request->taskId);
            }
        }
        for (const PointCloudNodeId id : desired) {
            submitDecodeLocked(id);
        }
    }
    for (const auto &[scheduler, taskId] : cancellations) {
        if (scheduler) {
            static_cast<void>(scheduler->cancel(taskId));
        }
    }
}

void PointCloudScene::setPinnedNodes(
    const std::span<const PointCloudNodeId> nodes)
{
    if (!hierarchical()) {
        return;
    }
    {
        const std::scoped_lock lock(mutex_);
        auto &hierarchicalStorage =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        hierarchicalStorage.pinnedNodes.clear();
        hierarchicalStorage.pinnedNodes.insert(nodes.begin(), nodes.end());
    }
    updateCachePins();
    requestNodes({});
}

void PointCloudScene::setDocumentHierarchyResources(
    HierarchyResidencyCoordinatorPtr coordinator,
    DecodedPageCachePtr cache,
    std::shared_ptr<TaskScheduler> scheduler,
    const bool active)
{
    if (!hierarchical()) {
        return;
    }
    if (!coordinator || !cache || !scheduler) {
        throw std::invalid_argument(
            "hierarchical scene requires shared document resources");
    }
    cancelAndWaitForDecodes();
    auto &hierarchicalStorage =
        std::get<Storage::HierarchicalSceneStorage>(storage_->value);
    auto participant = coordinator->registerParticipant(
        pointCloudNodePayloadBytes(*hierarchicalStorage.rootPayload), active);

    std::vector<PointCloudNodeId> pins{rootPointCloudNode};
    {
        const std::scoped_lock lock(mutex_);
        pins.insert(pins.end(),
                    hierarchicalStorage.pinnedNodes.begin(),
                    hierarchicalStorage.pinnedNodes.end());
    }
    cache->insert(hierarchicalStorage.sourceId,
                  hierarchicalStorage.rootPayload);
    cache->setPinned(hierarchicalStorage.sourceId, pins);

    DecodedPageCachePtr previousCache;
    DecodedPageCachePtr currentCache;
    HierarchyResidencyCoordinator::ParticipantPtr previousParticipant;
    {
        const std::scoped_lock lock(mutex_);
        previousCache = std::move(hierarchicalStorage.decodedPageCache);
        previousParticipant =
            std::move(hierarchicalStorage.residencyParticipant);
        hierarchicalStorage.decodedPageCache = std::move(cache);
        currentCache = hierarchicalStorage.decodedPageCache;
        hierarchicalStorage.decodeScheduler = std::move(scheduler);
        hierarchicalStorage.residencyParticipant = std::move(participant);
        hierarchicalStorage.sharedDocumentResources = true;
        ++revision_;
    }
    if (previousCache && previousCache != currentCache) {
        previousCache->removeSource(hierarchicalStorage.sourceId);
    }
    previousParticipant.reset();
    requestNodes({});
    notifyInvalidated();
}

void PointCloudScene::useStandaloneHierarchyResidency()
{
    if (hierarchical()) {
        auto &hierarchicalStorage =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        auto cache = std::make_shared<DecodedPageCache>(
            hierarchicalStorage.standaloneResidencyCoordinator
                ->availableResidencyBytes());
        auto scheduler = std::make_shared<TaskScheduler>(
            1,
            std::max<std::uint64_t>(
                1,
                std::min<std::uint64_t>(
                    TaskScheduler::defaultActiveByteBudget,
                    hierarchicalStorage.standaloneResidencyCoordinator
                        ->byteBudget())));
        setDocumentHierarchyResources(
            hierarchicalStorage.standaloneResidencyCoordinator,
            std::move(cache),
            std::move(scheduler),
            true);
        const std::scoped_lock lock(mutex_);
        std::get<Storage::HierarchicalSceneStorage>(storage_->value)
            .sharedDocumentResources = false;
    }
}

void PointCloudScene::setHierarchyResidencyActive(const bool active)
{
    if (!hierarchical()) {
        return;
    }
    HierarchyResidencyCoordinator::ParticipantPtr participant;
    {
        const std::scoped_lock lock(mutex_);
        participant =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value)
                .residencyParticipant;
    }
    participant->setActive(active);
    if (!active) {
        setPinnedNodes({});
        requestNodes({});
    }
}

void PointCloudScene::syncHierarchyResidencyBudget()
{
    if (!hierarchical()) {
        return;
    }
    HierarchyResidencyCoordinator::ParticipantPtr participant;
    DecodedPageCachePtr cache;
    bool shared = false;
    {
        const std::scoped_lock lock(mutex_);
        const auto &hierarchicalStorage =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        participant = hierarchicalStorage.residencyParticipant;
        cache = hierarchicalStorage.decodedPageCache;
        shared = hierarchicalStorage.sharedDocumentResources;
    }
    if (!shared && cache && participant) {
        cache->setByteBudget(participant->byteBudget());
    }
}

void PointCloudScene::trimDecodedCache(
    const std::span<const PointCloudNodeId> protectedNodes)
{
    DecodedPageCachePtr cache;
    PointCloudSourceId sourceId;
    std::uint64_t before = 0;
    {
        const std::scoped_lock lock(mutex_);
        if (const auto *hierarchical =
                std::get_if<Storage::HierarchicalSceneStorage>(
                    &storage_->value)) {
            cache = hierarchical->decodedPageCache;
            sourceId = hierarchical->sourceId;
        }
    }
    if (!cache) {
        return;
    }
    before = cache->residentPoints(sourceId);
    cache->trim(sourceId, protectedNodes);
    if (cache->residentPoints(sourceId) != before) {
        ++revision_;
        notifyInvalidated();
    }
}

std::uint64_t PointCloudScene::decodedByteBudget() const
{
    DecodedPageCachePtr cache;
    {
        const std::scoped_lock lock(mutex_);
        if (const auto *hierarchical =
                std::get_if<Storage::HierarchicalSceneStorage>(
                    &storage_->value)) {
            cache = hierarchical->decodedPageCache;
        }
    }
    return cache ? cache->byteBudget() : 0;
}

std::uint64_t PointCloudScene::decodedResidentBytes() const
{
    DecodedPageCachePtr cache;
    PointCloudSourceId sourceId;
    {
        const std::scoped_lock lock(mutex_);
        if (const auto *flat =
                std::get_if<Storage::FlatSceneStorage>(&storage_->value)) {
            return flat->residentBytes;
        }
        const auto &hierarchical =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        cache = hierarchical.decodedPageCache;
        sourceId = hierarchical.sourceId;
    }
    return cache ? cache->residentBytes(sourceId) : 0;
}

std::uint64_t PointCloudScene::decodedResidentPoints() const
{
    DecodedPageCachePtr cache;
    PointCloudSourceId sourceId;
    {
        const std::scoped_lock lock(mutex_);
        if (const auto *flat =
                std::get_if<Storage::FlatSceneStorage>(&storage_->value)) {
            return flat->totalPoints;
        }
        const auto &hierarchical =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        cache = hierarchical.decodedPageCache;
        sourceId = hierarchical.sourceId;
    }
    return cache ? cache->residentPoints(sourceId) : 0;
}

bool PointCloudScene::decodeInFlight() const
{
    const std::scoped_lock lock(mutex_);
    if (const auto *hierarchical =
            std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value)) {
        return !hierarchical->decodeRequests.empty();
    }
    return false;
}

std::string PointCloudScene::hierarchyError() const
{
    const std::scoped_lock lock(mutex_);
    if (const auto *hierarchical =
            std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value)) {
        return hierarchical->hierarchyError;
    }
    return {};
}

PointCloudSceneMetrics PointCloudScene::hierarchyMetrics() const
{
    PointCloudSceneMetrics result;
    PointCloudDataSourcePtr source;
    {
        const std::scoped_lock lock(mutex_);
        const auto *hierarchical =
            std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
        if (!hierarchical) {
            return result;
        }
        result.cache =
            hierarchical->decodedPageCache->metrics(hierarchical->sourceId);
        result.decodeRequestsQueued = hierarchical->decodeRequestsQueued;
        result.decodeRequestsStarted = hierarchical->decodeRequestsStarted;
        result.decodeRequestsCompleted = hierarchical->decodeRequestsCompleted;
        result.decodeRequestsCancelled = hierarchical->decodeRequestsCancelled;
        result.decodeRequestsFailed = hierarchical->decodeRequestsFailed;
        source = hierarchical->dataSource;
    }
    result.source = source->metrics();
    return result;
}

PointCloudStorageMetrics PointCloudScene::storageMetrics() const
{
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    return hierarchical ? hierarchical->dataSource->storageMetrics()
                        : PointCloudStorageMetrics{};
}

RasterPointColorMetrics PointCloudScene::rasterPointColorMetrics() const
{
    const std::scoped_lock lock(mutex_);
    RasterPointColorMetrics result;
    if (const auto *flat =
            std::get_if<Storage::FlatSceneStorage>(&storage_->value)) {
        if (flat->displacedColors) {
            result.flatDisplacedColorBytes =
                flat->displacedColors->capacity() * sizeof(std::uint32_t);
        }
        return result;
    }
    const auto &hierarchy =
        std::get<Storage::HierarchicalSceneStorage>(storage_->value);
    if (const auto decorated =
            std::dynamic_pointer_cast<RasterColorizedPointSource>(
                hierarchy.dataSource)) {
        result.activeColorTableBytes = decorated->byteSize();
    }
    if (hierarchy.sourceRootPayload) {
        result.retainedSourceRootBytes =
            pointCloudNodePayloadBytes(*hierarchy.sourceRootPayload);
        result.retainedColoredRootBytes =
            pointCloudNodePayloadBytes(*hierarchy.rootPayload);
    }
    return result;
}

bool PointCloudScene::hasRasterPointColors() const
{
    const std::scoped_lock lock(mutex_);
    if (const auto *flat =
            std::get_if<Storage::FlatSceneStorage>(&storage_->value)) {
        return flat->displacedColors != nullptr;
    }
    const auto &hierarchy =
        std::get<Storage::HierarchicalSceneStorage>(storage_->value);
    return hierarchy.baseDataSource != nullptr;
}

RasterPointColorizeAvailability
PointCloudScene::rasterPointColorizeAvailability() const
{
    const std::scoped_lock lock(mutex_);
    if (const auto *flat =
            std::get_if<Storage::FlatSceneStorage>(&storage_->value)) {
        static_cast<void>(flat);
        return loadingComplete_ ? RasterPointColorizeAvailability::Ready
                                : RasterPointColorizeAvailability::Loading;
    }
    const auto &hierarchy =
        std::get<Storage::HierarchicalSceneStorage>(storage_->value);
    const PointCloudDataSourcePtr &base = hierarchy.baseDataSource
                                              ? hierarchy.baseDataSource
                                              : hierarchy.dataSource;
    const PointCloudStorageMetrics metrics = base->storageMetrics();
    if (metrics.localPersistent && !metrics.committed) {
        return RasterPointColorizeAvailability::Loading;
    }
    return base->storedNodeIndex()
               ? RasterPointColorizeAvailability::Ready
               : RasterPointColorizeAvailability::Unsupported;
}

std::optional<RasterColorizeTargetSnapshot>
PointCloudScene::rasterPointColorizeTarget() const
{
    const std::scoped_lock lock(mutex_);
    const PointCloudScenePtr self =
        std::const_pointer_cast<PointCloudScene>(shared_from_this());
    if (const auto *flat =
            std::get_if<Storage::FlatSceneStorage>(&storage_->value)) {
        if (!loadingComplete_) {
            return std::nullopt;
        }
        return RasterColorizeTargetSnapshot{
            .scene = self,
            .data =
                RasterColorizeFlatTarget{
                    .blocks = flat->blocks,
                    .sourceColors = flat->displacedColors,
                },
        };
    }
    const auto &hierarchy =
        std::get<Storage::HierarchicalSceneStorage>(storage_->value);
    PointCloudDataSourcePtr base = hierarchy.baseDataSource
                                       ? hierarchy.baseDataSource
                                       : hierarchy.dataSource;
    const auto nodes = base->storedNodeIndex();
    if (!nodes) {
        return std::nullopt;
    }
    return RasterColorizeTargetSnapshot{
        .scene = self,
        .data =
            RasterColorizeHierarchicalTarget{
                .baseSource = std::move(base),
                .storedNodes = *nodes,
                .sourceRootPayload = hierarchy.sourceRootPayload
                                         ? hierarchy.sourceRootPayload
                                         : hierarchy.rootPayload,
                .expectedRootPayloadRevision = hierarchy.rootPayloadRevision,
            },
    };
}

RasterPointColorApplyOutcome
PointCloudScene::applyRasterPointColors(RasterColorizePreparedPtr prepared)
{
    if (!prepared || prepared->scene.get() != this) {
        throw std::invalid_argument(
            "Raster color transaction targets a different scene");
    }

    if (auto *hierarchyPrepared =
            std::get_if<RasterColorizePreparedHierarchy>(&prepared->data)) {
        if (!hierarchyPrepared->baseSource ||
            !hierarchyPrepared->colorizedSource ||
            !hierarchyPrepared->sourceRootPayload ||
            !hierarchyPrepared->coloredRootPayload ||
            hierarchyPrepared->sourceRootPayload->nodeId !=
                rootPointCloudNode ||
            hierarchyPrepared->coloredRootPayload->nodeId !=
                rootPointCloudNode) {
            throw std::invalid_argument(
                "Hierarchical raster color transaction is incomplete");
        }
        {
            const std::scoped_lock lock(mutex_);
            auto *hierarchy = std::get_if<Storage::HierarchicalSceneStorage>(
                &storage_->value);
            if (!hierarchy) {
                return RasterPointColorApplyOutcome::Stale;
            }
            const PointCloudDataSourcePtr &base =
                hierarchy->baseDataSource ? hierarchy->baseDataSource
                                          : hierarchy->dataSource;
            if (base != hierarchyPrepared->baseSource ||
                hierarchy->rootPayloadRevision !=
                    hierarchyPrepared->expectedRootPayloadRevision ||
                (hierarchy->sourceRootPayload ? hierarchy->sourceRootPayload
                                              : hierarchy->rootPayload) !=
                    hierarchyPrepared->sourceRootPayload) {
                return RasterPointColorApplyOutcome::Stale;
            }
        }

        cancelAndWaitForDecodes();
        DecodedPageCachePtr cache;
        PointCloudSourceId sourceId;
        HierarchyResidencyCoordinator::ParticipantPtr participant;
        std::vector<PointCloudNodeId> pins;
        {
            const std::scoped_lock lock(mutex_);
            auto &hierarchy =
                std::get<Storage::HierarchicalSceneStorage>(storage_->value);
            const PointCloudDataSourcePtr &base = hierarchy.baseDataSource
                                                      ? hierarchy.baseDataSource
                                                      : hierarchy.dataSource;
            if (base != hierarchyPrepared->baseSource ||
                hierarchy.rootPayloadRevision !=
                    hierarchyPrepared->expectedRootPayloadRevision) {
                hierarchy.suspendPinnedResubmission = false;
                return RasterPointColorApplyOutcome::Stale;
            }
            cache = hierarchy.decodedPageCache;
            sourceId = hierarchy.sourceId;
            participant = hierarchy.residencyParticipant;
            pins.reserve(hierarchy.pinnedNodes.size() + 1);
            pins.push_back(rootPointCloudNode);
            pins.insert(pins.end(),
                        hierarchy.pinnedNodes.begin(),
                        hierarchy.pinnedNodes.end());
        }
        cache->replaceSourceRoot(
            sourceId, hierarchyPrepared->coloredRootPayload, pins);

        PointMemoryBudget::ReservationPtr staging;
        std::uint64_t retainedBytes = 0;
        {
            const std::scoped_lock lock(mutex_);
            auto &hierarchy =
                std::get<Storage::HierarchicalSceneStorage>(storage_->value);
            if (!hierarchy.baseDataSource) {
                hierarchy.baseDataSource = hierarchyPrepared->baseSource;
                hierarchy.sourceRootPayload =
                    hierarchyPrepared->sourceRootPayload;
            }
            hierarchy.dataSource =
                std::move(hierarchyPrepared->colorizedSource);
            hierarchy.rootPayload =
                std::move(hierarchyPrepared->coloredRootPayload);
            staging = std::move(hierarchyPrepared->rootStagingReservation);
            retainedBytes =
                pointCloudNodePayloadBytes(*hierarchy.sourceRootPayload) +
                pointCloudNodePayloadBytes(*hierarchy.rootPayload);
            ++hierarchy.rootPayloadRevision;
            hierarchy.suspendPinnedResubmission = false;
            ++revision_;
        }
        participant->setRetainedRootBytes(retainedBytes);
        staging.reset();
        syncHierarchyResidencyBudget();
        requestNodes({});
        notifyInvalidated();
        return RasterPointColorApplyOutcome::Applied;
    }

    auto *flatPrepared =
        std::get_if<RasterColorizePreparedFlat>(&prepared->data);
    if (!flatPrepared || flatPrepared->expectedBlocks.size() !=
                             flatPrepared->replacementBlocks.size()) {
        throw std::invalid_argument(
            "Flat raster color transaction is incomplete");
    }
    PointMemoryBudget::ReservationPtr staging;
    {
        const std::scoped_lock lock(mutex_);
        auto *flat = std::get_if<Storage::FlatSceneStorage>(&storage_->value);
        if (!flat ||
            flat->blocks.size() != flatPrepared->expectedBlocks.size()) {
            return RasterPointColorApplyOutcome::Stale;
        }
        const bool firstBake = !flat->displacedColors;
        if (firstBake !=
            static_cast<bool>(flatPrepared->displacedSourceColors)) {
            return RasterPointColorApplyOutcome::Stale;
        }
        for (std::size_t index = 0; index < flat->blocks.size(); ++index) {
            const SceneBlock &live = flat->blocks[index];
            const RasterColorizeFlatBlockIdentity &expected =
                flatPrepared->expectedBlocks[index];
            const auto captured = expected.block.lock();
            if (!captured || captured != live.block || expected.id != live.id ||
                flatPrepared->replacementBlocks[index].id != live.id ||
                expected.pointCount != live.block->points.size() ||
                expected.attributeCount != live.block->attributes.size()) {
                return RasterPointColorApplyOutcome::Stale;
            }
        }
        flat->blocks.swap(flatPrepared->replacementBlocks);
        if (firstBake) {
            flat->displacedColors =
                std::move(flatPrepared->displacedSourceColors);
            flat->colorReservation = std::move(flatPrepared->colorReservation);
        }
        staging = std::move(flatPrepared->stagingReservation);
        ++revision_;
    }
    // The swap left the old live blocks in the prepared vector. Destroy them
    // while the staging reservation still accounts for the replacement set.
    flatPrepared->replacementBlocks.clear();
    staging.reset();
    notifyInvalidated();
    return RasterPointColorApplyOutcome::Applied;
}

RasterPointColorApplyOutcome PointCloudScene::revertPointColors()
{
    if (hierarchical()) {
        PointCloudDataSourcePtr base;
        PointCloudNodePayloadPtr sourceRoot;
        DecodedPageCachePtr cache;
        HierarchyResidencyCoordinator::ParticipantPtr participant;
        PointCloudSourceId sourceId;
        std::vector<PointCloudNodeId> pins;
        {
            const std::scoped_lock lock(mutex_);
            auto &hierarchy =
                std::get<Storage::HierarchicalSceneStorage>(storage_->value);
            if (!hierarchy.baseDataSource || !hierarchy.sourceRootPayload) {
                return RasterPointColorApplyOutcome::Stale;
            }
        }
        cancelAndWaitForDecodes();
        {
            const std::scoped_lock lock(mutex_);
            auto &hierarchy =
                std::get<Storage::HierarchicalSceneStorage>(storage_->value);
            if (!hierarchy.baseDataSource || !hierarchy.sourceRootPayload) {
                hierarchy.suspendPinnedResubmission = false;
                return RasterPointColorApplyOutcome::Stale;
            }
            base = hierarchy.baseDataSource;
            sourceRoot = hierarchy.sourceRootPayload;
            cache = hierarchy.decodedPageCache;
            participant = hierarchy.residencyParticipant;
            sourceId = hierarchy.sourceId;
            pins.push_back(rootPointCloudNode);
            pins.insert(pins.end(),
                        hierarchy.pinnedNodes.begin(),
                        hierarchy.pinnedNodes.end());
        }
        cache->replaceSourceRoot(sourceId, sourceRoot, pins);
        std::uint64_t retainedBytes = 0;
        {
            const std::scoped_lock lock(mutex_);
            auto &hierarchy =
                std::get<Storage::HierarchicalSceneStorage>(storage_->value);
            hierarchy.dataSource = std::move(base);
            hierarchy.rootPayload = std::move(sourceRoot);
            hierarchy.baseDataSource.reset();
            hierarchy.sourceRootPayload.reset();
            ++hierarchy.rootPayloadRevision;
            hierarchy.suspendPinnedResubmission = false;
            retainedBytes = pointCloudNodePayloadBytes(*hierarchy.rootPayload);
            ++revision_;
        }
        participant->setRetainedRootBytes(retainedBytes);
        syncHierarchyResidencyBudget();
        requestNodes({});
        notifyInvalidated();
        return RasterPointColorApplyOutcome::Applied;
    }

    std::vector<SceneBlock> replacements;
    std::shared_ptr<std::vector<std::uint32_t>> displaced;
    PointMemoryBudget::ReservationPtr staging;
    {
        const std::scoped_lock lock(mutex_);
        auto &flat = std::get<Storage::FlatSceneStorage>(storage_->value);
        if (!flat.displacedColors) {
            return RasterPointColorApplyOutcome::Stale;
        }
        if (!flat.colorReservation) {
            throw std::logic_error(
                "Retained source colors have no point-memory reservation");
        }
        const auto reserved =
            flat.colorReservation->tryReserveSibling(flat.residentBytes);
        if (!reserved) {
            return RasterPointColorApplyOutcome::Stale;
        }
        staging = *reserved;
        displaced = flat.displacedColors;
        replacements.reserve(flat.blocks.size());
        std::uint64_t offset = 0;
        for (const SceneBlock &entry : flat.blocks) {
            auto block = std::make_shared<PointBlock>(*entry.block);
            if (offset > displaced->size() ||
                block->points.size() > displaced->size() - offset) {
                throw std::logic_error(
                    "Retained source colors do not match flat blocks");
            }
            for (GpuPoint &point : block->points) {
                point.rgba = (*displaced)[offset++];
            }
            replacements.push_back({.id = entry.id, .block = std::move(block)});
        }
        if (offset != displaced->size()) {
            throw std::logic_error(
                "Retained source colors contain trailing entries");
        }
    }
    {
        const std::scoped_lock lock(mutex_);
        auto &flat = std::get<Storage::FlatSceneStorage>(storage_->value);
        if (flat.displacedColors != displaced ||
            flat.blocks.size() != replacements.size()) {
            return RasterPointColorApplyOutcome::Stale;
        }
        flat.blocks.swap(replacements);
        flat.displacedColors.reset();
        flat.colorReservation.reset();
        ++revision_;
    }
    // `replacements` owns the displaced colored blocks after the swap. Keep
    // their transient allowance until those payloads have been destroyed.
    replacements.clear();
    staging.reset();
    notifyInvalidated();
    return RasterPointColorApplyOutcome::Applied;
}

void PointCloudScene::submitDecodeLocked(const PointCloudNodeId id)
{
    auto &hierarchical =
        std::get<Storage::HierarchicalSceneStorage>(storage_->value);
    if (!hierarchical.decodeScheduler || !hierarchical.decodedPageCache ||
        hierarchical.decodedPageCache->contains(hierarchical.sourceId, id) ||
        hierarchical.failedNodes.contains(id) ||
        hierarchical.decodeRequests.contains(id)) {
        return;
    }
    const std::uint64_t estimated =
        std::min(estimatedDecodeAllowance(
                     hierarchical.dataSource->node(id).estimatedPointCount),
                 hierarchical.decodeScheduler->activeByteBudget());
    auto request = std::make_shared<HierarchyDecodeRequest>();
    ++hierarchical.outstandingDecodeCallbacks;
    try {
        request->taskId = hierarchical.decodeScheduler->submit(
            TaskPriority::VisibleDetail,
            estimated,
            [this, id, request] {
                executeDecode(id, request);
            },
            [this, id, request] {
                finishQueuedCancellation(id, request);
            },
            hierarchical.sourceId.value());
        hierarchical.decodeRequests.emplace(id, request);
        ++hierarchical.decodeRequestsQueued;
    } catch (...) {
        --hierarchical.outstandingDecodeCallbacks;
        throw;
    }
}

void PointCloudScene::executeDecode(
    const PointCloudNodeId id,
    const std::shared_ptr<HierarchyDecodeRequest> &request) noexcept
{
    PointCloudDataSourcePtr source;
    DecodedPageCachePtr cache;
    PointCloudSourceId sourceId;
    bool requestIsCurrent = false;
    {
        const std::scoped_lock lock(mutex_);
        auto &hierarchical =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        const auto found = hierarchical.decodeRequests.find(id);
        requestIsCurrent = found != hierarchical.decodeRequests.end() &&
                           found->second == request;
        if (requestIsCurrent) {
            source = hierarchical.dataSource;
            cache = hierarchical.decodedPageCache;
            sourceId = hierarchical.sourceId;
            ++hierarchical.decodeRequestsStarted;
        }
    }
    if (!requestIsCurrent) {
        finishQueuedCancellation(id, request);
        return;
    }

    PointCloudNodePayloadPtr payload;
    std::string error;
    bool cancelled = request->stop.stop_requested();
    try {
        if (!cancelled) {
            if (!request->stop.stop_requested()) {
                payload = source->loadNode(id, request->stop.get_token());
            } else {
                cancelled = true;
            }
        }
        if (payload && !request->stop.stop_requested()) {
            const std::array protectedNodes{id};
            cache->insert(sourceId, payload, protectedNodes);
        }
    } catch (const PointCloudDataSourceCancelled &) {
        cancelled = true;
    } catch (const std::exception &exception) {
        error = exception.what();
    } catch (...) {
        error = "unknown hierarchy decode failure";
    }

    if (request->finished.exchange(true)) {
        return;
    }
    {
        const std::scoped_lock lock(mutex_);
        auto &hierarchical =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        const auto found = hierarchical.decodeRequests.find(id);
        bool removedCurrentRequest = false;
        if (found != hierarchical.decodeRequests.end() &&
            found->second == request) {
            hierarchical.decodeRequests.erase(found);
            removedCurrentRequest = true;
        }
        if (payload && !request->stop.stop_requested() && error.empty()) {
            updateStatistics(*payload);
            ++hierarchical.decodeRequestsCompleted;
            ++revision_;
        } else if (!error.empty()) {
            hierarchical.failedNodes.insert(id);
            hierarchical.hierarchyError = std::move(error);
            ++hierarchical.decodeRequestsFailed;
            ++revision_;
        } else if (cancelled || request->stop.stop_requested()) {
            ++hierarchical.decodeRequestsCancelled;
        }
        if (hierarchical.outstandingDecodeCallbacks > 0) {
            --hierarchical.outstandingDecodeCallbacks;
        }
        if (removedCurrentRequest &&
            (cancelled || request->stop.stop_requested()) &&
            !hierarchical.suspendPinnedResubmission &&
            hierarchical.pinnedNodes.contains(id)) {
            try {
                submitDecodeLocked(id);
            } catch (const std::exception &exception) {
                hierarchical.failedNodes.insert(id);
                hierarchical.hierarchyError = exception.what();
                ++hierarchical.decodeRequestsFailed;
                ++revision_;
            } catch (...) {
                hierarchical.failedNodes.insert(id);
                hierarchical.hierarchyError =
                    "could not resubmit a newly pinned hierarchy page";
                ++hierarchical.decodeRequestsFailed;
                ++revision_;
            }
        }
    }
    decodeFinished_.notify_all();
    notifyInvalidated();
}

void PointCloudScene::finishQueuedCancellation(
    const PointCloudNodeId id,
    const std::shared_ptr<HierarchyDecodeRequest> &request) noexcept
{
    if (request->finished.exchange(true)) {
        return;
    }
    {
        const std::scoped_lock lock(mutex_);
        auto &hierarchical =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        const auto found = hierarchical.decodeRequests.find(id);
        bool removedCurrentRequest = false;
        if (found != hierarchical.decodeRequests.end() &&
            found->second == request) {
            hierarchical.decodeRequests.erase(found);
            removedCurrentRequest = true;
        }
        ++hierarchical.decodeRequestsCancelled;
        if (hierarchical.outstandingDecodeCallbacks > 0) {
            --hierarchical.outstandingDecodeCallbacks;
        }
        if (removedCurrentRequest && !hierarchical.suspendPinnedResubmission &&
            hierarchical.pinnedNodes.contains(id)) {
            try {
                submitDecodeLocked(id);
            } catch (const std::exception &exception) {
                hierarchical.failedNodes.insert(id);
                hierarchical.hierarchyError = exception.what();
                ++hierarchical.decodeRequestsFailed;
                ++revision_;
            } catch (...) {
                hierarchical.failedNodes.insert(id);
                hierarchical.hierarchyError =
                    "could not resubmit a newly pinned hierarchy page";
                ++hierarchical.decodeRequestsFailed;
                ++revision_;
            }
        }
    }
    decodeFinished_.notify_all();
    notifyInvalidated();
}

void PointCloudScene::cancelAndWaitForDecodes()
{
    if (!storage_ || !hierarchical()) {
        return;
    }
    std::vector<
        std::pair<std::shared_ptr<TaskScheduler>, TaskScheduler::TaskId>>
        cancellations;
    {
        const std::scoped_lock lock(mutex_);
        auto &hierarchicalStorage =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        hierarchicalStorage.suspendPinnedResubmission = true;
        cancellations.reserve(hierarchicalStorage.decodeRequests.size());
        for (const auto &[id, request] : hierarchicalStorage.decodeRequests) {
            static_cast<void>(id);
            request->stop.request_stop();
            cancellations.emplace_back(hierarchicalStorage.decodeScheduler,
                                       request->taskId);
        }
    }
    for (const auto &[scheduler, taskId] : cancellations) {
        if (scheduler) {
            static_cast<void>(scheduler->cancel(taskId));
        }
    }
    std::unique_lock lock(mutex_);
    decodeFinished_.wait(lock, [this] {
        return std::get<Storage::HierarchicalSceneStorage>(storage_->value)
                   .outstandingDecodeCallbacks == 0;
    });
    std::get<Storage::HierarchicalSceneStorage>(storage_->value)
        .suspendPinnedResubmission = false;
}

void PointCloudScene::updateCachePins()
{
    DecodedPageCachePtr cache;
    PointCloudSourceId sourceId;
    std::vector<PointCloudNodeId> pins{rootPointCloudNode};
    {
        const std::scoped_lock lock(mutex_);
        const auto &hierarchical =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        cache = hierarchical.decodedPageCache;
        sourceId = hierarchical.sourceId;
        pins.insert(pins.end(),
                    hierarchical.pinnedNodes.begin(),
                    hierarchical.pinnedNodes.end());
    }
    if (cache) {
        cache->setPinned(sourceId, pins);
    }
}

void PointCloudScene::updateStatistics(const PointCloudNodePayload &payload)
{
    for (const PointBlockPtr &block : payload.blocks) {
        if (block) {
            if (!haveIntensityStatistics_) {
                intensityMinimum_ = block->intensityMinimum;
                intensityMaximum_ = block->intensityMaximum;
                haveIntensityStatistics_ = true;
            } else {
                intensityMinimum_ =
                    std::min(intensityMinimum_, block->intensityMinimum);
                intensityMaximum_ =
                    std::max(intensityMaximum_, block->intensityMaximum);
            }
            updateClassifications(*block);
        }
    }
}

void PointCloudScene::updateClassifications(const PointBlock &block)
{
    for (const GpuPoint &point : block.points) {
        presentClassifications_.setVisible(
            static_cast<std::uint8_t>(point.attributes & 0xffU), true);
    }
}

PointCloudScalarRanges PointCloudScene::completeScalarRangesLocked() const
{
    if (const auto *hierarchical =
            std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value)) {
        return hierarchical->dataSource->scalarRanges();
    }
    const auto &flat = std::get<Storage::FlatSceneStorage>(storage_->value);
    PointCloudScalarRanges result;
    const bool completeSourceRetained =
        metadata_.sourcePointCount == 0 ||
        flat.totalPoints == metadata_.sourcePointCount;
    if (loadingComplete_ && completeSourceRetained && metadata_.hasIntensity &&
        haveIntensityStatistics_) {
        result.intensity = PointScalarRange{
            static_cast<double>(intensityMinimum_),
            static_cast<double>(intensityMaximum_),
        };
    }
    return result;
}

} // namespace pci
