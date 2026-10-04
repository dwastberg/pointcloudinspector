#pragma once

#include <pci/adapters/platform/SystemMemoryInfo.h>

#include <cstdint>

namespace pci {

struct AutomaticMemoryBudgetParameters {
    // On unified-memory platforms this must carry the point and raster GPU
    // budgets together, because both are drawn from the same physical pool.
    std::uint64_t gpuByteBudget = std::uint64_t{512} * 1024 * 1024;
    std::uint64_t activeDecodeByteBudget = std::uint64_t{256} * 1024 * 1024;
    std::uint64_t applicationReserveBytes = std::uint64_t{512} * 1024 * 1024;
    // Decoded raster tiles and GDAL's process-global block cache are separate
    // allocators from the point pages. Reserving them here is what stops
    // enabling rasters from raising the process envelope instead of lowering
    // the automatically chosen point budget.
    std::uint64_t rasterCpuByteBudget = 0;
    std::uint64_t gdalCacheByteBudget = 0;
    std::uint64_t currentPointBytes = 0;
};

struct AutomaticMemoryBudget {
    std::uint64_t pointByteBudget = 0;
    std::uint64_t effectiveTotalBytes = 0;
    std::uint64_t availableBytes = 0;
    std::uint64_t systemReserveBytes = 0;
    std::uint64_t workingReserveBytes = 0;
    bool usedFallback = false;
};

[[nodiscard]] AutomaticMemoryBudget automaticMemoryBudget(
    const SystemMemoryInfo &memory,
    const AutomaticMemoryBudgetParameters &parameters = {}) noexcept;

} // namespace pci
