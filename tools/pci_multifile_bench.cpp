// pci_multifile_bench — qualify local multi-file admission, persistent paging,
// shared residency, request cancellation, and process-memory behavior.

#include "app/ApplicationOptions.h"
#include "import/PointCloudLoadController.h"
#include "import/pdal/PdalPointCloudLoader.h"
#include "platform/ProcessMemory.h"
#include "platform/QtPath.h"
#include "scene/SceneDocument.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<double, std::milli>;
constexpr std::uint64_t bytesPerMiB = std::uint64_t{1024} * 1024;

struct Options {
    std::uint64_t maximumPoints = 100'000'000;
    std::uint64_t cpuCacheMiB = 1024;
    std::uint64_t timeoutSeconds = 120;
    std::uint64_t revisitPasses = 2;
    std::uint64_t localPageThreshold = 1'000'000;
    std::filesystem::path cacheDirectory;
    std::filesystem::path jsonPath;
    std::vector<std::filesystem::path> files;
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
    void observe(const std::uint64_t value) noexcept
    {
        std::uint64_t current = peak_.load(std::memory_order_relaxed);
        while (value > current &&
               !peak_.compare_exchange_weak(
                   current, value, std::memory_order_relaxed)) {
        }
    }

    std::uint64_t baseline_ = 0;
    std::atomic_uint64_t peak_ = 0;
    std::jthread worker_;
};

void usage(FILE *stream)
{
    std::fprintf(
        stream,
        "usage: pci_multifile_bench [options] <local-las-or-laz...>\n"
        "  --max-points N             point cap per source (default "
        "100000000)\n"
        "  --cpu-cache-mb N           shared CPU payload budget (default "
        "1024)\n"
        "  --cache-dir PATH           persistent local-page cache directory\n"
        "  --local-page-threshold N   paging route threshold (default "
        "1000000)\n"
        "  --revisit-passes N         interleaved detail passes (default 2)\n"
        "  --timeout-seconds N        whole import timeout (default 120)\n"
        "  --json PATH                archive a machine-readable report\n");
}

bool parseNumber(const std::vector<std::string> &arguments,
                 std::size_t &index,
                 std::uint64_t &destination,
                 const char *name,
                 const bool allowZero = false)
{
    if (index + 1 >= arguments.size()) {
        std::fprintf(stderr, "%s requires a value\n", name);
        return false;
    }
    const std::string &text = arguments[++index];
    if (allowZero && text == "0") {
        destination = 0;
        return true;
    }
    const auto value = pci::parsePointCount(text);
    if (!value) {
        std::fprintf(stderr,
                     "%s must be %sa positive integer\n",
                     name,
                     allowZero ? "zero or " : "");
        return false;
    }
    destination = *value;
    return true;
}

std::optional<Options> parseOptions(const int argc, char **argv)
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
            if (!parseNumber(
                    arguments, index, options.maximumPoints, "--max-points")) {
                return std::nullopt;
            }
        } else if (argument == "--cpu-cache-mb") {
            if (!parseNumber(
                    arguments, index, options.cpuCacheMiB, "--cpu-cache-mb")) {
                return std::nullopt;
            }
        } else if (argument == "--timeout-seconds") {
            if (!parseNumber(arguments,
                             index,
                             options.timeoutSeconds,
                             "--timeout-seconds")) {
                return std::nullopt;
            }
        } else if (argument == "--revisit-passes") {
            if (!parseNumber(arguments,
                             index,
                             options.revisitPasses,
                             "--revisit-passes",
                             true)) {
                return std::nullopt;
            }
        } else if (argument == "--local-page-threshold") {
            if (!parseNumber(arguments,
                             index,
                             options.localPageThreshold,
                             "--local-page-threshold")) {
                return std::nullopt;
            }
        } else if (argument == "--cache-dir" || argument == "--json") {
            if (index + 1 >= arguments.size()) {
                std::fprintf(stderr, "%s requires a path\n", argument.c_str());
                return std::nullopt;
            }
            const std::filesystem::path path(arguments[++index]);
            if (argument == "--cache-dir") {
                options.cacheDirectory = path;
            } else {
                options.jsonPath = path;
            }
        } else if (argument.starts_with('-')) {
            std::fprintf(stderr, "unknown option: %s\n", argument.c_str());
            return std::nullopt;
        } else {
            options.files.emplace_back(argument);
        }
    }
    if (options.files.empty()) {
        usage(stderr);
        return std::nullopt;
    }
    if (options.cpuCacheMiB >
        std::numeric_limits<std::uint64_t>::max() / bytesPerMiB) {
        std::fprintf(stderr, "--cpu-cache-mb is too large\n");
        return std::nullopt;
    }
    if (options.cacheDirectory.empty()) {
        options.cacheDirectory = std::filesystem::temp_directory_path() /
                                 "pcinspector-multifile-bench" / "point-pages";
    }
    return options;
}

