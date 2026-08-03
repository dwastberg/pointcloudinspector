#pragma once

#include "platform/SystemMemoryInfo.h"

#include <cstdint>

namespace pci {

struct AutomaticMemoryBudgetParameters {
    std::uint64_t gpuByteBudget = 512ULL * 1024 * 1024;
    std::uint64_t activeDecodeByteBudget = 256ULL * 1024 * 1024;
    std::uint64_t applicationReserveBytes = 512ULL * 1024 * 1024;
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
