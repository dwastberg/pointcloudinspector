#pragma once

#include <cstdint>

namespace pci {

struct ProcessMemoryMetrics {
    std::uint64_t residentBytes = 0;
    std::uint64_t peakResidentBytes = 0;
};

// Returns zero for values unavailable on an unsupported platform. The
// function is intended for low-frequency diagnostics, not per-draw sampling.
[[nodiscard]] ProcessMemoryMetrics processMemoryMetrics() noexcept;

} // namespace pci