double elapsedMilliseconds(const Clock::time_point started)
{
    return std::chrono::duration_cast<Milliseconds>(Clock::now() - started)
        .count();
}

void writeReport(const std::filesystem::path &path, const QJsonObject &report)
{
    if (path.empty()) {
        return;
    }
    const QString filename = pci::pathToQString(path);
    QDir().mkpath(QFileInfo(filename).absolutePath());
    QFile output(filename);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("could not write qualification JSON");
    }
    output.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const std::optional<Options> parsed = parseOptions(argc, argv);
    if (!parsed) {
        return argc > 1 && (std::string_view(argv[1]) == "--help" ||
                            std::string_view(argv[1]) == "-h")
                   ? 0
                   : 2;
    }
    const Options &options = *parsed;
    const std::vector<std::filesystem::path> paths =
        pci::expandPathArguments(options.files);
    if (paths.empty()) {
        std::fprintf(stderr, "No files matched the given arguments.\n");
        return 2;
    }

    const std::uint64_t cpuBudget = options.cpuCacheMiB * bytesPerMiB;
    auto memoryBudget = std::make_shared<pci::PointMemoryBudget>(cpuBudget);
    auto document = std::make_shared<pci::SceneDocument>(
        cpuBudget,
        pci::HierarchyResidencyCoordinator::defaultMaximumConcurrentDecodes,
        pci::HierarchyDecodeAdmissionPtr{},
        memoryBudget);
    pci::TaskScheduler scheduler;
    pci::PointCloudLoadController controller(
        std::make_shared<pci::PdalPointCloudLoader>(), scheduler);
    ResidentMemorySampler memory;
    const auto started = Clock::now();
    std::vector<pci::PointCloudScenePtr> scenes;
    std::unordered_map<pci::LoadJobId, double> previewTimes;
    std::unordered_map<pci::LoadJobId, double> readyTimes;
    std::size_t terminal = 0;
    std::size_t failed = 0;
    std::size_t cancelled = 0;
    bool timedOut = false;

    QObject::connect(
        &controller,
        &pci::PointCloudLoadController::sceneReady,
        &application,
        [&](const pci::LoadJobId jobId, const pci::PointCloudScenePtr &scene) {
            previewTimes.try_emplace(jobId, elapsedMilliseconds(started));
            if (std::ranges::find(scenes, scene) == scenes.end()) {
                scenes.push_back(scene);
                static_cast<void>(document->addLayer(scene));
            }
        });
    QObject::connect(
        &controller,
        &pci::PointCloudLoadController::loaded,
        &application,
        [&](const pci::LoadJobId jobId, const pci::PointCloudScenePtr &scene) {
            if (std::ranges::find(scenes, scene) == scenes.end()) {
                scenes.push_back(scene);
                static_cast<void>(document->addLayer(scene));
            }
            readyTimes[jobId] = elapsedMilliseconds(started);
            if (++terminal == paths.size()) {
                application.quit();
            }
        });
    QObject::connect(&controller,
                     &pci::PointCloudLoadController::failed,
                     &application,
                     [&](const pci::LoadJobId, const QString &message) {
                         ++failed;
                         std::fprintf(stderr,
                                      "source failed: %s\n",
                                      message.toUtf8().constData());
                         if (++terminal == paths.size()) {
                             application.quit();
                         }
                     });
    QObject::connect(&controller,
                     &pci::PointCloudLoadController::cancelled,
                     &application,
                     [&](const pci::LoadJobId) {
                         ++cancelled;
                         if (++terminal == paths.size()) {
                             application.quit();
                         }
                     });
    QTimer::singleShot(
        std::chrono::seconds(options.timeoutSeconds), &application, [&] {
            timedOut = terminal != paths.size();
            if (timedOut) {
                controller.cancel();
            }
            application.quit();
        });

    std::vector<pci::PointCloudLoadRequest> requests;
    requests.reserve(paths.size());
    for (const std::filesystem::path &path : paths) {
        requests.push_back({
            .options =
                {
                    .sourcePath = path,
                    .maximumPoints = options.maximumPoints,
                    .localPaging =
                        {
                            .pointThreshold = options.localPageThreshold,
                            .cacheDirectory = options.cacheDirectory,
                        },
                },
            .resources =
                {
                    .decodedByteBudget = cpuBudget,
                    .residency = document->residencyCoordinator(),
                    .memoryBudget = memoryBudget,
                    .flatReservation = {},
                },
        });
    }
    const std::vector<pci::LoadJobId> jobIds =
        controller.loadBatch(std::move(requests));
    application.exec();
    const double importMilliseconds = elapsedMilliseconds(started);

    const auto requestStarted = Clock::now();
    for (std::uint64_t pass = 0; pass < options.revisitPasses; ++pass) {
        for (const pci::PointCloudScenePtr &scene : scenes) {
            if (!scene->hierarchical() || scene->rootNode().leaf) {
                continue;
            }
            const auto children = pci::childNodeIds(pci::rootPointCloudNode);
            scene->requestNodes(children);
            if (pass + 1U < options.revisitPasses) {
                // Camera replacement: obsolete requests should cancel or
                // coalesce rather than accumulating behind the new view.
                scene->requestNodes(std::span<const pci::PointCloudNodeId>(
                    children.data(), children.size() / 2U));
            }
        }
        document->hierarchyScheduler()->waitForIdle();
        for (const pci::PointCloudScenePtr &scene : scenes) {
            scene->trimDecodedCache();
        }
    }
    const double requestMilliseconds = elapsedMilliseconds(requestStarted);

    const pci::SceneDocumentMetrics documentMetrics =
        document->hierarchyMetrics();
    const pci::PointCloudLoadControllerMetrics loadMetrics =
        controller.metrics();
    const pci::ProcessMemoryMetrics finalMemory = pci::processMemoryMetrics();
    const double firstAny =
        previewTimes.empty()
            ? 0.0
            : std::ranges::min(previewTimes | std::views::values);
    double firstAll = 0.0;
    for (const auto &[jobId, value] : previewTimes) {
        static_cast<void>(jobId);
        firstAll = std::max(firstAll, value);
    }
    std::uint64_t sourceFileBytes = 0;
    for (const std::filesystem::path &path : paths) {
        std::error_code error;
        const std::uintmax_t bytes = std::filesystem::file_size(path, error);
        if (!error) {
            sourceFileBytes += static_cast<std::uint64_t>(bytes);
        }
    }
    const bool bounded =
        documentMetrics.cache.residentBytes <= cpuBudget &&
        documentMetrics.memoryBudget.reservedBytes <= cpuBudget &&
        documentMetrics.scheduler.active == 0 &&
        documentMetrics.scheduler.pending == 0;
    const std::string status = timedOut        ? "timeout"
                               : failed > 0    ? "partial_failure"
                               : cancelled > 0 ? "cancelled"
                               : bounded       ? "ok"
                                               : "unbounded";

    QJsonArray sourceReports;
    for (const pci::LoadJobId jobId : jobIds) {
        const auto state = controller.jobState(jobId);
        QJsonObject source;
        source.insert("job_id", static_cast<qint64>(jobId.value()));
        if (state) {
            source.insert("path", pci::pathToQString(state->sourcePath));
            source.insert("preview_available", state->previewAvailable);
            source.insert("local_paging", state->localPaging);
        }
        if (previewTimes.contains(jobId)) {
            source.insert("preview_ms", previewTimes[jobId]);
        }
        if (readyTimes.contains(jobId)) {
            source.insert("ready_ms", readyTimes[jobId]);
        }
        sourceReports.append(source);
    }
    QJsonObject report{
        {"schema", QStringLiteral("pcinspector.release-h.v1")},
        {"status", QString::fromStdString(status)},
        {"source_count", static_cast<qint64>(paths.size())},
        {"source_file_bytes", static_cast<qint64>(sourceFileBytes)},
        {"first_any_preview_ms", firstAny},
        {"first_all_preview_ms",
         previewTimes.size() == paths.size() ? firstAll : 0.0},
        {"import_ms", importMilliseconds},
        {"request_pass_ms", requestMilliseconds},
        {"persistent_index_bytes",
         static_cast<qint64>(documentMetrics.persistentIndexBytes)},
        {"persistent_sources",
         static_cast<qint64>(documentMetrics.localPersistentSources)},
        {"reused_persistent_sources",
         static_cast<qint64>(documentMetrics.reusedPersistentSources)},
        {"cpu_budget_bytes", static_cast<qint64>(cpuBudget)},
        {"decoded_resident_bytes",
         static_cast<qint64>(documentMetrics.cache.residentBytes)},
        {"decoded_peak_bytes",
         static_cast<qint64>(documentMetrics.cache.peakResidentBytes)},
        {"cache_hits", static_cast<qint64>(documentMetrics.cache.hits)},
        {"cache_misses", static_cast<qint64>(documentMetrics.cache.misses)},
        {"cache_evictions",
         static_cast<qint64>(documentMetrics.cache.evictions)},
        {"decode_cancelled",
         static_cast<qint64>(documentMetrics.decodeRequestsCancelled)},
        {"import_peak_active",
         static_cast<qint64>(loadMetrics.scheduler.peakActive)},
        {"import_peak_pending",
         static_cast<qint64>(loadMetrics.scheduler.peakPending)},
        {"import_peak_active_bytes",
         static_cast<qint64>(loadMetrics.scheduler.peakActiveEstimatedBytes)},
        {"rss_baseline_bytes", static_cast<qint64>(memory.baseline())},
        {"rss_peak_bytes", static_cast<qint64>(memory.peak())},
        {"rss_final_bytes", static_cast<qint64>(finalMemory.residentBytes)},
        {"bounded", bounded},
        {"sources", sourceReports},
    };
    try {
        writeReport(options.jsonPath, report);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }

    std::printf(
        "MULTIFILE_RESULT status=%s sources=%zu previews=%zu "
        "first_any_ms=%.1f first_all_ms=%.1f import_ms=%.1f "
        "request_ms=%.1f index_bytes=%llu reused=%llu "
        "cpu_resident=%llu cpu_peak=%llu rss_peak=%llu "
        "cache_hits=%llu cache_misses=%llu cache_evictions=%llu "
        "decode_cancelled=%llu import_peak_active=%zu "
        "import_peak_pending=%zu bounded=%d\n",
        status.c_str(),
        paths.size(),
        previewTimes.size(),
        firstAny,
        previewTimes.size() == paths.size() ? firstAll : 0.0,
        importMilliseconds,
        requestMilliseconds,
        static_cast<unsigned long long>(documentMetrics.persistentIndexBytes),
        static_cast<unsigned long long>(
            documentMetrics.reusedPersistentSources),
        static_cast<unsigned long long>(documentMetrics.cache.residentBytes),
        static_cast<unsigned long long>(
            documentMetrics.cache.peakResidentBytes),
        static_cast<unsigned long long>(memory.peak()),
        static_cast<unsigned long long>(documentMetrics.cache.hits),
        static_cast<unsigned long long>(documentMetrics.cache.misses),
        static_cast<unsigned long long>(documentMetrics.cache.evictions),
        static_cast<unsigned long long>(
            documentMetrics.decodeRequestsCancelled),
        loadMetrics.scheduler.peakActive,
        loadMetrics.scheduler.peakPending,
        bounded ? 1 : 0);
    return status == "ok" ? 0 : 1;
}
