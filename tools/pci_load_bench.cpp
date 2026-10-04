// pci_load_bench — measure LAZ/LAS/COPC decode cost and how well it
// parallelizes.
//
// Decodes each given file once sequentially, then again with a bounded pool of
// worker threads, and reports per-file and aggregate timings plus the observed
// speedup. Build with optimizations (Release / -O3); Debug numbers are not
// representative of decode performance.
//
// Usage: pci_load_bench [--max-points N] <files-or-globs...>

#include <pci/adapters/pdal/PdalPointCloudLoader.h>
#include <pci/adapters/platform/ProcessMemory.h>
#include <pci/desktop/config/ApplicationOptions.h>
#include <pci/operations/PointCloudImport.h>
#include <pci/operations/PointDatasetInstallation.h>
#include <pci/pointcloud/GpuPoint.h>
#include <pci/runtime/point/PointDatasetRuntime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <future>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

struct LoadResult {
    std::string name;
    std::uint64_t sourcePoints = 0;
    std::uint64_t loadedPoints = 0;
    double seconds = 0.0;
};

pci::PointDatasetRuntimePtr loadOne(const std::filesystem::path &path,
                                    const std::uint64_t maxPoints)
{
    pci::PointCloudLoadOptions options{
        .sourcePath = path,
        .maximumPoints = maxPoints,
        .localPaging = {},
    };
    // This tool measures the retained-flat PDAL decode path. Persistent local
    // page construction/reuse is measured separately from this baseline.
    options.localPaging.pointThreshold =
        std::numeric_limits<std::uint64_t>::max();
    const pci::PointCloudLoadResources resources;
    const pci::PointCloudLoadContext context{
        .stopToken = {},
        .progress = [](pci::PointCloudImportProgress) {},

    };
    const pci::PdalPointCloudLoader loader;
    const pci::PointCloudImportPreflight preflight =
        loader.inspect(options, resources.decodedByteBudget, context.stopToken);
    return pci::createPointDatasetRuntime(
        loader.load(options, resources, preflight, context));
}

double megaPointsPerSecond(const std::uint64_t points, const double seconds)
{
    return seconds > 0.0 ? static_cast<double>(points) / seconds / 1.0e6 : 0.0;
}

double mibPerSecond(const std::uint64_t points, const double seconds)
{
    const double bytes = static_cast<double>(points) * sizeof(pci::GpuPoint);
    return seconds > 0.0 ? bytes / seconds / (1024.0 * 1024.0) : 0.0;
}

double mebibytes(const std::uint64_t bytes)
{
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

} // namespace

