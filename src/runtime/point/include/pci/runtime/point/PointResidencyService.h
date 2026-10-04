#pragma once

#include <pci/runtime/point/DecodedPageCache.h>
#include <pci/runtime/point/PointDecodeQueue.h>

#include <pci/runtime/HierarchyResidencyCoordinator.h>
#include <pci/runtime/PointMemoryBudget.h>
#include <pci/tasking/TaskScheduler.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace pci {

// Owns the shared cache, memory admission, decode admission, and worker
// resources used by point datasets attached to one scene runtime.
class PointResidencyService final {
public:
    explicit PointResidencyService(
        std::uint64_t decodedByteBudget =
            HierarchyResidencyCoordinator::defaultByteBudget,
        std::size_t maximumConcurrentDecodes =
            HierarchyResidencyCoordinator::defaultMaximumConcurrentDecodes,
        HierarchyDecodeAdmissionPtr decodeAdmission = {},
        PointMemoryBudgetPtr memoryBudget = {},
        DecodedPageCachePtr initialCache = {});

    [[nodiscard]] const PointMemoryBudgetPtr &memoryBudget() const noexcept;
    [[nodiscard]] const HierarchyResidencyCoordinatorPtr &
    coordinator() const noexcept;
    [[nodiscard]] const DecodedPageCachePtr &cache() const noexcept;
    [[nodiscard]] const std::shared_ptr<TaskScheduler> &
    scheduler() const noexcept;

    [[nodiscard]] std::uint64_t decodedByteBudget() const noexcept;
    [[nodiscard]] std::uint64_t decodedResidentBytes() const;
    void syncCacheBudget();
    [[nodiscard]] const PointDecodeQueuePtr &completionQueue() const noexcept
    {
        return completionQueue_;
    }

private:
    PointMemoryBudgetPtr memoryBudget_;
    HierarchyResidencyCoordinatorPtr coordinator_;
    DecodedPageCachePtr cache_;
    std::shared_ptr<TaskScheduler> scheduler_;
    PointDecodeQueuePtr completionQueue_;
};

using PointResidencyServicePtr = std::shared_ptr<PointResidencyService>;

} // namespace pci
