// pci_residency_bench — exercise local hierarchical sources beyond cache
// capacity and report cache, decoder, cancellation, and process-memory peaks.

#include <pci/adapters/pdal/PdalPointCloudLoader.h>
#include <pci/adapters/platform/ProcessMemory.h>
#include <pci/desktop/config/ApplicationOptions.h>
#include <pci/document/SceneDocument.h>
#include <pci/operations/PointCloudImport.h>
#include <pci/operations/PointDatasetInstallation.h>
#include <pci/runtime/scene/SceneRuntime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<double, std::milli>;

constexpr std::uint64_t bytesPerMiB = std::uint64_t{1024} * 1024;

struct Options {
    std::uint64_t maximumPoints = 100'000'000;
    std::uint64_t cpuCacheMiB = 64;
    std::uint64_t stressNodes = 160;
    std::uint64_t passes = 2;
    std::uint64_t timeoutSeconds = 30;
    std::uint64_t minimumWorkingSetMultiple = 0;
    std::vector<std::filesystem::path> fileArguments;
};

struct StressNode {
    pci::PointCloudNodeId id;
    std::uint64_t bytes = 0;
};

struct StressResult {
    std::string status = "ok";
    std::uint64_t cacheBudget = 0;
    std::uint64_t workingSetBytes = 0;
    std::uint64_t largestNodeBytes = 0;
    std::uint64_t discoveredNodes = 0;
    std::uint64_t discoveryQueries = 0;
    std::uint64_t revisitRequests = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t cacheMisses = 0;
    std::uint64_t cacheEvictions = 0;
    std::uint64_t residentBytes = 0;
    std::uint64_t peakCacheBytes = 0;
    std::uint64_t peakDecoderBytes = 0;
    std::uint64_t sourceRequests = 0;
    std::uint64_t sourceCompleted = 0;
    std::uint64_t sourceCancelled = 0;
    std::uint64_t sourceFailed = 0;
    std::uint64_t sourcePointsVisited = 0;
    std::uint64_t decodedBytesProduced = 0;
    std::uint64_t processBaselineBytes = 0;
    std::uint64_t processPeakBytes = 0;
    std::uint64_t processFinalBytes = 0;
    std::uint64_t processHighWaterBytes = 0;
    double loadMilliseconds = 0.0;
    double stressMilliseconds = 0.0;
    double cancellationMilliseconds = 0.0;
    bool cancellationObserved = false;
    bool bounded = false;
};

