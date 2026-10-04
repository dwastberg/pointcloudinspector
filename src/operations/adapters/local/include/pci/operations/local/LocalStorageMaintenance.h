#pragma once

#include <memory>
#include <pci/operations/StorageMaintenance.h>

namespace pci {
struct StorageMaintenanceLocations {
    std::filesystem::path pointCache;
    std::filesystem::path workingFiles;
    std::filesystem::path legacyPointCache;
    std::filesystem::path legacyTemporaryBase;
};
[[nodiscard]] std::shared_ptr<const StorageMaintenance>
makeLocalStorageMaintenance(StorageMaintenanceLocations locations);
} // namespace pci
