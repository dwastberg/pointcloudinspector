#include <pci/runtime/point/PointDatasetRuntime.h>

#include <pci/foundation/CheckedArithmetic.h>
#include <pci/pointcloud/PointSample.h>
#include <pci/runtime/point/RasterColorizedPointSource.h>

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

struct PointDatasetRuntimeInvalidationState {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::function<void()>> callbacks;
    std::uint64_t nextId = 1;
};

struct PointDatasetRuntime::HierarchyDecodeRequest {
    TaskScheduler::TaskId taskId;
    std::stop_source stop;
    std::atomic_bool finished = false;
    std::shared_ptr<PointDecodeQueue::Slot> slot;
    PointCloudNodePayloadPtr payload;
    std::string error;
    std::uint64_t contentRevision = 0;
};

struct PointDatasetRuntime::Storage {
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
        PointMemoryBudget::ReservationPtr rootReservation;
        PointMemoryBudget::ReservationPtr coloredRootReservation;
        PointCloudNodePayloadPtr rootPayload;
        PointCloudDataSourcePtr baseDataSource;
        PointCloudNodePayloadPtr sourceRootPayload;
        std::uint64_t rootPayloadRevision = 0;
        PointCloudSourceId sourceId;
        HierarchyResidencyCoordinatorPtr standaloneResidencyCoordinator;
        HierarchyResidencyCoordinator::ParticipantPtr residencyParticipant;
        DecodedPageCachePtr decodedPageCache;
        std::shared_ptr<TaskScheduler> decodeScheduler;
        PointDecodeQueuePtr completionQueue;
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

class ScenePointResidencyView final : public PointResidencyView {
public:
    ScenePointResidencyView(const PointCloudSourceId sourceId,
                            const std::uint64_t contentRevision,
                            std::shared_ptr<const PointCloudDataSource> source,
                            std::shared_ptr<const DecodedPageCache> cache,
                            std::string error)
        : sourceId_(sourceId)
        , contentRevision_(contentRevision)
        , source_(std::move(source))
        , cache_(std::move(cache))
        , error_(std::move(error))
    {
    }

    [[nodiscard]] PointCloudSourceId sourceId() const noexcept override
    {
        return sourceId_;
    }

    [[nodiscard]] std::uint64_t contentRevision() const noexcept override
    {
        return contentRevision_;
    }

    [[nodiscard]] PointCloudNode node(const PointCloudNodeId id) const override
    {
        return source_->node(id);
    }

    [[nodiscard]] PointCloudNodePayloadPtr
    acquire(const PointCloudNodeId id) const override
    {
        return cache_->peek(sourceId_, id);
    }

