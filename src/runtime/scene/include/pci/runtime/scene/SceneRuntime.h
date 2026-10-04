#pragma once

#include <pci/runtime/point/PointResidencyService.h>

#include <pci/foundation/Generation.h>
#include <pci/pointcloud/PointCloudDataSource.h>
#include <pci/pointcloud/PointDatasetDescriptor.h>
#include <pci/raster/RasterDatasetDescriptor.h>
#include <pci/raster/RasterTileSource.h>
#include <pci/runtime/HierarchyResidencyCoordinator.h>
#include <pci/runtime/PointMemoryBudget.h>
#include <pci/tasking/TaskScheduler.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <variant>

namespace pci {

class PointDatasetRuntime;
class PointDatasetAttachment;

struct PointRuntimeBinding {
    PointDatasetDescriptor descriptor;
    std::shared_ptr<PointDatasetRuntime> runtime;
    BindingGeneration generation;
    bool active = true;
};

struct PointRuntimeActivityUpdate {
    PointCloudSourceId sourceId;
    BindingGeneration generation;
    bool active = true;
};

struct RasterRuntimeBinding {
    RasterDatasetDescriptor descriptor;
    RasterTileSourcePtr source;
    BindingGeneration generation;
};

using SceneRuntimeBinding =
    std::variant<PointRuntimeBinding, RasterRuntimeBinding>;

class SceneRuntimeSnapshot final {
public:
    [[nodiscard]] std::shared_ptr<PointDatasetRuntime>
    point(PointCloudSourceId sourceId, BindingGeneration generation) const;
    [[nodiscard]] RasterTileSourcePtr
    raster(RasterSourceId sourceId, BindingGeneration generation) const;
    [[nodiscard]] std::optional<bool>
    pointActive(PointCloudSourceId sourceId,
                BindingGeneration generation) const noexcept;

    [[nodiscard]] const PointResidencyServicePtr &
    pointResidency() const noexcept
    {
        return pointResidency_;
    }
    [[nodiscard]] std::size_t bindingCount() const noexcept;
    [[nodiscard]] std::size_t pointBindingCount() const noexcept;
    [[nodiscard]] std::size_t rasterBindingCount() const noexcept;

private:
    friend class SceneRuntime;

    explicit SceneRuntimeSnapshot(
        std::unordered_map<BindingGeneration, SceneRuntimeBinding> bindings,
        PointResidencyServicePtr residency);

    std::unordered_map<BindingGeneration, SceneRuntimeBinding> bindings_;
    PointResidencyServicePtr pointResidency_;
};

using SceneRuntimeSnapshotPtr = std::shared_ptr<const SceneRuntimeSnapshot>;

struct SceneRuntimeMetrics {
    DecodedCacheMetrics cache;
    PointCloudDataSourceMetrics source;
    HierarchyDecodeAdmissionMetrics decodeAdmission;
    std::uint64_t decodeRequestsQueued = 0;
    std::uint64_t decodeRequestsStarted = 0;
    std::uint64_t decodeRequestsCompleted = 0;
    std::uint64_t decodeRequestsCancelled = 0;
    std::uint64_t decodeRequestsFailed = 0;
    std::uint64_t hierarchicalLayers = 0;
    std::uint64_t retainedFlatBytes = 0;
    std::uint64_t persistentIndexBytes = 0;
    std::uint64_t localPersistentSources = 0;
    std::uint64_t reusedPersistentSources = 0;
    std::uint64_t activeColorTableBytes = 0;
    std::uint64_t flatDisplacedColorBytes = 0;
    std::uint64_t retainedSourceRootBytes = 0;
    std::uint64_t retainedColoredRootBytes = 0;
    PointMemoryBudgetMetrics memoryBudget;
    TaskSchedulerMetrics scheduler;
};

// Owns operational source handles independently of SceneDocument. Bindings
// are addressed by their non-reusable generation and validated against the
// source identity carried by document snapshots and asynchronous results.
class SceneRuntime final {
public:
    explicit SceneRuntime(
        std::uint64_t decodedByteBudget =
            HierarchyResidencyCoordinator::defaultByteBudget,
        std::size_t maximumConcurrentDecodes =
            HierarchyResidencyCoordinator::defaultMaximumConcurrentDecodes,
        HierarchyDecodeAdmissionPtr decodeAdmission = {},
        PointMemoryBudgetPtr memoryBudget = {},
        DecodedPageCachePtr initialCache = {});