class ResidentMemorySampler {
public:
    ResidentMemorySampler()
        : baseline_(pci::processMemoryMetrics().residentBytes)
        , peak_(baseline_)
        , worker_([this](const std::stop_token stopToken) {
            while (!stopToken.stop_requested()) {
                observe(pci::processMemoryMetrics().residentBytes);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            observe(pci::processMemoryMetrics().residentBytes);
        })
    {
    }

    ~ResidentMemorySampler()
    {
        worker_.request_stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] std::uint64_t baseline() const noexcept
    {
        return baseline_;
    }

    [[nodiscard]] std::uint64_t peak() const noexcept
    {
        return peak_.load(std::memory_order_relaxed);
    }

private:
    void observe(const std::uint64_t bytes) noexcept
    {
        std::uint64_t current = peak_.load(std::memory_order_relaxed);
        while (bytes > current &&
               !peak_.compare_exchange_weak(
                   current, bytes, std::memory_order_relaxed)) {
        }
    }

    std::uint64_t baseline_ = 0;
    std::atomic_uint64_t peak_ = 0;
    std::jthread worker_;
};

[[nodiscard]] double mebibytes(const std::uint64_t bytes) noexcept
{
    return static_cast<double>(bytes) / static_cast<double>(bytesPerMiB);
}

[[nodiscard]] double ratio(const std::uint64_t numerator,
                           const std::uint64_t denominator) noexcept
{
    return denominator > 0 ? static_cast<double>(numerator) /
                                 static_cast<double>(denominator)
                           : 0.0;
}

void usage(FILE *stream)
{
    std::fprintf(
        stream,
        "usage: pci_residency_bench [options] <local-copc-or-ept...>\n"
        "  --max-points N                 hierarchy point cap (default "
        "100000000)\n"
        "  --cpu-cache-mb N               decoded cache budget (default 64)\n"
        "  --stress-nodes N               non-empty nodes to discover (default "
        "160)\n"
        "  --passes N                     revisit passes (default 2)\n"
        "  --timeout-seconds N            timeout per node (default 30)\n"
        "  --minimum-working-set-multiple N\n"
        "                                 fail unless decoded work is Nx "
        "budget\n");
}

[[nodiscard]] bool
parsePositiveOption(const std::vector<std::string> &arguments,
                    std::size_t &index,
                    std::uint64_t &destination,
                    const char *name)
{
    if (index + 1 >= arguments.size()) {
        std::fprintf(stderr, "%s requires a value\n", name);
        return false;
    }
    const auto value = pci::parsePointCount(arguments[index + 1]);
    if (!value) {
        std::fprintf(stderr, "%s must be a positive integer\n", name);
        return false;
    }
    destination = *value;
    ++index;
    return true;
}

[[nodiscard]] std::optional<Options> parseOptions(const int argc, char **argv)
{
    Options options;
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string &argument = arguments[index];
        if (argument == "--help" || argument == "-h") {
            usage(stdout);
            return std::nullopt;
        }
        if (argument == "--max-points") {
            if (!parsePositiveOption(
                    arguments, index, options.maximumPoints, "--max-points")) {
                return std::nullopt;
            }
        } else if (argument == "--cpu-cache-mb") {
            if (!parsePositiveOption(
                    arguments, index, options.cpuCacheMiB, "--cpu-cache-mb")) {
                return std::nullopt;
            }
        } else if (argument == "--stress-nodes") {
            if (!parsePositiveOption(
                    arguments, index, options.stressNodes, "--stress-nodes")) {
                return std::nullopt;
            }
        } else if (argument == "--passes") {
            if (!parsePositiveOption(
                    arguments, index, options.passes, "--passes")) {
                return std::nullopt;
            }
        } else if (argument == "--timeout-seconds") {
            if (!parsePositiveOption(arguments,
                                     index,
                                     options.timeoutSeconds,
                                     "--timeout-seconds")) {
                return std::nullopt;
            }
        } else if (argument == "--minimum-working-set-multiple") {
            if (!parsePositiveOption(arguments,
                                     index,
                                     options.minimumWorkingSetMultiple,
                                     "--minimum-working-set-multiple")) {
                return std::nullopt;
            }
        } else if (argument.starts_with('-')) {
            std::fprintf(stderr, "unknown option: %s\n", argument.c_str());
            return std::nullopt;
        } else {
            options.fileArguments.emplace_back(argument);
        }
    }
    if (options.fileArguments.empty()) {
        usage(stderr);
        return std::nullopt;
    }
    if (options.cpuCacheMiB >
        std::numeric_limits<std::uint64_t>::max() / bytesPerMiB) {
        std::fprintf(stderr, "--cpu-cache-mb is too large\n");
        return std::nullopt;
    }
    return options;
}

