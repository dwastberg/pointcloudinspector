#pragma once

#include <cstdint>

namespace pci {

inline constexpr std::uint64_t defaultDecodedCacheByteBudget =
    512ULL * 1024 * 1024;

struct DecodedCacheMetrics {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t insertions = 0;
    std::uint64_t replacements = 0;
    std::uint64_t evictions = 0;
    std::uint64_t residentBytes = 0;
    std::uint64_t peakResidentBytes = 0;
    std::uint64_t residentPoints = 0;
    std::uint64_t byteBudget = 0;
};

} // namespace pci