int main(int argc, char **argv)
{
    const std::vector<std::string> args(argv + 1, argv + argc);
    std::uint64_t maxPoints = std::uint64_t{10'000'000};
    std::vector<std::filesystem::path> fileArgs;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--max-points" && i + 1 < args.size()) {
            if (const auto value = pci::parsePointCount(args[i + 1])) {
                maxPoints = *value;
            }
            ++i;
        } else {
            fileArgs.emplace_back(args[i]);
        }
    }
    if (fileArgs.empty()) {
        std::fprintf(
            stderr,
            "usage: pci_load_bench [--max-points N] <files-or-globs...>\n");
        return 2;
    }

    const std::vector<std::filesystem::path> paths =
        pci::expandPathArguments(fileArgs);
    if (paths.empty()) {
        std::fprintf(stderr, "No files matched the given arguments.\n");
        return 2;
    }

    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const unsigned cap =
        static_cast<unsigned>(std::min<std::size_t>(paths.size(), hardware));
    std::printf("Files: %zu   hardware_concurrency: %u   parallel cap: %u   "
                "max-points/file: %llu\n",
                paths.size(),
                hardware,
                cap,
                static_cast<unsigned long long>(maxPoints));
    std::printf(
        "(warm-cache CPU decode; a cold disk would be IO-bound instead)\n\n");
    const pci::ProcessMemoryMetrics baselineMemory =
        pci::processMemoryMetrics();

    // Warm-up: prime PDAL's global plugin registry and the OS file cache with
    // the first file so both timed passes measure CPU decode, not cold IO.
    try {
        static_cast<void>(loadOne(paths.front(), maxPoints));
    } catch (const std::exception &error) {
        std::fprintf(stderr, "warm-up load failed: %s\n", error.what());
        return 1;
    }
    // Sequential pass.
    std::vector<LoadResult> sequential(paths.size());
    double sequentialTotal = 0.0;
    try {
        for (std::size_t i = 0; i < paths.size(); ++i) {
            const auto start = Clock::now();
            const pci::PointDatasetRuntimePtr scene =
                loadOne(paths[i], maxPoints);
            const double seconds =
                std::chrono::duration_cast<Seconds>(Clock::now() - start)
                    .count();
            sequential[i] = {
                .name = paths[i].filename().string(),
                .sourcePoints = scene->metadata().sourcePointCount,
                .loadedPoints = scene->decodedResidentPoints(),
                .seconds = seconds,
            };
            sequentialTotal += seconds;
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "sequential load failed: %s\n", error.what());
        return 1;
    }
    const pci::ProcessMemoryMetrics sequentialMemory =
        pci::processMemoryMetrics();

    std::printf("== Sequential ==\n");
    for (const LoadResult &result : sequential) {
        std::printf("  %-30s %11llu pts  %7.3fs  %7.1f Mpts/s  %8.1f MiB/s\n",
                    result.name.c_str(),
                    static_cast<unsigned long long>(result.loadedPoints),
                    result.seconds,
                    megaPointsPerSecond(result.loadedPoints, result.seconds),
                    mibPerSecond(result.loadedPoints, result.seconds));
    }
    std::printf("  ----\n  sequential decode total: %.3fs\n\n",
                sequentialTotal);

    // Parallel pass: a fixed pool of `cap` workers pulls files off a shared
    // index, mirroring a bounded thread-pool fan-out.
    std::vector<double> parallelSeconds(paths.size(), 0.0);
    std::atomic<std::size_t> nextIndex{0};
    std::atomic<bool> failed{false};
    std::string firstError;
    std::mutex errorMutex;
    const auto parallelStart = Clock::now();
    {
        std::vector<std::future<void>> workers;
        workers.reserve(cap);
        for (unsigned worker = 0; worker < cap; ++worker) {
            workers.push_back(std::async(std::launch::async, [&] {
                for (;;) {
                    const std::size_t i =
                        nextIndex.fetch_add(1, std::memory_order_relaxed);
                    if (i >= paths.size() || failed.load()) {
                        break;
                    }
                    try {
                        const auto start = Clock::now();
                        const pci::PointDatasetRuntimePtr scene =
                            loadOne(paths[i], maxPoints);
                        parallelSeconds[i] =
                            std::chrono::duration_cast<Seconds>(Clock::now() -
                                                                start)
                                .count();
                        static_cast<void>(scene);
                    } catch (const std::exception &error) {
                        const std::scoped_lock lock(errorMutex);
                        if (!failed.exchange(true)) {
                            firstError = error.what();
                        }
                    }
                }
            }));
        }
        for (std::future<void> &future : workers) {
            future.get();
        }
    }
    if (failed.load()) {
        std::fprintf(stderr, "parallel load failed: %s\n", firstError.c_str());
        return 1;
    }
    const double parallelTotal =
        std::chrono::duration_cast<Seconds>(Clock::now() - parallelStart)
            .count();
    const pci::ProcessMemoryMetrics parallelMemory =
        pci::processMemoryMetrics();

    std::printf("== Parallel (cap %u) ==\n", cap);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        std::printf("  %-30s %7.3fs (in-worker)\n",
                    sequential[i].name.c_str(),
                    parallelSeconds[i]);
    }
    std::printf("  ----\n  parallel decode wall time: %.3fs\n\n",
                parallelTotal);

    std::uint64_t totalPoints = 0;
    for (const LoadResult &result : sequential) {
        totalPoints += result.loadedPoints;
    }
    const double speedup =
        parallelTotal > 0.0 ? sequentialTotal / parallelTotal : 0.0;

    std::printf("== Summary ==\n");
    std::printf("  total loaded points : %llu\n",
                static_cast<unsigned long long>(totalPoints));
    std::printf("  sequential decode   : %.3fs\n", sequentialTotal);
    std::printf(
        "  parallel decode     : %.3fs  (cap %u)\n", parallelTotal, cap);
    std::printf(
        "  decode speedup      : %.2fx  (ideal ceiling %u)\n", speedup, cap);
    std::printf("  aggregate throughput: %.1f Mpts/s parallel vs %.1f Mpts/s "
                "sequential\n",
                megaPointsPerSecond(totalPoints, parallelTotal),
                megaPointsPerSecond(totalPoints, sequentialTotal));
    std::printf("  resident memory     : %.1f MiB baseline, %.1f MiB after "
                "sequential, %.1f MiB after parallel\n",
                mebibytes(baselineMemory.residentBytes),
                mebibytes(sequentialMemory.residentBytes),
                mebibytes(parallelMemory.residentBytes));
    std::printf("  peak process RSS    : %.1f MiB after sequential, %.1f MiB "
                "whole run\n",
                mebibytes(sequentialMemory.peakResidentBytes),
                mebibytes(parallelMemory.peakResidentBytes));
    return 0;
}
