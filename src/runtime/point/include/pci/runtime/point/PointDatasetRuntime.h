#pragma once

#include <pci/runtime/point/PointDecodeQueue.h>

#include <pci/pointcloud/PointClassificationFilter.h>
#include <pci/pointcloud/PointCloudMetadata.h>
#include <pci/pointcloud/PointDatasetDescriptor.h>
#include <pci/pointcloud/PointDatasetRuntimeSnapshot.h>
#include <pci/pointcloud/PointResidencyView.h>
#include <pci/pointcloud/RasterPointColorData.h>
#include <pci/runtime/point/DecodedPageCache.h>
#include <pci/runtime/point/PointColorInstallation.h>
#include <pci/tasking/TaskScheduler.h>

#include <pci/pointcloud/PointBlock.h>
#include <pci/pointcloud/PointCloudDataSource.h>
#include <pci/pointcloud/PointIdentity.h>
#include <pci/runtime/HierarchyResidencyCoordinator.h>
#include <pci/runtime/PointMemoryBudget.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pci {

class PointDatasetRuntime;
class PointDatasetPublication;
class PointDatasetAttachment;
class PointColorPublication;
using PointDatasetRuntimePtr = std::shared_ptr<PointDatasetRuntime>;

struct PointDatasetRuntimeInvalidationState;

class PointDatasetRuntimeInvalidationSubscription {
public:
    PointDatasetRuntimeInvalidationSubscription() = default;
    ~PointDatasetRuntimeInvalidationSubscription();

    PointDatasetRuntimeInvalidationSubscription(
        PointDatasetRuntimeInvalidationSubscription &&other) noexcept;
    PointDatasetRuntimeInvalidationSubscription &
    operator=(PointDatasetRuntimeInvalidationSubscription &&other) noexcept;
    PointDatasetRuntimeInvalidationSubscription(
        const PointDatasetRuntimeInvalidationSubscription &) = delete;
    PointDatasetRuntimeInvalidationSubscription &
    operator=(const PointDatasetRuntimeInvalidationSubscription &) = delete;

    void reset() noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

private:
    friend class PointDatasetRuntime;
    PointDatasetRuntimeInvalidationSubscription(
        std::weak_ptr<PointDatasetRuntimeInvalidationState> state,
        std::uint64_t id) noexcept;

    std::weak_ptr<PointDatasetRuntimeInvalidationState> state_;
    std::uint64_t id_ = 0;
};

struct PointDatasetRuntimeMetrics {
    DecodedCacheMetrics cache;
    PointCloudDataSourceMetrics source;
    std::uint64_t decodeRequestsQueued = 0;
    std::uint64_t decodeRequestsStarted = 0;
    std::uint64_t decodeRequestsCompleted = 0;
    std::uint64_t decodeRequestsCancelled = 0;
    std::uint64_t decodeRequestsFailed = 0;
};