    SceneRuntime(const SceneRuntime &) = delete;
    SceneRuntime &operator=(const SceneRuntime &) = delete;
    SceneRuntime(SceneRuntime &&) noexcept = default;
    SceneRuntime &operator=(SceneRuntime &&) noexcept = default;

    [[nodiscard]] bool attachPoint(PointRuntimeBinding binding);
    [[nodiscard]] bool stagePoint(PointRuntimeBinding binding);
    void preparePointAttachments();
    void preparePointDetachments(const SceneRuntime &previous);
    [[nodiscard]] bool stageDetach(BindingGeneration generation);
    void commitPointAttachments() noexcept;
    void resumePointRequests() noexcept;
    [[nodiscard]] bool attachRaster(RasterRuntimeBinding binding);
    // A private registry candidate sharing the existing residency service.
    // Overlay-only edits must not reattach or interrupt point datasets.
    [[nodiscard]] SceneRuntime copyBindings() const;

    [[nodiscard]] std::shared_ptr<PointDatasetRuntime>
    point(PointCloudSourceId sourceId, BindingGeneration generation) const;
    [[nodiscard]] RasterTileSourcePtr
    raster(RasterSourceId sourceId, BindingGeneration generation) const;

    [[nodiscard]] bool setPointActive(PointCloudSourceId sourceId,
                                      BindingGeneration generation,
                                      bool active);
    [[nodiscard]] bool
    setPointActive(std::span<const PointRuntimeActivityUpdate> updates);
    [[nodiscard]] bool detach(BindingGeneration generation);
    void clear();
    void swap(SceneRuntime &other) noexcept;

    [[nodiscard]] std::size_t bindingCount() const noexcept;
    [[nodiscard]] std::size_t pointBindingCount() const noexcept;
    [[nodiscard]] std::size_t rasterBindingCount() const noexcept;
    [[nodiscard]] SceneRuntimeSnapshotPtr snapshot() const;
    [[nodiscard]] std::uint64_t decodedByteBudget() const noexcept;
    [[nodiscard]] std::uint64_t decodedResidentBytes() const;
    [[nodiscard]] const HierarchyResidencyCoordinatorPtr &
    residencyCoordinator() const noexcept;
    [[nodiscard]] const PointMemoryBudgetPtr &memoryBudget() const noexcept;
    [[nodiscard]] const DecodedPageCachePtr &decodedPageCache() const noexcept;
    [[nodiscard]] const std::shared_ptr<TaskScheduler> &
    hierarchyScheduler() const noexcept;
    [[nodiscard]] SceneRuntimeMetrics metrics() const;
    void syncResidencyBudgets();

private:
    explicit SceneRuntime(PointResidencyServicePtr residency);
    [[nodiscard]] bool contains(PointCloudSourceId sourceId) const noexcept;
    [[nodiscard]] bool contains(RasterSourceId sourceId) const noexcept;
    void rebalanceHierarchyResidency();
    void
    fitHierarchyRoots(const std::shared_ptr<PointDatasetRuntime> &incoming = {},
                      bool restoreMinimumBudget = false);

    std::unordered_map<BindingGeneration, SceneRuntimeBinding> bindings_;
    mutable SceneRuntimeSnapshotPtr snapshotCache_;
    PointResidencyServicePtr pointResidency_;
    std::vector<std::pair<std::shared_ptr<PointDatasetRuntime>,
                          std::shared_ptr<PointDatasetAttachment>>>
        preparedPoints_;
};

using SceneRuntimePtr = std::shared_ptr<SceneRuntime>;

} // namespace pci
