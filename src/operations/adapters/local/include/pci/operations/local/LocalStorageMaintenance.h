#pragma once

#include <memory>
#include <pci/operations/StorageMaintenance.h>

namespace pci {
class LocalPageCacheContext;
struct StorageMaintenanceLocations {
    std::filesystem::path pointCache;
    std::filesystem::path workingFiles;
    std::filesystem::path legacyPointCache;
    std::filesystem::path legacyTemporaryBase;
};
[[nodiscard]] std::shared_ptr<const StorageMaintenance>
makeLocalStorageMaintenance(StorageMaintenanceLocations locations);
// Snapshot session publications when run, after all publishers have stopped.
[[nodiscard]] std::shared_ptr<const StorageMaintenance>
makeSessionPointCacheMaintenance(
    std::shared_ptr<const LocalPageCacheContext> cache);
} // namespace pci
