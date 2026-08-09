#include "app/MemoryBudgetPolicy.h"

#include "foundation/CheckedArithmetic.h"

#include <algorithm>
#include <limits>

namespace pci {
namespace {

constexpr std::uint64_t bytesPerMiB = std::uint64_t{1024} * 1024;
constexpr std::uint64_t bytesPerGiB = std::uint64_t{1024} * 1024 * 1024;
constexpr std::uint64_t fallbackPointBudget = std::uint64_t{1024} * bytesPerMiB;
constexpr std::uint64_t minimumAutomaticBudget =
    std::uint64_t{256} * bytesPerMiB;

std::uint64_t fractionTenths(const std::uint64_t value,
                             const std::uint64_t numerator) noexcept
{
    return (value / 10) * numerator + ((value % 10) * numerator) / 10;
}

std::uint64_t roundDownToMiB(const std::uint64_t bytes) noexcept
{
    return bytes / bytesPerMiB * bytesPerMiB;
}

} // namespace

AutomaticMemoryBudget automaticMemoryBudget(
    const SystemMemoryInfo &memory,
    const AutomaticMemoryBudgetParameters &parameters) noexcept
{
    AutomaticMemoryBudget result{
        .effectiveTotalBytes = memory.totalPhysicalBytes,
        .availableBytes = memory.availablePhysicalBytes,
    };
    if (memory.totalPhysicalBytes == 0 || memory.availablePhysicalBytes == 0) {
        result.pointByteBudget = fallbackPointBudget;
        result.usedFallback = true;
        return result;
    }

    // The platform's available-memory signal already excludes memory that
    // cannot be reclaimed safely (Linux MemAvailable, Windows standby/free,
    // or reusable Mach pages). Preserve an additional live reserve without
    // subtracting the much larger static 30% envelope a second time.
    result.systemReserveBytes =
        std::max(std::uint64_t{2} * bytesPerGiB,
                 fractionTenths(memory.totalPhysicalBytes, 1));
    // On small systems never reserve more than half the effective RAM solely
    // for the OS; the working reserve below remains independently protected.
    result.systemReserveBytes =
        std::min(result.systemReserveBytes, memory.totalPhysicalBytes / 2);
    result.workingReserveBytes =
        saturatingAdd(saturatingAdd(parameters.gpuByteBudget,
                                    parameters.activeDecodeByteBudget),
                      parameters.applicationReserveBytes);
    // Budget all three allocators or none: leaving the raster caches out here
    // produces a confident point budget that is wrong by their size.
    result.workingReserveBytes =
        saturatingAdd(result.workingReserveBytes,
                      saturatingAdd(parameters.rasterCpuByteBudget,
                                    parameters.gdalCacheByteBudget));

    const std::uint64_t processEnvelope =
        fractionTenths(memory.totalPhysicalBytes, 7);
    const std::uint64_t staticPointLimit =
        processEnvelope > result.workingReserveBytes
            ? processEnvelope - result.workingReserveBytes
            : 0;
    const std::uint64_t protectedAvailable =
        saturatingAdd(result.systemReserveBytes, result.workingReserveBytes);
    const std::uint64_t additionalHeadroom =
        memory.availablePhysicalBytes > protectedAvailable
            ? memory.availablePhysicalBytes - protectedAvailable
            : 0;
    const std::uint64_t livePointLimit =
        saturatingAdd(parameters.currentPointBytes, additionalHeadroom);

    std::uint64_t budget = std::min(staticPointLimit, livePointLimit);
    const std::uint64_t safeMinimum =
        std::min(minimumAutomaticBudget, staticPointLimit);
    budget = std::max(budget, safeMinimum);
    result.pointByteBudget = std::max<std::uint64_t>(roundDownToMiB(budget), 1);
    return result;
}

} // namespace pci
