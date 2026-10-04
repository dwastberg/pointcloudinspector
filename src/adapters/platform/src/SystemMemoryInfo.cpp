#include <pci/adapters/platform/SystemMemoryInfo.h>

#include <pci/foundation/CheckedArithmetic.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/host_info.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <filesystem>
#endif

namespace pci {
namespace {

#if defined(__linux__)
std::optional<std::uint64_t> parseUnsigned(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    std::uint64_t value = 0;
    const auto [next, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || next != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint64_t> readUnsignedFile(const std::filesystem::path &path)
{
    std::ifstream input(path);
    std::string text;
    if (!(input >> text) || text == "max") {
        return std::nullopt;
    }
    return parseUnsigned(text);
}

std::filesystem::path currentCgroupV2Directory()
{
    std::ifstream input("/proc/self/cgroup");
    std::string line;
    while (std::getline(input, line)) {
        constexpr std::string_view prefix = "0::";
        if (!std::string_view(line).starts_with(prefix)) {
            continue;
        }
        std::string_view relative(line);
        relative.remove_prefix(prefix.size());
        while (!relative.empty() && relative.front() == '/') {
            relative.remove_prefix(1);
        }
        return std::filesystem::path("/sys/fs/cgroup") /
               std::filesystem::path(relative);
    }
    return {};
}
#endif

} // namespace

SystemMemoryInfo systemMemoryInfo() noexcept
{
    SystemMemoryInfo result;
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = static_cast<DWORD>(sizeof(status));
    if (GlobalMemoryStatusEx(&status) != FALSE) {
        result.totalPhysicalBytes = status.ullTotalPhys;
        result.availablePhysicalBytes = status.ullAvailPhys;
        // Physical headroom is the preferred signal, but a tighter commit
        // limit is a real allocation boundary and must also be respected.
        if (status.ullAvailPageFile > 0) {
            result.availablePhysicalBytes =
                std::min(result.availablePhysicalBytes,
                         static_cast<std::uint64_t>(status.ullAvailPageFile));
        }
    }
#elif defined(__APPLE__)
    std::uint64_t totalBytes = 0;
    std::size_t totalSize = sizeof(totalBytes);
    if (sysctlbyname("hw.memsize", &totalBytes, &totalSize, nullptr, 0) == 0) {
        result.totalPhysicalBytes = totalBytes;
    } else {
        host_basic_info_data_t basic{};
        mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
        if (host_info(mach_host_self(),
                      HOST_BASIC_INFO,
                      reinterpret_cast<host_info_t>(&basic),
                      &count) == KERN_SUCCESS) {
            result.totalPhysicalBytes = basic.max_mem;
        }
    }

    vm_size_t pageSize = 0;
    vm_statistics64_data_t statistics{};
    mach_msg_type_number_t statisticsCount = HOST_VM_INFO64_COUNT;
    if (host_page_size(mach_host_self(), &pageSize) == KERN_SUCCESS &&
        host_statistics64(mach_host_self(),
                          HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&statistics),
                          &statisticsCount) == KERN_SUCCESS) {
        const std::uint64_t reusablePages =
            static_cast<std::uint64_t>(statistics.free_count) +
            static_cast<std::uint64_t>(statistics.inactive_count) +
            static_cast<std::uint64_t>(statistics.speculative_count);
        result.availablePhysicalBytes = saturatingMultiply(
            reusablePages, static_cast<std::uint64_t>(pageSize));
    }
#elif defined(__linux__)
    std::ifstream memory("/proc/meminfo");
    std::string line;
    while (std::getline(memory, line)) {
        std::istringstream fields(line);
        std::string key;
        std::uint64_t valueKiB = 0;
        if (!(fields >> key >> valueKiB)) {
            continue;
        }
        if (key == "MemTotal:") {
            result.totalPhysicalBytes =
                saturatingMultiply(valueKiB, std::uint64_t{1024});
        } else if (key == "MemAvailable:") {
            result.availablePhysicalBytes =
                saturatingMultiply(valueKiB, std::uint64_t{1024});
        }
    }

    const std::filesystem::path cgroup = currentCgroupV2Directory();
    if (!cgroup.empty()) {
        std::optional<std::uint64_t> limit =
            readUnsignedFile(cgroup / "memory.max");
        if (const auto high = readUnsignedFile(cgroup / "memory.high")) {
            limit = limit ? std::min(*limit, *high) : high;
        }
        const auto current = readUnsignedFile(cgroup / "memory.current");
        if (limit && *limit > 0) {
            result.totalPhysicalBytes =
                result.totalPhysicalBytes == 0
                    ? *limit
                    : std::min(result.totalPhysicalBytes, *limit);
            if (current) {
                const std::uint64_t cgroupAvailable =
                    *current < *limit ? *limit - *current : 0;
                result.availablePhysicalBytes =
                    result.availablePhysicalBytes == 0
                        ? cgroupAvailable
                        : std::min(result.availablePhysicalBytes,
                                   cgroupAvailable);
            }
        }
    }
#endif
    if (result.totalPhysicalBytes > 0) {
        result.availablePhysicalBytes =
            std::min(result.availablePhysicalBytes, result.totalPhysicalBytes);
    }
    return result;
}

} // namespace pci
