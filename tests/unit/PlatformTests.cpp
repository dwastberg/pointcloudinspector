#include <pci/adapters/platform/ProcessMemory.h>
#include <pci/adapters/platform/SystemMemoryInfo.h>

#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("process memory diagnostics report a coherent peak",
          "[unit][platform][metrics]")
{
    const pci::ProcessMemoryMetrics memory = pci::processMemoryMetrics();
    CHECK(memory.peakResidentBytes >= memory.residentBytes);
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    CHECK(memory.residentBytes > 0);
    CHECK(memory.peakResidentBytes > 0);
#endif
}

TEST_CASE("system memory diagnostics report coherent effective capacity",
          "[unit][platform][metrics][memory]")
{
    const pci::SystemMemoryInfo memory = pci::systemMemoryInfo();
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    CHECK(memory.totalPhysicalBytes > 0);
    CHECK(memory.availablePhysicalBytes > 0);
#endif
    if (memory.totalPhysicalBytes > 0) {
        CHECK(memory.availablePhysicalBytes <= memory.totalPhysicalBytes);
    }
}

} // namespace