class PointDatasetRuntime
    : public std::enable_shared_from_this<PointDatasetRuntime> {
public:
    explicit PointDatasetRuntime(PointCloudMetadata metadata,
                                 PointCloudSourceId sourceId = {});
    PointDatasetRuntime(
        PointCloudMetadata metadata,
        PointCloudDataSourcePtr dataSource,
        PointCloudNodePayloadPtr rootPayload,
        std::uint64_t decodedByteBudget = defaultDecodedCacheByteBudget,
        bool loadingComplete = true,
        PointCloudSourceId sourceId = {},
        PointMemoryBudget::ReservationPtr rootReservation = {});
    ~PointDatasetRuntime();

    PointDatasetRuntime(const PointDatasetRuntime &) = delete;
    PointDatasetRuntime &operator=(const PointDatasetRuntime &) = delete;

    [[nodiscard]] const PointCloudMetadata &metadata() const noexcept;
    [[nodiscard]] PointCloudSourceId sourceId() const noexcept;
    [[nodiscard]] PointDatasetDescriptor descriptor() const;
    [[nodiscard]] PointDatasetView datasetView() const;
    void addBlock(PointBlockPtr block);
    // Prepare imported data and its document-facing view without changing
    // this runtime. Publication notification follows the paired document swap.
    [[nodiscard]] std::unique_ptr<PointDatasetPublication>
    preparePublication(PointBlockPtr block = {}, bool complete = false) const;
    [[nodiscard]] bool
    commitPublication(PointDatasetPublication &publication) noexcept;
    void publishInvalidation() noexcept;
    [[nodiscard]] std::shared_ptr<PointDatasetAttachment>
    prepareAttachment(HierarchyResidencyCoordinatorPtr coordinator,
                      DecodedPageCachePtr cache,
                      std::shared_ptr<TaskScheduler> scheduler,
                      bool active,
                      std::uint64_t rootByteLimit,
                      PointDecodeQueuePtr completionQueue = {}) const;
    void commitAttachment(PointDatasetAttachment &attachment) noexcept;
    [[nodiscard]] std::shared_ptr<PointDatasetAttachment>
    prepareStandaloneAttachment() const;
    // Quiesce every participant before the first paired attachment swap.
    void quiesceForAttachment() noexcept;
    [[nodiscard]] std::vector<PointBlockPtr> blocks() const;
    [[nodiscard]] std::vector<SceneBlock> blockEntries() const;
    [[nodiscard]] std::uint64_t totalPointCount() const;
    [[nodiscard]] std::uint64_t revision() const;
    [[nodiscard]] Bounds3d bounds() const;
    [[nodiscard]] PointCloudScalarRanges scalarRanges() const;
    [[nodiscard]] PointClassificationFilter presentClassifications() const;
    [[nodiscard]] std::uint16_t intensityMinimum() const;
    [[nodiscard]] std::uint16_t intensityMaximum() const;
    [[nodiscard]] PointDatasetRuntimeSnapshot snapshot() const;
    void markLoadingComplete();
    void
    setResidentMemoryReservation(PointMemoryBudget::ReservationPtr reservation);
    [[nodiscard]] PointDatasetRuntimeInvalidationSubscription
    subscribeInvalidation(std::function<void()> callback);

    [[nodiscard]] bool hierarchical() const noexcept;
    [[nodiscard]] bool detailLimited() const noexcept;
    [[nodiscard]] PointCloudNode rootNode() const;
    [[nodiscard]] PointCloudNode node(PointCloudNodeId id) const;
    [[nodiscard]] PointCloudNodePayloadPtr nodePayload(PointCloudNodeId id);
    [[nodiscard]] PointCloudNodePayloadPtr
    peekNodePayload(PointCloudNodeId id) const;
    [[nodiscard]] PointResidencyViewPtr residencyView() const;
    [[nodiscard]] std::uint64_t residencyContentRevision() const noexcept;
    void applyDecodedLookupEffect(PointCloudNodeId id, bool resident);
    [[nodiscard]] std::optional<PointCloudFullDetailInfo>
    fullDetailInfo() const;
    [[nodiscard]] std::uint64_t minimumRootPayloadBytes() const;
    [[nodiscard]] std::uint64_t reservedRootBytes() const;
    [[nodiscard]] std::uint64_t
    limitRootPayloadBytes(std::uint64_t maximumBytes);
    void requestNodes(std::span<const PointCloudNodeId> nodes);
    // Owner-thread admission; may resubmit outstanding exact-data pins, but
    // never waits for workers or requires owner-queue pumping.
    std::size_t
    drainDecodeCompletions(std::size_t maximum = PointDecodeQueue::capacity);
    // Reserved for explicit exact-data operations such as raster recoloring
    // and export. Interactive rendering must not pin a whole hierarchy.
    void setPinnedNodes(std::span<const PointCloudNodeId> nodes);
    // Hierarchical scenes start with standalone residency. SceneRuntime
    // installs shared resources while attached, then restores
    // standalone residency before removal. Either state may proceed directly to
    // destruction.
    void
    setDocumentHierarchyResources(HierarchyResidencyCoordinatorPtr coordinator,
                                  DecodedPageCachePtr cache,
                                  std::shared_ptr<TaskScheduler> scheduler,
                                  bool active,
                                  PointDecodeQueuePtr completionQueue = {});
    void useStandaloneHierarchyResidency();
    void setHierarchyResidencyActive(bool active);
    void syncHierarchyResidencyBudget();
    void
    trimDecodedCache(std::span<const PointCloudNodeId> protectedNodes = {});
    [[nodiscard]] std::uint64_t decodedByteBudget() const;
    [[nodiscard]] std::uint64_t decodedResidentBytes() const;
    [[nodiscard]] std::uint64_t decodedResidentPoints() const;
    [[nodiscard]] bool decodeInFlight() const;
    [[nodiscard]] bool hasPreparedDecodes() const;
    [[nodiscard]] std::string hierarchyError() const;
    [[nodiscard]] PointDatasetRuntimeMetrics hierarchyMetrics() const;
    [[nodiscard]] PointCloudStorageMetrics storageMetrics() const;
    [[nodiscard]] RasterPointColorMetrics rasterPointColorMetrics() const;
    [[nodiscard]] bool hasRasterPointColors() const;
    [[nodiscard]] RasterPointColorizeAvailability
    rasterPointColorizeAvailability() const;
    [[nodiscard]] std::optional<RasterColorizeTargetSnapshot>
    rasterPointColorizeTarget() const;
    [[nodiscard]] RasterPointColorApplyOutcome
    applyRasterPointColors(PointColorInstallationPtr installation);
    [[nodiscard]] RasterPointColorApplyOutcome revertPointColors();
    // A null installation prepares restoration of the original source colors.
    // Prepare the document from view(), then prepareColorCommit as the last
    // fallible step. Commit both owners before notifying any observer.
    [[nodiscard]] std::unique_ptr<PointColorPublication>
    preparePointColors(PointColorInstallationPtr installation = {}) const;
    [[nodiscard]] bool prepareColorCommit(PointColorPublication &publication);
    void commitPointColors(PointColorPublication &publication) noexcept;
    void notifyPointColors() noexcept;

private:
    friend class PointDatasetAttachment;
    friend class PointColorPublication;
    struct Storage;
    struct HierarchyDecodeRequest;

    void submitDecodeLocked(PointCloudNodeId id);
    void executeDecode(
        PointCloudNodeId id,
        const std::shared_ptr<HierarchyDecodeRequest> &request) noexcept;
    void finishQueuedCancellation(
        PointCloudNodeId id,
        const std::shared_ptr<HierarchyDecodeRequest> &request) noexcept;
    void cancelAndWaitForDecodes() noexcept;
    void updateCachePins();
    void updateStatistics(const PointCloudNodePayload &payload);
    void updateClassifications(const PointBlock &block);
    [[nodiscard]] RasterPointColorizeAvailability
    rasterPointColorizeAvailabilityLocked() const;
    [[nodiscard]] PointCloudScalarRanges completeScalarRangesLocked() const;
    void notifyInvalidated() noexcept;

    const PointCloudSourceId sourceId_;
    const PointCloudMetadata metadata_;
    std::unique_ptr<Storage> storage_;
    mutable std::mutex mutex_;
    std::condition_variable decodeFinished_;
    // Readers poll this on every frame. The mutation itself remains protected
    // by mutex_, while the atomic makes the unchanged-snapshot fast path
    // lock-free.
    std::atomic_uint64_t revision_ = 0;
    std::uint16_t intensityMinimum_ = 0;
    std::uint16_t intensityMaximum_ = 0;
    bool haveIntensityStatistics_ = false;
    PointClassificationFilter presentClassifications_ =
        PointClassificationFilter::noneVisible();
    bool loadingComplete_ = false;
    std::shared_ptr<PointDatasetRuntimeInvalidationState> invalidationState_;
};

class PointDatasetPublication final {
public:
    [[nodiscard]] const PointDatasetView &view() const noexcept
    {
        return view_;
    }

private:
    friend class PointDatasetRuntime;
    PointDatasetRuntimePtr candidate_;
    PointDatasetView view_;
    std::uint64_t expectedRevision_ = 0;
    bool complete_ = false;
    bool committed_ = false;
};

class PointColorPublication final {
public:
    ~PointColorPublication();
    [[nodiscard]] const PointDatasetView &view() const noexcept
    {
        return view_;
    }

private:
    friend class PointDatasetRuntime;
    PointColorPublication();
    struct State;
    std::unique_ptr<State> state_;
    PointDatasetView view_;
};

} // namespace pci
