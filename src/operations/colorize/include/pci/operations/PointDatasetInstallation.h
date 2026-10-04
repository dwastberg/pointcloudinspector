#pragma once

#include <pci/operations/PreparedPointDataset.h>
#include <pci/runtime/point/PointDatasetRuntime.h>

namespace pci {

[[nodiscard]] PointDatasetRuntimePtr createPointDatasetRuntime(
    const PreparedPointDatasetPtr &dataset,
    std::uint64_t decodedByteBudget = defaultDecodedCacheByteBudget);
void completePointDatasetRuntime(const PointDatasetRuntimePtr &runtime,
                                 const PreparedPointDatasetPtr &dataset);
void validatePointDatasetCompletion(const PointDatasetRuntimePtr &runtime,
                                    const PreparedPointDatasetPtr &dataset);

} // namespace pci
