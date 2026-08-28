#pragma once

#include "pointcloud/PointClassificationFilter.h"
#include "pointcloud/PointCloudMetadata.h"
#include "scene/DecodedPageCache.h"
#include "scene/HierarchyResidencyCoordinator.h"
#include "scene/PointBlock.h"
#include "scene/PointCloudDataSource.h"
#include "scene/PointCloudSceneSnapshot.h"
#include "scene/PointMemoryBudget.h"
#include "scene/RasterPointColorize.h"
#include "tasking/TaskScheduler.h"

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

struct PointCloudSceneInvalidationState;

class PointCloudSceneInvalidationSubscription {
public:
    PointCloudSceneInvalidationSubscription() = default;
    ~PointCloudSceneInvalidationSubscription();

    PointCloudSceneInvalidationSubscription(
        PointCloudSceneInvalidationSubscription &&other) noexcept;
    PointCloudSceneInvalidationSubscription &
    operator=(PointCloudSceneInvalidationSubscription &&other) noexcept;
    PointCloudSceneInvalidationSubscription(
        const PointCloudSceneInvalidationSubscription &) = delete;
    PointCloudSceneInvalidationSubscription &
    operator=(const PointCloudSceneInvalidationSubscription &) = delete;

    void reset() noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

private:
    friend class PointCloudScene;
    PointCloudSceneInvalidationSubscription(
        std::weak_ptr<PointCloudSceneInvalidationState> state,
        std::uint64_t id) noexcept;

    std::weak_ptr<PointCloudSceneInvalidationState> state_;
    std::uint64_t id_ = 0;
};

struct PointCloudSceneMetrics {
    DecodedCacheMetrics cache;
    PointCloudDataSourceMetrics source;
    std::uint64_t decodeRequestsQueued = 0;
    std::uint64_t decodeRequestsStarted = 0;
    std::uint64_t decodeRequestsCompleted = 0;
    std::uint64_t decodeRequestsCancelled = 0;
    std::uint64_t decodeRequestsFailed = 0;
};

struct RasterPointColorMetrics {
    std::uint64_t activeColorTableBytes = 0;
    std::uint64_t flatDisplacedColorBytes = 0;
    std::uint64_t retainedSourceRootBytes = 0;
    std::uint64_t retainedColoredRootBytes = 0;
};

class PointCloudScene : public std::enable_shared_from_this<PointCloudScene> {
public:
    explicit PointCloudScene(PointCloudMetadata metadata);
    PointCloudScene(
        PointCloudMetadata metadata,
        PointCloudDataSourcePtr dataSource,
        PointCloudNodePayloadPtr rootPayload,
        std::uint64_t decodedByteBudget = defaultDecodedCacheByteBudget,
        bool loadingComplete = true);
    ~PointCloudScene();

    PointCloudScene(const PointCloudScene &) = delete;
    PointCloudScene &operator=(const PointCloudScene &) = delete;

    [[nodiscard]] const PointCloudMetadata &metadata() const noexcept;
    void addBlock(PointBlockPtr block);
    [[nodiscard]] std::vector<PointBlockPtr> blocks() const;
    [[nodiscard]] std::vector<SceneBlock> blockEntries() const;
    [[nodiscard]] std::uint64_t totalPointCount() const;
    [[nodiscard]] std::uint64_t revision() const;
    [[nodiscard]] Bounds3d bounds() const;
    [[nodiscard]] PointCloudScalarRanges scalarRanges() const;
    [[nodiscard]] PointClassificationFilter presentClassifications() const;
    [[nodiscard]] std::uint16_t intensityMinimum() const;
    [[nodiscard]] std::uint16_t intensityMaximum() const;
    [[nodiscard]] PointCloudSceneSnapshot snapshot() const;
    void markLoadingComplete();
    void
    setResidentMemoryReservation(PointMemoryBudget::ReservationPtr reservation);
    [[nodiscard]] PointCloudSceneInvalidationSubscription
    subscribeInvalidation(std::function<void()> callback);

    [[nodiscard]] bool hierarchical() const noexcept;
    [[nodiscard]] bool detailLimited() const noexcept;
    [[nodiscard]] PointCloudNode rootNode() const;
    [[nodiscard]] PointCloudNode node(PointCloudNodeId id) const;
    [[nodiscard]] PointCloudNodePayloadPtr nodePayload(PointCloudNodeId id);
    [[nodiscard]] PointCloudNodePayloadPtr
    peekNodePayload(PointCloudNodeId id) const;
    [[nodiscard]] std::optional<PointCloudFullDetailInfo>
    fullDetailInfo() const;
    [[nodiscard]] std::uint64_t minimumRootPayloadBytes() const;
    [[nodiscard]] std::uint64_t
    limitRootPayloadBytes(std::uint64_t maximumBytes);
    void requestNodes(std::span<const PointCloudNodeId> nodes);
    void setPinnedNodes(std::span<const PointCloudNodeId> nodes);
    // Hierarchical scenes start with standalone residency. SceneDocument
    // installs document-owned resources while attached, then restores
    // standalone residency before removal. Either state may proceed directly to
    // destruction.
    void
    setDocumentHierarchyResources(HierarchyResidencyCoordinatorPtr coordinator,
                                  DecodedPageCachePtr cache,
                                  std::shared_ptr<TaskScheduler> scheduler,
                                  bool active);
    void useStandaloneHierarchyResidency();
    void setHierarchyResidencyActive(bool active);
    void syncHierarchyResidencyBudget();
    void
    trimDecodedCache(std::span<const PointCloudNodeId> protectedNodes = {});
    [[nodiscard]] std::uint64_t decodedByteBudget() const;
    [[nodiscard]] std::uint64_t decodedResidentBytes() const;
    [[nodiscard]] std::uint64_t decodedResidentPoints() const;
    [[nodiscard]] bool decodeInFlight() const;
    [[nodiscard]] std::string hierarchyError() const;
    [[nodiscard]] PointCloudSceneMetrics hierarchyMetrics() const;
    [[nodiscard]] PointCloudStorageMetrics storageMetrics() const;
    [[nodiscard]] RasterPointColorMetrics rasterPointColorMetrics() const;
    [[nodiscard]] bool hasRasterPointColors() const;
    [[nodiscard]] RasterPointColorizeAvailability
    rasterPointColorizeAvailability() const;
    [[nodiscard]] std::optional<RasterColorizeTargetSnapshot>
    rasterPointColorizeTarget() const;
    [[nodiscard]] RasterPointColorApplyOutcome
    applyRasterPointColors(RasterColorizePreparedPtr prepared);
    [[nodiscard]] RasterPointColorApplyOutcome revertPointColors();

private:
    struct Storage;
    struct HierarchyDecodeRequest;

    void submitDecodeLocked(PointCloudNodeId id);
    void executeDecode(
        PointCloudNodeId id,
        const std::shared_ptr<HierarchyDecodeRequest> &request) noexcept;
    void finishQueuedCancellation(
        PointCloudNodeId id,
        const std::shared_ptr<HierarchyDecodeRequest> &request) noexcept;
    void cancelAndWaitForDecodes();
    void updateCachePins();
    void updateStatistics(const PointCloudNodePayload &payload);
    void updateClassifications(const PointBlock &block);
    [[nodiscard]] PointCloudScalarRanges completeScalarRangesLocked() const;
    void notifyInvalidated() noexcept;

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
    std::shared_ptr<PointCloudSceneInvalidationState> invalidationState_;
};

} // namespace pci
