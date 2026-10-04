#include <pci/runtime/point/PointResidencyService.h>

#include <algorithm>
#include <utility>

namespace pci {

PointResidencyService::PointResidencyService(
    const std::uint64_t decodedByteBudget,
    const std::size_t maximumConcurrentDecodes,
    HierarchyDecodeAdmissionPtr decodeAdmission,
    PointMemoryBudgetPtr memoryBudget,
    DecodedPageCachePtr initialCache)
    : memoryBudget_(
          memoryBudget ? std::move(memoryBudget)
                       : std::make_shared<PointMemoryBudget>(decodedByteBudget))
    , coordinator_(std::make_shared<HierarchyResidencyCoordinator>(
          decodedByteBudget,
          maximumConcurrentDecodes,
          std::move(decodeAdmission),
          memoryBudget_))
    , cache_(initialCache ? std::move(initialCache)
                          : std::make_shared<DecodedPageCache>(
                                memoryBudget_->availableBytes()))
    , scheduler_(std::make_shared<TaskScheduler>(
          maximumConcurrentDecodes,
          std::max<std::uint64_t>(
              1,
              std::min<std::uint64_t>(TaskScheduler::defaultActiveByteBudget,
                                      decodedByteBudget))))
    , completionQueue_(
          std::make_shared<PointDecodeQueue>(memoryBudget_, cache_))
{
}

const PointMemoryBudgetPtr &PointResidencyService::memoryBudget() const noexcept
{
    return memoryBudget_;
}

const HierarchyResidencyCoordinatorPtr &
PointResidencyService::coordinator() const noexcept
{
    return coordinator_;
}

const DecodedPageCachePtr &PointResidencyService::cache() const noexcept
{
    return cache_;
}

const std::shared_ptr<TaskScheduler> &
PointResidencyService::scheduler() const noexcept
{
    return scheduler_;
}

std::uint64_t PointResidencyService::decodedByteBudget() const noexcept
{
    return coordinator_->byteBudget();
}

std::uint64_t PointResidencyService::decodedResidentBytes() const
{
    return cache_->residentBytes();
}

void PointResidencyService::syncCacheBudget()
{
    cache_->setByteBudget(
        std::max<std::uint64_t>(1, coordinator_->availableResidencyBytes()));
}

} // namespace pci
