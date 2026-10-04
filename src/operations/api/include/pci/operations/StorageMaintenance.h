#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace pci {
enum class StorageCategory {
    PointCache,
    WorkingFiles,
    Legacy
};
enum class StorageMaintenanceAction {
    Scan,
    CleanUnused,
    CleanLegacy
};
struct StorageUsage {
    std::uint64_t totalBytes = 0;
    std::uint64_t reclaimableBytes = 0;
    std::uint64_t protectedBytes = 0;
    std::uint64_t unverifiedBytes = 0;
    std::uint64_t reclaimableEntries = 0;
    std::uint64_t unverifiedEntries = 0;
    std::vector<std::filesystem::path> locations;
};
struct StorageMaintenanceResult {
    std::array<StorageUsage, 3> usage;
    std::uint64_t removedBytes = 0;
    std::uint64_t removedEntries = 0;
    std::uint64_t errorCount = 0;
    // Bounded samples; errorCount includes failures beyond these samples.
    std::vector<std::string> errors;
    bool cancelled = false;
};
class StorageMaintenance {
public:
    using Progress = std::function<void(std::uint64_t)>;
    virtual ~StorageMaintenance() = default;
    // CleanLegacy may only be requested after the UI's explicit confirmation.
    // Results describe remaining storage after completed deletions.
    [[nodiscard]] virtual StorageMaintenanceResult
    run(StorageMaintenanceAction action,
        std::stop_token stop,
        const Progress &progress) const = 0;
};
} // namespace pci