[[nodiscard]] pci::PointCloudNodePayloadPtr
waitForNode(const pci::PointDatasetRuntimePtr &scene,
            const pci::PointCloudNodeId id,
            const std::chrono::seconds timeout)
{
    if (pci::PointCloudNodePayloadPtr payload = scene->nodePayload(id)) {
        return payload;
    }
    std::uint64_t observedRevision = scene->revision();
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        scene->drainDecodeCompletions();
        const std::uint64_t revision = scene->revision();
        if (revision != observedRevision) {
            observedRevision = revision;
            if (pci::PointCloudNodePayloadPtr payload =
                    scene->nodePayload(id)) {
                return payload;
            }
            if (!scene->hierarchyError().empty()) {
                throw std::runtime_error(scene->hierarchyError());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("timed out waiting for hierarchy node");
}

[[nodiscard]] bool waitForMetric(const pci::PointDatasetRuntimePtr &scene,
                                 const std::chrono::seconds timeout,
                                 const auto &ready)
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        scene->drainDecodeCompletions();
        if (ready(scene->hierarchyMetrics())) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ready(scene->hierarchyMetrics());
}

[[nodiscard]] std::vector<StressNode>
discoverNodes(const pci::PointDatasetRuntimePtr &scene,
              const std::uint64_t targetCount,
              const std::chrono::seconds timeout,
              std::uint64_t &queryCount)
{
    std::vector<StressNode> result;
    std::deque<pci::PointCloudNodeId> pending;
    if (!scene->rootNode().leaf) {
        const auto children = pci::childNodeIds(pci::rootPointCloudNode);
        pending.insert(pending.end(), children.begin(), children.end());
    }
    const std::uint64_t queryLimit = std::max<std::uint64_t>(
        64,
        targetCount > std::numeric_limits<std::uint64_t>::max() / 16
            ? std::numeric_limits<std::uint64_t>::max()
            : targetCount * 16);
    while (!pending.empty() && result.size() < targetCount &&
           queryCount < queryLimit) {
        const pci::PointCloudNodeId id = pending.front();
        pending.pop_front();
        ++queryCount;
        scene->requestNodes(std::array{id});
        pci::PointCloudNodePayloadPtr payload = waitForNode(scene, id, timeout);
        const pci::PointCloudNode descriptor = scene->node(id);
        if (payload->sourcePointCount > 0) {
            const std::uint64_t bytes =
                pci::pointCloudNodePayloadBytes(*payload);
            if (bytes > 0) {
                result.push_back({.id = id, .bytes = bytes});
            }
            if (!descriptor.leaf) {
                const auto children = pci::childNodeIds(id);
                pending.insert(pending.end(), children.begin(), children.end());
            }
        }
        payload.reset();
        scene->trimDecodedCache();
    }
    return result;
}

void exerciseRevisits(const pci::PointDatasetRuntimePtr &scene,
                      const std::span<const StressNode> nodes,
                      const std::uint64_t passes,
                      const std::chrono::seconds timeout)
{
    for (std::uint64_t pass = 0; pass < passes; ++pass) {
        if (pass % 2 == 0) {
            for (const StressNode &node : nodes) {
                scene->requestNodes(std::array{node.id});
                auto lease = waitForNode(scene, node.id, timeout);
                lease.reset();
                scene->trimDecodedCache();
            }
        } else {
            for (auto node = nodes.rbegin(); node != nodes.rend(); ++node) {
                scene->requestNodes(std::array{node->id});
                auto lease = waitForNode(scene, node->id, timeout);
                lease.reset();
                scene->trimDecodedCache();
            }
        }
    }
}

void measureCancellation(StressResult &result,
                         const pci::PointDatasetRuntimePtr &scene,
                         const std::span<const StressNode> nodes,
                         const std::chrono::seconds timeout)
{
    if (nodes.size() < 2) {
        return;
    }
    std::optional<pci::PointCloudNodeId> evicted;
    for (const StressNode &node : nodes) {
        if (!scene->nodePayload(node.id)) {
            evicted = node.id;
            break;
        }
    }
    if (!evicted) {
        return;
    }

    pci::PointCloudNodeId replacement = nodes.front().id;
    for (const StressNode &node : nodes) {
        if (node.id != *evicted && scene->nodePayload(node.id)) {
            replacement = node.id;
            break;
        }
    }
    if (replacement == *evicted) {
        return;
    }

    const pci::PointDatasetRuntimeMetrics before = scene->hierarchyMetrics();
    scene->requestNodes(std::array{*evicted});
    const bool started = waitForMetric(
        scene,
        timeout,
        [&before](const pci::PointDatasetRuntimeMetrics &metrics) {
            return metrics.decodeRequestsStarted > before.decodeRequestsStarted;
        });
    if (!started) {
        return;
    }
    const auto cancellationStarted = Clock::now();
    scene->requestNodes(std::array{replacement});
    const auto observationTimeout = std::min(timeout, std::chrono::seconds(2));
    result.cancellationObserved = waitForMetric(
        scene,
        observationTimeout,
        [&before](const pci::PointDatasetRuntimeMetrics &metrics) {
            return metrics.decodeRequestsCancelled >
                   before.decodeRequestsCancelled;
        });
    result.cancellationMilliseconds = std::chrono::duration_cast<Milliseconds>(
                                          Clock::now() - cancellationStarted)
                                          .count();
    if (scene->decodeInFlight()) {
        static_cast<void>(waitForNode(scene, replacement, timeout));
    }
}

[[nodiscard]] StressResult runStress(const std::filesystem::path &path,
                                     const Options &options)
{
    StressResult result;
    result.cacheBudget = options.cpuCacheMiB * bytesPerMiB;
    ResidentMemorySampler memory;
    result.processBaselineBytes = memory.baseline();

    pci::SceneRuntime runtime(result.cacheBudget, 1);
    auto document = std::make_shared<pci::SceneDocument>();
    const auto loadStarted = Clock::now();
    const pci::PointCloudLoadOptions loadOptions{
        .sourcePath = path,
        .maximumPoints = options.maximumPoints,
        .localPaging = {},
    };
    const pci::PointCloudLoadResources loadResources{
        .decodedByteBudget = result.cacheBudget,
        .residency = runtime.residencyCoordinator(),
        .memoryBudget = {},
        .flatReservation = {},
    };
    const pci::PointCloudLoadContext loadContext;
    const pci::PdalPointCloudLoader loader;
    const pci::PointCloudImportPreflight preflight = loader.inspect(
        loadOptions, loadResources.decodedByteBudget, loadContext.stopToken);
    pci::PointDatasetRuntimePtr scene = pci::createPointDatasetRuntime(
        loader.load(loadOptions, loadResources, preflight, loadContext),
        loadResources.decodedByteBudget);
    result.loadMilliseconds =
        std::chrono::duration_cast<Milliseconds>(Clock::now() - loadStarted)
            .count();
    const pci::BindingGeneration binding =
        pci::nextGeneration(document->lastBindingGeneration());
    if (!runtime.attachPoint({.descriptor = scene->descriptor(),
                              .runtime = scene,
                              .generation = binding})) {
        throw std::logic_error("point runtime attachment failed");
    }
    try {
        static_cast<void>(document->addLayer(scene->datasetView(), binding));
    } catch (...) {
        static_cast<void>(runtime.detach(binding));
        throw;
    }
    if (!scene->hierarchical()) {
        result.status = "not_hierarchical";
        return result;
    }
    if (scene->rootNode().leaf) {
        result.status = "insufficient_hierarchy";
    }

    const auto stressStarted = Clock::now();
    std::vector<StressNode> nodes =
        discoverNodes(scene,
                      options.stressNodes,
                      std::chrono::seconds(options.timeoutSeconds),
                      result.discoveryQueries);
    result.discoveredNodes = nodes.size();
    for (const StressNode &node : nodes) {
        result.largestNodeBytes = std::max(result.largestNodeBytes, node.bytes);
        result.workingSetBytes =
            node.bytes > std::numeric_limits<std::uint64_t>::max() -
                             result.workingSetBytes
                ? std::numeric_limits<std::uint64_t>::max()
                : result.workingSetBytes + node.bytes;
    }

    const pci::PointDatasetRuntimeMetrics beforeRevisit =
        scene->hierarchyMetrics();
    exerciseRevisits(scene,
                     nodes,
                     options.passes,
                     std::chrono::seconds(options.timeoutSeconds));
    const pci::PointDatasetRuntimeMetrics afterRevisit =
        scene->hierarchyMetrics();
    result.revisitRequests =
        afterRevisit.source.requests - beforeRevisit.source.requests;
    measureCancellation(
        result, scene, nodes, std::chrono::seconds(options.timeoutSeconds));
    result.stressMilliseconds =
        std::chrono::duration_cast<Milliseconds>(Clock::now() - stressStarted)
            .count();

    scene->trimDecodedCache();
    const pci::PointDatasetRuntimeMetrics sceneMetrics =
        scene->hierarchyMetrics();
    const pci::SceneRuntimeMetrics documentMetrics = runtime.metrics();
    result.cacheHits = sceneMetrics.cache.hits;
    result.cacheMisses = sceneMetrics.cache.misses;
    result.cacheEvictions = sceneMetrics.cache.evictions;
    result.residentBytes = sceneMetrics.cache.residentBytes;
    result.peakCacheBytes = sceneMetrics.cache.peakResidentBytes;
    result.peakDecoderBytes =
        documentMetrics.decodeAdmission.peakActiveEstimatedBytes;
    result.sourceRequests = sceneMetrics.source.requests;
    result.sourceCompleted = sceneMetrics.source.completed;
    result.sourceCancelled = sceneMetrics.source.cancelled;
    result.sourceFailed = sceneMetrics.source.failed;
    result.sourcePointsVisited = sceneMetrics.source.sourcePointsVisited;
    result.decodedBytesProduced = sceneMetrics.source.decodedBytesProduced;
    const pci::ProcessMemoryMetrics finalMemory = pci::processMemoryMetrics();
    result.processFinalBytes = finalMemory.residentBytes;
    result.processPeakBytes = std::max(memory.peak(), result.processFinalBytes);
    result.processHighWaterBytes = finalMemory.peakResidentBytes;
    const std::uint64_t cachePeakAllowance =
        result.largestNodeBytes >
                std::numeric_limits<std::uint64_t>::max() - result.cacheBudget
            ? std::numeric_limits<std::uint64_t>::max()
            : result.cacheBudget + result.largestNodeBytes;
    result.bounded = result.residentBytes <= result.cacheBudget &&
                     result.peakCacheBytes <= cachePeakAllowance &&
                     documentMetrics.decodeAdmission.active == 0 &&
                     documentMetrics.decodeAdmission.pending == 0;

    const double workingSetMultiple =
        ratio(result.workingSetBytes, result.cacheBudget);
    if (!result.bounded) {
        result.status = "unbounded";
    } else if (options.minimumWorkingSetMultiple > 0 &&
               workingSetMultiple <
                   static_cast<double>(options.minimumWorkingSetMultiple)) {
        result.status = "insufficient_working_set";
    } else if (nodes.empty()) {
        result.status = "insufficient_hierarchy";
    } else {
        result.status = "ok";
    }
    return result;
}

void printResult(const std::filesystem::path &path, const StressResult &result)
{
    const double workingSetMultiple =
        ratio(result.workingSetBytes, result.cacheBudget);
    std::printf("\n== Residency stress: %s ==\n", path.string().c_str());
    std::printf("  status                 : %s\n", result.status.c_str());
    std::printf("  root load              : %.3f ms\n",
                result.loadMilliseconds);
    std::printf("  stress elapsed         : %.3f ms\n",
                result.stressMilliseconds);
    std::printf("  discovered/query nodes : %llu / %llu\n",
                static_cast<unsigned long long>(result.discoveredNodes),
                static_cast<unsigned long long>(result.discoveryQueries));
    std::printf("  cache working set      : %.1f MiB (%.2fx budget)\n",
                mebibytes(result.workingSetBytes),
                workingSetMultiple);
    std::printf("  largest decoded node   : %.1f MiB\n",
                mebibytes(result.largestNodeBytes));
    std::printf("  cache current/peak     : %.1f / %.1f MiB, budget %.1f MiB "
                "(+ one insertion)\n",
                mebibytes(result.residentBytes),
                mebibytes(result.peakCacheBytes),
                mebibytes(result.cacheBudget));
    std::printf("  cache hit/miss/evict   : %llu / %llu / %llu\n",
                static_cast<unsigned long long>(result.cacheHits),
                static_cast<unsigned long long>(result.cacheMisses),
                static_cast<unsigned long long>(result.cacheEvictions));
    std::printf("  source requests        : %llu (%llu complete, %llu "
                "cancelled, %llu failed)\n",
                static_cast<unsigned long long>(result.sourceRequests),
                static_cast<unsigned long long>(result.sourceCompleted),
                static_cast<unsigned long long>(result.sourceCancelled),
                static_cast<unsigned long long>(result.sourceFailed));
    std::printf("  revisit source queries : %llu\n",
                static_cast<unsigned long long>(result.revisitRequests));
    std::printf("  source points visited  : %llu\n",
                static_cast<unsigned long long>(result.sourcePointsVisited));
    std::printf("  decoded bytes produced : %.1f MiB\n",
                mebibytes(result.decodedBytesProduced));
    std::printf("  peak decoder allowance : %.1f MiB\n",
                mebibytes(result.peakDecoderBytes));
    std::printf("  cancellation latency   : %s",
                result.cancellationObserved ? "" : "not observed");
    if (result.cancellationObserved) {
        std::printf("%.3f ms", result.cancellationMilliseconds);
    }
    std::printf("\n");
    std::printf("  process RSS            : %.1f MiB baseline, %.1f MiB "
                "sampled peak, %.1f MiB final, %.1f MiB OS high-water\n",
                mebibytes(result.processBaselineBytes),
                mebibytes(result.processPeakBytes),
                mebibytes(result.processFinalBytes),
                mebibytes(result.processHighWaterBytes));
    std::printf(
        "RESIDENCY_RESULT status=%s file=%s working_set_multiple=%.3f "
        "bounded=%u discovered_nodes=%llu resident_bytes=%llu "
        "cache_budget_bytes=%llu peak_cache_bytes=%llu "
        "largest_node_bytes=%llu peak_decoder_bytes=%llu "
        "cache_evictions=%llu revisit_requests=%llu source_requests=%llu "
        "source_completed=%llu source_cancelled=%llu source_failed=%llu "
        "decoded_bytes=%llu cancellation_observed=%u cancellation_ms=%.3f "
        "rss_baseline_bytes=%llu rss_peak_bytes=%llu rss_final_bytes=%llu "
        "rss_high_water_bytes=%llu\n",
        result.status.c_str(),
        path.filename().string().c_str(),
        workingSetMultiple,
        result.bounded ? 1U : 0U,
        static_cast<unsigned long long>(result.discoveredNodes),
        static_cast<unsigned long long>(result.residentBytes),
        static_cast<unsigned long long>(result.cacheBudget),
        static_cast<unsigned long long>(result.peakCacheBytes),
        static_cast<unsigned long long>(result.largestNodeBytes),
        static_cast<unsigned long long>(result.peakDecoderBytes),
        static_cast<unsigned long long>(result.cacheEvictions),
        static_cast<unsigned long long>(result.revisitRequests),
        static_cast<unsigned long long>(result.sourceRequests),
        static_cast<unsigned long long>(result.sourceCompleted),
        static_cast<unsigned long long>(result.sourceCancelled),
        static_cast<unsigned long long>(result.sourceFailed),
        static_cast<unsigned long long>(result.decodedBytesProduced),
        result.cancellationObserved ? 1U : 0U,
        result.cancellationMilliseconds,
        static_cast<unsigned long long>(result.processBaselineBytes),
        static_cast<unsigned long long>(result.processPeakBytes),
        static_cast<unsigned long long>(result.processFinalBytes),
        static_cast<unsigned long long>(result.processHighWaterBytes));
}

} // namespace

int main(int argc, char **argv)
{
    const std::optional<Options> options = parseOptions(argc, argv);
    if (!options) {
        return argc > 1 && (std::string_view(argv[1]) == "--help" ||
                            std::string_view(argv[1]) == "-h")
                   ? 0
                   : 2;
    }
    const std::vector<std::filesystem::path> paths =
        pci::expandPathArguments(options->fileArguments);
    if (paths.empty()) {
        std::fprintf(stderr, "No files matched the given arguments.\n");
        return 2;
    }

    bool qualificationFailed = false;
    for (const std::filesystem::path &path : paths) {
        try {
            const StressResult result = runStress(path, *options);
            printResult(path, result);
            qualificationFailed = qualificationFailed ||
                                  result.status == "unbounded" ||
                                  (options->minimumWorkingSetMultiple > 0 &&
                                   result.status != "ok");
        } catch (const std::exception &error) {
            std::fprintf(stderr,
                         "residency stress failed for '%s': %s\n",
                         path.string().c_str(),
                         error.what());
            return 1;
        }
    }
    return qualificationFailed ? 3 : 0;
}
