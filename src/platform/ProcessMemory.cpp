#include "platform/ProcessMemory.h"

#include <algorithm>

#if defined(_WIN32)
#include <psapi.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>

#include <fstream>
#endif

namespace pci {

ProcessMemoryMetrics processMemoryMetrics() noexcept
{
    ProcessMemoryMetrics result;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(), &counters, sizeof(counters)) != FALSE) {
        result.residentBytes = counters.WorkingSetSize;
        result.peakResidentBytes = counters.PeakWorkingSetSize;
    }
#elif defined(__APPLE__)
    mach_task_basic_info_data_t taskInfo{};
    mach_msg_type_number_t taskInfoCount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(),
                  MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&taskInfo),
                  &taskInfoCount) == KERN_SUCCESS) {
        result.residentBytes = taskInfo.resident_size;
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        result.peakResidentBytes = static_cast<std::uint64_t>(usage.ru_maxrss);
    }
#elif defined(__linux__)
    std::ifstream statistics("/proc/self/statm");
    std::uint64_t totalPages = 0;
    std::uint64_t residentPages = 0;
    if (statistics >> totalPages >> residentPages) {
        static_cast<void>(totalPages);
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize > 0) {
            result.residentBytes =
                residentPages * static_cast<std::uint64_t>(pageSize);
        }
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss > 0) {
        result.peakResidentBytes =
            static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
    }
#endif
    result.peakResidentBytes =
        std::max(result.peakResidentBytes, result.residentBytes);
    return result;
}

} // namespace pci
