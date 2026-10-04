#pragma once

#include <cstdint>

namespace pci {

struct SystemMemoryInfo {
    // Effective values already account for an enclosing process/container
    // limit when the platform exposes one.
    std::uint64_t totalPhysicalBytes = 0;
    std::uint64_t availablePhysicalBytes = 0;
};

// Returns a current snapshot. A zero field means the platform could not
// provide that value. The result must not be cached as available memory can
// change between admissions.
[[nodiscard]] SystemMemoryInfo systemMemoryInfo() noexcept;

} // namespace pci