    [[nodiscard]] std::string_view error() const noexcept override
    {
        return error_;
    }

private:
    PointCloudSourceId sourceId_;
    std::uint64_t contentRevision_ = 0;
    std::shared_ptr<const PointCloudDataSource> source_;
    std::shared_ptr<const DecodedPageCache> cache_;
    std::string error_;
};

std::uint64_t estimatedDecodeAllowance(const std::uint64_t pointCount) noexcept
{
    constexpr std::uint64_t bytesPerPoint =
        sizeof(PointSample) + sizeof(GpuPoint) + sizeof(PointAttributes);
    return saturatingMultiply(pointCount, bytesPerPoint);
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

} // namespace

PointDatasetRuntimeInvalidationSubscription::
    PointDatasetRuntimeInvalidationSubscription(
        std::weak_ptr<PointDatasetRuntimeInvalidationState> state,
        const std::uint64_t id) noexcept
    : state_(std::move(state))
    , id_(id)
{
}

PointDatasetRuntimeInvalidationSubscription::
    ~PointDatasetRuntimeInvalidationSubscription()
{
    reset();
}

PointDatasetRuntimeInvalidationSubscription::
    PointDatasetRuntimeInvalidationSubscription(
        PointDatasetRuntimeInvalidationSubscription &&other) noexcept
    : state_(std::move(other.state_))
    , id_(std::exchange(other.id_, 0))
{
}

PointDatasetRuntimeInvalidationSubscription &
PointDatasetRuntimeInvalidationSubscription::operator=(
    PointDatasetRuntimeInvalidationSubscription &&other) noexcept
{
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

void PointDatasetRuntimeInvalidationSubscription::reset() noexcept
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

PointDatasetRuntimeInvalidationSubscription::operator bool() const noexcept
{
    return id_ != 0 && !state_.expired();
}

PointDatasetRuntime::PointDatasetRuntime(PointCloudMetadata metadata,
                                         PointCloudSourceId sourceId)
    : sourceId_(sourceId.value() ? sourceId : allocatePointCloudSourceId())
    , metadata_(std::move(metadata))
    , storage_(std::make_unique<Storage>(Storage::FlatSceneStorage{}))
    , invalidationState_(
          std::make_shared<PointDatasetRuntimeInvalidationState>())
{
}

PointDatasetRuntime::PointDatasetRuntime(
    PointCloudMetadata metadata,
    PointCloudDataSourcePtr dataSource,
    PointCloudNodePayloadPtr rootPayload,
    const std::uint64_t decodedByteBudget,
    const bool loadingComplete,
    PointCloudSourceId sourceId,
    PointMemoryBudget::ReservationPtr rootReservation)
    : sourceId_(sourceId.value() ? sourceId : allocatePointCloudSourceId())
    , metadata_(std::move(metadata))
    , invalidationState_(
          std::make_shared<PointDatasetRuntimeInvalidationState>())
{
    if (!dataSource || !rootPayload ||
        rootPayload->nodeId != rootPointCloudNode) {
        throw std::invalid_argument(
            "hierarchical scene requires a source and root payload");
    }
    Storage::HierarchicalSceneStorage hierarchical;
    hierarchical.dataSource = std::move(dataSource);
    hierarchical.rootReservation = std::move(rootReservation);
    hierarchical.rootPayload = std::move(rootPayload);
    hierarchical.sourceId = sourceId_;
    hierarchical.standaloneResidencyCoordinator =
        std::make_shared<HierarchyResidencyCoordinator>(
            decodedByteBudget,
            1,
            HierarchyDecodeAdmissionPtr{},
            hierarchical.rootReservation
                ? hierarchical.rootReservation->budget()
                : PointMemoryBudgetPtr{});
    hierarchical.residencyParticipant =
        hierarchical.standaloneResidencyCoordinator->registerParticipant(
            pointCloudNodePayloadBytes(*hierarchical.rootPayload),
            true,
            hierarchical.rootReservation
                ? pointCloudNodePayloadBytes(*hierarchical.rootPayload)
                : 0);
    hierarchical.decodedPageCache = std::make_shared<DecodedPageCache>(
        hierarchical.residencyParticipant->byteBudget());
    hierarchical.decodeScheduler = std::make_shared<TaskScheduler>(
        1,
        std::max<std::uint64_t>(
            1,
            std::min<std::uint64_t>(TaskScheduler::defaultActiveByteBudget,
                                    decodedByteBudget)));
    hierarchical.completionQueue = std::make_shared<PointDecodeQueue>(
        hierarchical.rootReservation
            ? hierarchical.rootReservation->budget()
            : std::make_shared<PointMemoryBudget>(decodedByteBudget));
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

PointDatasetRuntime::~PointDatasetRuntime()
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

const PointCloudMetadata &PointDatasetRuntime::metadata() const noexcept
{
    return metadata_;
}

PointCloudSourceId PointDatasetRuntime::sourceId() const noexcept
{
    return sourceId_;
}

PointDatasetDescriptor PointDatasetRuntime::descriptor() const
{
    return {.sourceId = sourceId_, .metadata = metadata_};
}

PointDatasetView PointDatasetRuntime::datasetView() const
{
    const std::scoped_lock lock(mutex_);
    const auto *flat = std::get_if<Storage::FlatSceneStorage>(&storage_->value);
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    PointColorizeAvailability colorizeAvailability =
        PointColorizeAvailability::Unsupported;
    switch (rasterPointColorizeAvailabilityLocked()) {
    case RasterPointColorizeAvailability::Ready:
        colorizeAvailability = PointColorizeAvailability::Ready;
        break;
    case RasterPointColorizeAvailability::Loading:
        colorizeAvailability = PointColorizeAvailability::Loading;
        break;
    case RasterPointColorizeAvailability::Unsupported:
        break;
    }
    return {
        .descriptor = {.sourceId = sourceId_, .metadata = metadata_},
        .availability =
            {
                .bounds = !flat || flat->blocks.empty() ? metadata_.sourceBounds
                                                        : flat->blockBounds,
                .pointCount =
                    flat ? flat->totalPoints : metadata_.sourcePointCount,
                .colorizeAvailability = colorizeAvailability,
                .scalarRanges = completeScalarRangesLocked(),
                .presentClassifications = presentClassifications_,
                .storage = hierarchical
                               ? hierarchical->dataSource->storageMetrics()
                               : PointCloudStorageMetrics{},
            },
    };
}

void PointDatasetRuntime::addBlock(PointBlockPtr block)
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
        // Reserve before updating any counters or statistics. Allocation
        // failure must leave the runtime unchanged.
        if (flat->blocks.size() == flat->blocks.capacity()) {
            flat->blocks.reserve(std::max<std::size_t>(
                1, saturatingMultiply<std::size_t>(flat->blocks.size(), 2)));
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

std::unique_ptr<PointDatasetPublication>
PointDatasetRuntime::preparePublication(PointBlockPtr block,
                                        const bool complete) const
{
    auto publication = std::make_unique<PointDatasetPublication>();
    publication->complete_ = complete;
    if (hierarchical()) {
        if (block) {
            throw std::invalid_argument(
                "hierarchies cannot ingest flat blocks");
        }
        publication->view_ = datasetView();
        return publication;
    }
    auto candidate =
        std::make_shared<PointDatasetRuntime>(metadata_, sourceId_);
    {
        const std::scoped_lock lock(mutex_);
        candidate->storage_ = std::make_unique<Storage>(
            std::get<Storage::FlatSceneStorage>(storage_->value));
        candidate->intensityMinimum_ = intensityMinimum_;
        candidate->intensityMaximum_ = intensityMaximum_;
        candidate->haveIntensityStatistics_ = haveIntensityStatistics_;
        candidate->presentClassifications_ = presentClassifications_;
        candidate->loadingComplete_ = loadingComplete_;
        publication->expectedRevision_ = revision_.load();
        candidate->revision_ = publication->expectedRevision_;
    }
    if (block) {
        candidate->addBlock(std::move(block));
    }
    if (complete) {
        candidate->markLoadingComplete();
    }
    publication->view_ = candidate->datasetView();
    publication->candidate_ = std::move(candidate);
    return publication;
}

bool PointDatasetRuntime::commitPublication(
    PointDatasetPublication &publication) noexcept
{
    const std::scoped_lock lock(mutex_);
    if (publication.committed_ ||
        publication.view_.descriptor.sourceId != sourceId_) {
        return false;
    }
    if (publication.candidate_) {
        if (revision_.load() != publication.expectedRevision_) {
            return false;
        }
        auto &candidate = *publication.candidate_;
        storage_.swap(candidate.storage_);
        std::swap(intensityMinimum_, candidate.intensityMinimum_);
        std::swap(intensityMaximum_, candidate.intensityMaximum_);
        std::swap(haveIntensityStatistics_, candidate.haveIntensityStatistics_);
        std::swap(presentClassifications_, candidate.presentClassifications_);
        std::swap(loadingComplete_, candidate.loadingComplete_);
        revision_.store(candidate.revision_.load());
    } else if (publication.complete_ && !loadingComplete_) {
        loadingComplete_ = true;
        ++revision_;
    }
    publication.committed_ = true;
    return true;
}

void PointDatasetRuntime::publishInvalidation() noexcept
{
    notifyInvalidated();
}

void PointDatasetRuntime::setResidentMemoryReservation(
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

std::vector<PointBlockPtr> PointDatasetRuntime::blocks() const
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

std::vector<SceneBlock> PointDatasetRuntime::blockEntries() const
{
    const std::scoped_lock lock(mutex_);
    if (const auto *flat =
            std::get_if<Storage::FlatSceneStorage>(&storage_->value)) {
        return flat->blocks;
    }
    return {};
}

std::uint64_t PointDatasetRuntime::totalPointCount() const
{
    const std::scoped_lock lock(mutex_);
    if (const auto *flat =
            std::get_if<Storage::FlatSceneStorage>(&storage_->value)) {
        return flat->totalPoints;
    }
    return metadata_.sourcePointCount;
}

std::uint64_t PointDatasetRuntime::revision() const
{
    return revision_.load(std::memory_order_acquire);
}

Bounds3d PointDatasetRuntime::bounds() const
{
    const std::scoped_lock lock(mutex_);
    const auto *flat = std::get_if<Storage::FlatSceneStorage>(&storage_->value);
    return !flat || flat->blocks.empty() ? metadata_.sourceBounds
                                         : flat->blockBounds;
}

PointCloudScalarRanges PointDatasetRuntime::scalarRanges() const
{
    const std::scoped_lock lock(mutex_);
    return completeScalarRangesLocked();
}

PointClassificationFilter PointDatasetRuntime::presentClassifications() const
{
    const std::scoped_lock lock(mutex_);
    return presentClassifications_;
}

std::uint16_t PointDatasetRuntime::intensityMinimum() const
{
    const std::scoped_lock lock(mutex_);
    return intensityMinimum_;
}

std::uint16_t PointDatasetRuntime::intensityMaximum() const
{
    const std::scoped_lock lock(mutex_);
    return intensityMaximum_;
}

PointDatasetRuntimeSnapshot PointDatasetRuntime::snapshot() const
{
    const std::scoped_lock lock(mutex_);
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    const auto *flat = std::get_if<Storage::FlatSceneStorage>(&storage_->value);
    PointDatasetRuntimeSnapshot result{
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

void PointDatasetRuntime::markLoadingComplete()
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

PointDatasetRuntimeInvalidationSubscription
PointDatasetRuntime::subscribeInvalidation(std::function<void()> callback)
{
    if (!callback) {
        return {};
    }
    const auto state = invalidationState_;
    const std::scoped_lock lock(state->mutex);
    const std::uint64_t id = state->nextId++;
    state->callbacks.emplace(id, std::move(callback));
    return PointDatasetRuntimeInvalidationSubscription(state, id);
}

void PointDatasetRuntime::notifyInvalidated() noexcept
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

bool PointDatasetRuntime::hierarchical() const noexcept
{
    return std::holds_alternative<Storage::HierarchicalSceneStorage>(
        storage_->value);
}

bool PointDatasetRuntime::detailLimited() const noexcept
{
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    return hierarchical && hierarchical->dataSource->detailLimited();
}

PointCloudNode PointDatasetRuntime::rootNode() const
{
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    if (!hierarchical) {
        throw std::logic_error("scene has no hierarchy");
    }
    return hierarchical->dataSource->rootNode();
}

PointCloudNode PointDatasetRuntime::node(const PointCloudNodeId id) const
{
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    if (!hierarchical) {
        throw std::logic_error("scene has no hierarchy");
    }
    return hierarchical->dataSource->node(id);
}

PointCloudNodePayloadPtr
PointDatasetRuntime::nodePayload(const PointCloudNodeId id)
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
PointDatasetRuntime::peekNodePayload(const PointCloudNodeId id) const
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

PointResidencyViewPtr PointDatasetRuntime::residencyView() const
{
    const std::scoped_lock lock(mutex_);
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    if (!hierarchical) {
        return {};
    }
    return std::make_shared<const ScenePointResidencyView>(
        hierarchical->sourceId,
        hierarchical->rootPayloadRevision,
        hierarchical->dataSource,
        hierarchical->decodedPageCache,
        hierarchical->hierarchyError);
}

std::uint64_t PointDatasetRuntime::residencyContentRevision() const noexcept
{
    const std::scoped_lock lock(mutex_);
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    return hierarchical ? hierarchical->rootPayloadRevision : 0;
}

void PointDatasetRuntime::applyDecodedLookupEffect(const PointCloudNodeId id,
                                                   const bool resident)
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
    if (cache) {
        cache->applyLookupEffect(sourceId, id, resident);
    }
}

std::optional<PointCloudFullDetailInfo>
PointDatasetRuntime::fullDetailInfo() const
{
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    return hierarchical ? hierarchical->dataSource->fullDetailInfo()
                        : std::nullopt;
}

std::uint64_t PointDatasetRuntime::reservedRootBytes() const
{
    const std::scoped_lock lock(mutex_);
    const auto *hierarchy =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    if (!hierarchy) {
        return 0;
    }
    const auto original = hierarchy->rootReservation
                              ? std::min(hierarchy->rootReservation->bytes(),
                                         pointCloudNodePayloadBytes(
                                             hierarchy->sourceRootPayload
                                                 ? *hierarchy->sourceRootPayload
                                                 : *hierarchy->rootPayload))
                              : 0;
    const auto colored =
        hierarchy->coloredRootReservation
            ? std::min(hierarchy->coloredRootReservation->bytes(),
                       pointCloudNodePayloadBytes(*hierarchy->rootPayload))
            : 0;
    return saturatingAdd(original, colored);
}

std::uint64_t PointDatasetRuntime::minimumRootPayloadBytes() const
{
    if (!hierarchical()) {
        return 0;
    }
    const std::scoped_lock lock(mutex_);
    const auto &storage =
        std::get<Storage::HierarchicalSceneStorage>(storage_->value);
    // Published reserved roots can have surviving block/upload owners. A
    // copied reduction would allocate new points without freeing that charge.
    // Preserve these roots until a replacement has its own admission.
    if (storage.rootReservation) {
        return saturatingAdd(
            pointCloudNodePayloadBytes(*storage.rootPayload),
            storage.sourceRootPayload
                ? pointCloudNodePayloadBytes(*storage.sourceRootPayload)
                : 0);
    }
    const std::uint64_t active = maximumRootBytesPerPoint(*storage.rootPayload);
    return storage.sourceRootPayload
               ? saturatingAdd(
                     active,
                     maximumRootBytesPerPoint(*storage.sourceRootPayload))
               : active;
}

std::uint64_t
PointDatasetRuntime::limitRootPayloadBytes(const std::uint64_t maximumBytes)
{
    if (!hierarchical()) {
        return 0;
    }
    PointCloudNodePayloadPtr currentRoot;
    PointCloudNodePayloadPtr currentSourceRoot;
    PointMemoryBudget::ReservationPtr coloredReservation;
    PointMemoryBudget::ReservationPtr originalReservation;
    {
        const std::scoped_lock lock(mutex_);
        const auto &storage =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        currentRoot = storage.rootPayload;
        currentSourceRoot = storage.sourceRootPayload;
        coloredReservation = storage.coloredRootReservation;
        originalReservation = storage.rootReservation;
    }
    const std::uint64_t currentBytes = saturatingAdd(
        pointCloudNodePayloadBytes(*currentRoot),
        currentSourceRoot ? pointCloudNodePayloadBytes(*currentSourceRoot) : 0);
    if (currentBytes <= maximumBytes) {
        return currentBytes;
    }
    if (originalReservation) {
        throw std::length_error(
            "reserved hierarchy roots require separate replacement admission");
    }
    PointMemoryBudget::ReservationPtr replacementReservation;
    if (coloredReservation) {
        const auto reserved =
            coloredReservation->tryReserveSibling(maximumBytes);
        if (!reserved) {
            throw std::length_error("Cannot admit reduced colored roots");
        }
        replacementReservation = *reserved;
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

    if (replacementReservation) {
        const auto retain = [&](const PointCloudNodePayloadPtr &payload)
            -> PointCloudNodePayloadPtr {
            if (!payload) {
                return {};
            }
            auto copy = std::make_shared<PointCloudNodePayload>(*payload);
            for (auto &block : copy->blocks) {
                block = retainPointMemoryReservation(std::move(block),
                                                     replacementReservation);
            }
            return copy;
        };
        reduced = retain(reduced);
        reducedSource = retain(reducedSource);
        static_cast<void>(replacementReservation->tryResize(reducedBytes));
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
        hierarchicalStorage.rootReservation = replacementReservation;
        hierarchicalStorage.coloredRootReservation = replacementReservation;
        if (currentSourceRoot) {
            hierarchicalStorage.sourceRootPayload = reducedSource;
        }
        ++hierarchicalStorage.rootPayloadRevision;
        cache = hierarchicalStorage.decodedPageCache;
        participant = hierarchicalStorage.residencyParticipant;
        sourceId = hierarchicalStorage.sourceId;
        ++revision_;
    }
    participant->setRootBytes(reducedBytes,
                              replacementReservation ? reducedBytes : 0);
    const std::array protectedRoot{rootPointCloudNode};
    cache->insert(sourceId, std::move(reduced), protectedRoot);
    notifyInvalidated();
    return reducedBytes;
}

void PointDatasetRuntime::requestNodes(
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

void PointDatasetRuntime::setPinnedNodes(
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

class PointDatasetAttachment {
    friend class PointDatasetRuntime;
    std::unique_ptr<PointDatasetRuntime::Storage> storage;
};

std::shared_ptr<PointDatasetAttachment> PointDatasetRuntime::prepareAttachment(
    HierarchyResidencyCoordinatorPtr coordinator,
    DecodedPageCachePtr cache,
    std::shared_ptr<TaskScheduler> scheduler,
    const bool active,
    const std::uint64_t rootByteLimit,
    PointDecodeQueuePtr completionQueue) const
{
    auto attachment = std::make_shared<PointDatasetAttachment>();
    if (!hierarchical()) {
        return attachment;
    }
    Storage::HierarchicalSceneStorage candidate;
    {
        const std::scoped_lock lock(mutex_);
        candidate =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
    }
    candidate.decodeRequests.clear();
    candidate.completionQueue = completionQueue
                                    ? std::move(completionQueue)
                                    : std::make_shared<PointDecodeQueue>(
                                          std::make_shared<PointMemoryBudget>(
                                              coordinator->byteBudget()));
    candidate.outstandingDecodeCallbacks = 0;
    const auto bytes = saturatingAdd(
        pointCloudNodePayloadBytes(*candidate.rootPayload),
        candidate.sourceRootPayload
            ? pointCloudNodePayloadBytes(*candidate.sourceRootPayload)
            : 0);
    if (bytes > rootByteLimit) {
        if (candidate.rootReservation || candidate.coloredRootReservation) {
            throw std::length_error("reserved hierarchy roots require separate "
                                    "replacement admission");
        }
        if (candidate.sourceRootPayload) {
            auto reduced = reducedRootPayloadPair(*candidate.sourceRootPayload,
                                                  *candidate.rootPayload,
                                                  rootByteLimit);
            candidate.rootPayload = std::move(reduced.colored);
            candidate.sourceRootPayload = std::move(reduced.source);
        } else {
            candidate.rootPayload =
                reducedRootPayload(*candidate.rootPayload, rootByteLimit);
        }
        ++candidate.rootPayloadRevision;
    }
    candidate.residencyParticipant = coordinator->registerParticipant(
        saturatingAdd(
            pointCloudNodePayloadBytes(*candidate.rootPayload),
            candidate.sourceRootPayload
                ? pointCloudNodePayloadBytes(*candidate.sourceRootPayload)
                : 0),
        active,
        reservedRootBytes());
    candidate.decodedPageCache = std::move(cache);
    candidate.decodeScheduler = std::move(scheduler);
    candidate.sharedDocumentResources = true;
    candidate.suspendPinnedResubmission = false;
    std::vector<PointCloudNodeId> pins{rootPointCloudNode};
    pins.insert(
        pins.end(), candidate.pinnedNodes.begin(), candidate.pinnedNodes.end());
    // This cache belongs to the private candidate registry. Existing runtime
    // caches, roots, and decode requests remain untouched until commit.
    if (candidate.decodedPageCache->peek(sourceId_, rootPointCloudNode) !=
        candidate.rootPayload) {
        candidate.decodedPageCache->insert(
            sourceId_, candidate.rootPayload, pins);
    }
    candidate.decodedPageCache->setPinned(sourceId_, pins);
    attachment->storage = std::make_unique<Storage>(std::move(candidate));
    return attachment;
}

std::shared_ptr<PointDatasetAttachment>
PointDatasetRuntime::prepareStandaloneAttachment() const
{
    if (!hierarchical()) {
        return std::make_shared<PointDatasetAttachment>();
    }
    HierarchyResidencyCoordinatorPtr coordinator;
    {
        const std::scoped_lock lock(mutex_);
        coordinator =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value)
                .standaloneResidencyCoordinator;
    }
    auto cache = std::make_shared<DecodedPageCache>(std::max<std::uint64_t>(
        1,
        saturatingAdd(coordinator->availableResidencyBytes(),
                      reservedRootBytes())));
    auto scheduler = std::make_shared<TaskScheduler>(
        1,
        std::max<std::uint64_t>(1,
                                std::min(TaskScheduler::defaultActiveByteBudget,
                                         coordinator->byteBudget())));
    auto result = prepareAttachment(coordinator,
                                    std::move(cache),
                                    std::move(scheduler),
                                    true,
                                    std::numeric_limits<std::uint64_t>::max());
    std::get<Storage::HierarchicalSceneStorage>(result->storage->value)
        .sharedDocumentResources = false;
    return result;
}

void PointDatasetRuntime::quiesceForAttachment() noexcept
{
    cancelAndWaitForDecodes();
}

void PointDatasetRuntime::commitAttachment(
    PointDatasetAttachment &attachment) noexcept
{
    if (!attachment.storage) {
        return;
    }
    const std::scoped_lock lock(mutex_);
    const auto &live =
        std::get<Storage::HierarchicalSceneStorage>(storage_->value);
    auto &next =
        std::get<Storage::HierarchicalSceneStorage>(attachment.storage->value);
    next.decodeRequestsQueued = live.decodeRequestsQueued;
    next.decodeRequestsStarted = live.decodeRequestsStarted;
    next.decodeRequestsCompleted = live.decodeRequestsCompleted;
    next.decodeRequestsCancelled = live.decodeRequestsCancelled;
    next.decodeRequestsFailed = live.decodeRequestsFailed;
    storage_.swap(attachment.storage);
    ++revision_;
}

void PointDatasetRuntime::setDocumentHierarchyResources(
    HierarchyResidencyCoordinatorPtr coordinator,
    DecodedPageCachePtr cache,
    std::shared_ptr<TaskScheduler> scheduler,
    const bool active,
    PointDecodeQueuePtr completionQueue)
{
    if (!hierarchical()) {
        return;
    }
    if (!coordinator || !cache || !scheduler) {
        throw std::invalid_argument(
            "hierarchical scene requires shared document resources");
    }
    if (!completionQueue) {
        completionQueue = std::make_shared<PointDecodeQueue>(
            std::make_shared<PointMemoryBudget>(coordinator->byteBudget()));
    }
    cancelAndWaitForDecodes();
    auto &hierarchicalStorage =
        std::get<Storage::HierarchicalSceneStorage>(storage_->value);
    auto participant = coordinator->registerParticipant(
        saturatingAdd(
            pointCloudNodePayloadBytes(*hierarchicalStorage.rootPayload),
            hierarchicalStorage.sourceRootPayload
                ? pointCloudNodePayloadBytes(
                      *hierarchicalStorage.sourceRootPayload)
                : 0),
        active,
        reservedRootBytes());

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
        hierarchicalStorage.completionQueue = std::move(completionQueue);
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

void PointDatasetRuntime::useStandaloneHierarchyResidency()
{
    if (hierarchical()) {
        auto &hierarchicalStorage =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        auto cache = std::make_shared<DecodedPageCache>(std::max<std::uint64_t>(
            1,
            saturatingAdd(hierarchicalStorage.standaloneResidencyCoordinator
                              ->availableResidencyBytes(),
                          hierarchicalStorage.sharedDocumentResources
                              ? reservedRootBytes()
                              : 0)));
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

void PointDatasetRuntime::setHierarchyResidencyActive(const bool active)
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

void PointDatasetRuntime::syncHierarchyResidencyBudget()
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

void PointDatasetRuntime::trimDecodedCache(
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

std::uint64_t PointDatasetRuntime::decodedByteBudget() const
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

std::uint64_t PointDatasetRuntime::decodedResidentBytes() const
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

std::uint64_t PointDatasetRuntime::decodedResidentPoints() const
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

bool PointDatasetRuntime::hasPreparedDecodes() const
{
    const std::scoped_lock lock(mutex_);
    if (const auto *state =
            std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value)) {
        return std::ranges::any_of(state->decodeRequests,
                                   [](const auto &entry) {
                                       return entry.second->finished.load();
                                   });
    }
    return false;
}

bool PointDatasetRuntime::decodeInFlight() const
{
    const std::scoped_lock lock(mutex_);
    if (const auto *hierarchical =
            std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value)) {
        return !hierarchical->decodeRequests.empty();
    }
    return false;
}

std::string PointDatasetRuntime::hierarchyError() const
{
    const std::scoped_lock lock(mutex_);
    if (const auto *hierarchical =
            std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value)) {
        return hierarchical->hierarchyError;
    }
    return {};
}

PointDatasetRuntimeMetrics PointDatasetRuntime::hierarchyMetrics() const
{
    PointDatasetRuntimeMetrics result;
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

PointCloudStorageMetrics PointDatasetRuntime::storageMetrics() const
{
    const auto *hierarchical =
        std::get_if<Storage::HierarchicalSceneStorage>(&storage_->value);
    return hierarchical ? hierarchical->dataSource->storageMetrics()
                        : PointCloudStorageMetrics{};
}

RasterPointColorMetrics PointDatasetRuntime::rasterPointColorMetrics() const
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

bool PointDatasetRuntime::hasRasterPointColors() const
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
PointDatasetRuntime::rasterPointColorizeAvailability() const
{
    const std::scoped_lock lock(mutex_);
    return rasterPointColorizeAvailabilityLocked();
}

RasterPointColorizeAvailability
PointDatasetRuntime::rasterPointColorizeAvailabilityLocked() const
{
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
PointDatasetRuntime::rasterPointColorizeTarget() const
{
    const std::scoped_lock lock(mutex_);
    if (const auto *flat =
            std::get_if<Storage::FlatSceneStorage>(&storage_->value)) {
        if (!loadingComplete_) {
            return std::nullopt;
        }
        return RasterColorizeTargetSnapshot{
            .sourceId = sourceId_,
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
        .sourceId = sourceId_,
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

struct PointColorPublication::State {
    const PointDatasetRuntime *owner = nullptr;
    std::unique_ptr<PointDatasetRuntime::Storage> candidate;
    std::vector<SceneBlock> expectedBlocks;
    std::shared_ptr<std::vector<std::uint32_t>> expectedColors;
    PointCloudDataSourcePtr expectedSource;
    std::uint64_t expectedRootRevision = 0;
    std::unique_lock<std::mutex> runtimeLock;
    DecodedPageCachePtr cache;
    std::unique_ptr<DecodedPageCache::RootReplacement> cacheReplacement;
    bool committed = false;
};

PointColorPublication::PointColorPublication()
    : state_(std::make_unique<State>())
{
}
PointColorPublication::~PointColorPublication() = default;

std::unique_ptr<PointColorPublication> PointDatasetRuntime::preparePointColors(
    PointColorInstallationPtr installation) const
{
    if (installation && installation->sourceId != sourceId_) {
        throw std::invalid_argument(
            "Raster color transaction targets a different dataset");
    }
    auto publication =
        std::unique_ptr<PointColorPublication>(new PointColorPublication());
    publication->view_ = datasetView();
    auto &prepared = *publication->state_;
    prepared.owner = this;
    const std::scoped_lock lock(mutex_);
    if (const auto *flat =
            std::get_if<Storage::FlatSceneStorage>(&storage_->value)) {
        auto candidate = *flat;
        prepared.expectedBlocks = flat->blocks;
        prepared.expectedColors = flat->displacedColors;
        if (installation) {
            const auto *next =
                std::get_if<PointColorFlatInstallation>(&installation->data);
            if (!next || next->expectedBlocks.size() != flat->blocks.size() ||
                next->replacementBlocks.size() != flat->blocks.size() ||
                static_cast<bool>(next->displacedSourceColors) ==
                    static_cast<bool>(flat->displacedColors)) {
                return {};
            }
            for (std::size_t index = 0; index < flat->blocks.size(); ++index) {
                const auto &live = flat->blocks[index];
                const auto &expected = next->expectedBlocks[index];
                if (expected.block.lock() != live.block ||
                    expected.id != live.id ||
                    next->replacementBlocks[index].id != live.id ||
                    !next->replacementBlocks[index].block ||
                    expected.pointCount != live.block->points.size() ||
                    expected.attributeCount != live.block->attributes.size()) {
                    return {};
                }
            }
            candidate.blocks = next->replacementBlocks;
            // Preserve the charge even for callers constructing installations
            // directly.
            for (auto &entry : candidate.blocks) {
                entry.block = retainPointMemoryReservation(
                    std::move(entry.block), next->stagingReservation);
            }
            candidate.residentMemoryReservation = next->stagingReservation;
            if (!candidate.displacedColors) {
                candidate.displacedColors = retainPointMemoryReservation(
                    next->displacedSourceColors, next->colorReservation);
                candidate.colorReservation = next->colorReservation;
            }
        } else {
            if (!flat->displacedColors) {
                return {};
            }
            if (!flat->colorReservation) {
                throw std::logic_error(
                    "Retained source colors have no point-memory reservation");
            }
            const auto reserved =
                flat->colorReservation->tryReserveSibling(flat->residentBytes);
            if (!reserved) {
                return {};
            }
            candidate.blocks.clear();
            std::size_t offset = 0;
            for (const auto &entry : flat->blocks) {
                auto block = std::make_shared<PointBlock>(*entry.block);
                if (offset > flat->displacedColors->size() ||
                    block->points.size() >
                        flat->displacedColors->size() - offset) {
                    throw std::logic_error(
                        "Retained source colors do not match flat blocks");
                }
                for (auto &point : block->points) {
                    point.rgba = (*flat->displacedColors)[offset++];
                }
                candidate.blocks.push_back(
                    {.id = entry.id,
                     .block = retainPointMemoryReservation(std::move(block),
                                                           *reserved)});
            }
            if (offset != flat->displacedColors->size()) {
                throw std::logic_error(
                    "Retained source colors contain trailing entries");
            }
            candidate.residentMemoryReservation = *reserved;
            candidate.displacedColors.reset();
            candidate.colorReservation.reset();
        }
        candidate.residentBytes = 0;
        for (const auto &entry : candidate.blocks) {
            candidate.residentBytes = saturatingAdd(
                candidate.residentBytes,
                saturatingAdd<std::uint64_t>(
                    saturatingMultiply<std::uint64_t>(
                        entry.block->points.capacity(), sizeof(GpuPoint)),
                    saturatingMultiply<std::uint64_t>(
                        entry.block->attributes.capacity(),
                        sizeof(PointAttributes))));
        }
        if (candidate.residentMemoryReservation &&
            candidate.residentBytes >
                candidate.residentMemoryReservation->bytes()) {
            throw std::length_error(
                "Color replacement exceeds its admitted allocation");
        }
        prepared.candidate = std::make_unique<Storage>(std::move(candidate));
    } else {
        const auto &live =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        auto candidate = live;
        candidate.decodeRequests.clear();

        candidate.outstandingDecodeCallbacks = 0;
        prepared.expectedSource = live.dataSource;
        prepared.expectedRootRevision = live.rootPayloadRevision;
        if (installation) {
            const auto *next = std::get_if<PointColorHierarchyInstallation>(
                &installation->data);
            if (!next || !next->baseSource || !next->colorizedSource ||
                !next->sourceRootPayload || !next->coloredRootPayload ||
                next->sourceRootPayload->nodeId != rootPointCloudNode ||
                next->coloredRootPayload->nodeId != rootPointCloudNode) {
                throw std::invalid_argument(
                    "Hierarchical raster color transaction is incomplete");
            }
            if ((live.baseDataSource ? live.baseDataSource : live.dataSource) !=
                    next->baseSource ||
                live.rootPayloadRevision != next->expectedRootPayloadRevision ||
                (live.sourceRootPayload
                     ? live.sourceRootPayload
                     : live.rootPayload) != next->sourceRootPayload) {
                return {};
            }
            candidate.baseDataSource = next->baseSource;
            candidate.sourceRootPayload = next->sourceRootPayload;
            candidate.dataSource = next->colorizedSource;
            auto root = std::make_shared<PointCloudNodePayload>(
                *next->coloredRootPayload);
            for (auto &block : root->blocks) {
                block = retainPointMemoryReservation(
                    std::move(block), next->rootStagingReservation);
            }
            candidate.rootPayload = std::move(root);
            candidate.coloredRootReservation = next->rootStagingReservation;
        } else {
            if (!live.baseDataSource || !live.sourceRootPayload) {
                return {};
            }
            candidate.dataSource = live.baseDataSource;
            candidate.rootPayload = live.sourceRootPayload;
            candidate.baseDataSource.reset();
            candidate.sourceRootPayload.reset();
            candidate.coloredRootReservation.reset();
        }
        publication->view_.availability.storage =
            candidate.dataSource->storageMetrics();
        prepared.candidate = std::make_unique<Storage>(std::move(candidate));
    }
    return publication;
}

bool PointDatasetRuntime::prepareColorCommit(PointColorPublication &publication)
{
    auto &prepared = *publication.state_;
    if (prepared.owner != this || prepared.committed ||
        prepared.runtimeLock.owns_lock()) {
        return false;
    }
    // Workers must finish before acquiring the cache transaction lock. No
    // observer or source I/O is invoked while the document/runtime swap is
    // armed.
    if (hierarchical()) {
        cancelAndWaitForDecodes();
    }
    std::unique_lock lock(mutex_);
    if (const auto *flat =
            std::get_if<Storage::FlatSceneStorage>(&storage_->value)) {
        if (flat->displacedColors != prepared.expectedColors ||
            flat->blocks.size() != prepared.expectedBlocks.size()) {
            return false;
        }
        for (std::size_t index = 0; index < flat->blocks.size(); ++index) {
            if (flat->blocks[index].id != prepared.expectedBlocks[index].id ||
                flat->blocks[index].block !=
                    prepared.expectedBlocks[index].block) {
                return false;
            }
        }
    } else {
        const auto &live =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        if (live.dataSource != prepared.expectedSource ||
            live.rootPayloadRevision != prepared.expectedRootRevision) {
            return false;
        }
        std::vector<PointCloudNodeId> pins{rootPointCloudNode};
        pins.insert(
            pins.end(), live.pinnedNodes.begin(), live.pinnedNodes.end());
        prepared.cache = live.decodedPageCache;
        prepared.cacheReplacement = prepared.cache->prepareSourceRoot(
            sourceId_,
            std::get<Storage::HierarchicalSceneStorage>(
                prepared.candidate->value)
                .rootPayload,
            pins);
    }
    prepared.runtimeLock = std::move(lock);
    return true;
}

void PointDatasetRuntime::commitPointColors(
    PointColorPublication &publication) noexcept
{
    auto &prepared = *publication.state_;
    if (prepared.owner != this || prepared.committed ||
        !prepared.runtimeLock.owns_lock()) {
        return;
    }
    if (auto *flat = std::get_if<Storage::FlatSceneStorage>(&storage_->value)) {
        std::swap(
            *flat,
            std::get<Storage::FlatSceneStorage>(prepared.candidate->value));
    } else {
        auto &live =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        auto &next = std::get<Storage::HierarchicalSceneStorage>(
            prepared.candidate->value);
        live.dataSource.swap(next.dataSource);
        live.baseDataSource.swap(next.baseDataSource);
        live.rootPayload.swap(next.rootPayload);
        live.sourceRootPayload.swap(next.sourceRootPayload);
        live.coloredRootReservation.swap(next.coloredRootReservation);
        ++live.rootPayloadRevision;
        live.failedNodes.clear();
        live.hierarchyError.clear();
        const auto sourceBytes =
            live.sourceRootPayload
                ? pointCloudNodePayloadBytes(*live.sourceRootPayload)
                : 0;
        const auto rootBytes = pointCloudNodePayloadBytes(*live.rootPayload);
        const auto originalCharge =
            live.rootReservation
                ? std::min(live.rootReservation->bytes(),
                           sourceBytes ? sourceBytes : rootBytes)
                : 0;
        const auto coloredCharge =
            live.coloredRootReservation
                ? std::min(live.coloredRootReservation->bytes(), rootBytes)
                : 0;
        live.residencyParticipant->setRootBytes(
            saturatingAdd(sourceBytes, rootBytes),
            saturatingAdd(originalCharge, coloredCharge));
        prepared.cacheReplacement->commit();
    }
    ++revision_;
    prepared.committed = true;
    prepared.runtimeLock.unlock();
}

void PointDatasetRuntime::notifyPointColors() noexcept
{
    try {
        syncHierarchyResidencyBudget();
        requestNodes({});
    } catch (...) {
        // Publication already succeeded; later residency work can be retried.
    }
    notifyInvalidated();
}

RasterPointColorApplyOutcome PointDatasetRuntime::applyRasterPointColors(
    PointColorInstallationPtr installation)
{
    if (!installation) {
        throw std::invalid_argument("Missing color installation");
    }
    auto publication = preparePointColors(std::move(installation));
    if (!publication || !prepareColorCommit(*publication)) {
        return RasterPointColorApplyOutcome::Stale;
    }
    commitPointColors(*publication);
    publication.reset();
    notifyPointColors();
    return RasterPointColorApplyOutcome::Applied;
}

RasterPointColorApplyOutcome PointDatasetRuntime::revertPointColors()
{
    auto publication = preparePointColors();
    if (!publication || !prepareColorCommit(*publication)) {
        return RasterPointColorApplyOutcome::Stale;
    }
    commitPointColors(*publication);
    publication.reset();
    notifyPointColors();
    return RasterPointColorApplyOutcome::Applied;
}

void PointDatasetRuntime::submitDecodeLocked(const PointCloudNodeId id)
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
    auto slot = hierarchical.completionQueue->reserve(estimated);
    if (!slot) {
        return;
    }
    auto request = std::make_shared<HierarchyDecodeRequest>();
    request->slot = std::move(slot);
    request->contentRevision = hierarchical.rootPayloadRevision;
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

void PointDatasetRuntime::executeDecode(
    const PointCloudNodeId id,
    const std::shared_ptr<HierarchyDecodeRequest> &request) noexcept
{
    PointCloudDataSourcePtr source;
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
        if (payload &&
            !request->slot->fit(pointCloudNodePayloadBytes(*payload))) {
            payload.reset();
            throw std::length_error(
                "decoded point payload exceeds admitted memory");
        }
    } catch (const PointCloudDataSourceCancelled &) {
        cancelled = true;
    } catch (const std::exception &exception) {
        error = exception.what();
    } catch (...) {
        error = "unknown hierarchy decode failure";
    }

    {
        const std::scoped_lock lock(mutex_);
        if (!cancelled && !request->stop.stop_requested()) {
            request->payload = std::move(payload);
            request->error = std::move(error);
        }
        request->finished = true;
        ++revision_;
    }
    notifyInvalidated();
    {
        const std::scoped_lock lock(mutex_);
        --std::get<Storage::HierarchicalSceneStorage>(storage_->value)
              .outstandingDecodeCallbacks;
        // Notify while locked: after unlocking a destructor may release the
        // condition variable, so the callback must not access this again.
        decodeFinished_.notify_all();
    }
}

std::size_t
PointDatasetRuntime::drainDecodeCompletions(const std::size_t maximum)
{
    if (!hierarchical()) {
        return 0;
    }
    std::size_t drained = 0;
    const std::scoped_lock lock(mutex_);
    auto &state = std::get<Storage::HierarchicalSceneStorage>(storage_->value);
    for (auto it = state.decodeRequests.begin();
         it != state.decodeRequests.end() && drained < maximum;) {
        const auto &request = it->second;
        if (!request->finished) {
            ++it;
            continue;
        }
        if (request->stop.stop_requested() ||
            request->contentRevision != state.rootPayloadRevision) {
            ++state.decodeRequestsCancelled;
        } else if (!request->error.empty()) {
            state.failedNodes.insert(it->first);
            state.hierarchyError = request->error;
            ++state.decodeRequestsFailed;
            ++revision_;
        } else if (request->payload) {
            const std::array protect{it->first};
            state.decodedPageCache->insert(
                state.sourceId, request->payload, protect);
            updateStatistics(*request->payload);
            ++state.decodeRequestsCompleted;
            ++revision_;
        }
        // A scheduler callback can retain the request briefly after publishing
        // finished. Admission transfers payload ownership to the cache now,
        // rather than retaining its memory charge until that callback dies.
        request->payload.reset();
        request->slot.reset();
        it = state.decodeRequests.erase(it);
        ++drained;
    }
    if (drained > 0 && !state.suspendPinnedResubmission) {
        // Exact-data operation pins can outlive a cancelled camera request.
        // Preserve their explicit request after discarding that old attempt.
        for (const auto id : state.pinnedNodes) {
            submitDecodeLocked(id);
        }
    }
    return drained;
}

void PointDatasetRuntime::finishQueuedCancellation(
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
        request->slot.reset();
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
    notifyInvalidated();
    {
        const std::scoped_lock lock(mutex_);
        --std::get<Storage::HierarchicalSceneStorage>(storage_->value)
              .outstandingDecodeCallbacks;
        // Notify while locked: after unlocking a destructor may release the
        // condition variable, so the callback must not access this again.
        decodeFinished_.notify_all();
    }
}

void PointDatasetRuntime::cancelAndWaitForDecodes() noexcept
{
    if (!storage_ || !hierarchical()) {
        return;
    }
    {
        const std::scoped_lock lock(mutex_);
        auto &state =
            std::get<Storage::HierarchicalSceneStorage>(storage_->value);
        state.suspendPinnedResubmission = true;
        for (const auto &[id, request] : state.decodeRequests) {
            static_cast<void>(id);
            request->stop.request_stop();
        }
    }
    // Cancel queued requests one at a time without allocating a temporary
    // vector during commit/destruction. Running requests observe their stop.
    for (;;) {
        std::shared_ptr<TaskScheduler> scheduler;
        std::shared_ptr<HierarchyDecodeRequest> request;
        {
            const std::scoped_lock lock(mutex_);
            auto &state =
                std::get<Storage::HierarchicalSceneStorage>(storage_->value);
            if (state.decodeRequests.empty()) {
                break;
            }
            request = state.decodeRequests.begin()->second;
            if (request->finished) {
                request->payload.reset();
                request->slot.reset();
                state.decodeRequests.erase(state.decodeRequests.begin());
                ++state.decodeRequestsCancelled;
                continue;
            }
            scheduler = state.decodeScheduler;
        }
        if (scheduler) {
            static_cast<void>(scheduler->cancel(request->taskId));
        }
        std::unique_lock lock(mutex_);
        decodeFinished_.wait(lock, [&] {
            return request->finished.load();
        });
    }
    std::unique_lock lock(mutex_);
    decodeFinished_.wait(lock, [this] {
        return std::get<Storage::HierarchicalSceneStorage>(storage_->value)
                   .outstandingDecodeCallbacks == 0;
    });
    std::get<Storage::HierarchicalSceneStorage>(storage_->value)
        .suspendPinnedResubmission = false;
}

void PointDatasetRuntime::updateCachePins()
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

void PointDatasetRuntime::updateStatistics(const PointCloudNodePayload &payload)
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

void PointDatasetRuntime::updateClassifications(const PointBlock &block)
{
    for (const GpuPoint &point : block.points) {
        presentClassifications_.setVisible(
            static_cast<std::uint8_t>(point.attributes & 0xffU), true);
    }
}

PointCloudScalarRanges PointDatasetRuntime::completeScalarRangesLocked() const
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
