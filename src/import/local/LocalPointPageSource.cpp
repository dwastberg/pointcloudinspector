#include "import/local/LocalPointPageSource.h"

#include "foundation/CheckedArithmetic.h"
#include "scene/BlockPartitioner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pci {

struct LocalPointPageSourceState {
    ~LocalPointPageSourceState()
    {
        if (!leasePath.empty()) {
            std::error_code ignored;
            std::filesystem::remove(leasePath, ignored);
        }
    }

    PointCloudMetadata metadata;
    LocalPointSourceFingerprint fingerprint{};
    std::filesystem::path payloadPath;
    std::filesystem::path storeDirectory;
    std::filesystem::path leasePath;
    std::uint8_t maximumLevel = 0;
    std::uint32_t pointsPerLeaf = 0;
    mutable std::mutex mutex;
    mutable std::condition_variable_any changed;
    std::unordered_map<PointCloudNodeId,
                       LocalPointPageRecord,
                       PointCloudNodeIdHash>
        pages;
    PointCloudScalarRanges scalarRanges;
    bool complete = false;
    bool committed = false;
    bool reused = false;
    std::string failure;

    mutable std::atomic_uint64_t requests = 0;
    mutable std::atomic_uint64_t completed = 0;
    mutable std::atomic_uint64_t cancelled = 0;
    mutable std::atomic_uint64_t failed = 0;
    mutable std::atomic_uint64_t estimatedDecodedBytesRequested = 0;
    mutable std::atomic_uint64_t decodedBytesProduced = 0;
    mutable std::atomic_uint64_t sourcePointsVisited = 0;
    mutable std::atomic_uint64_t decodedPointsProduced = 0;
    mutable std::atomic_uint64_t totalQueryNanoseconds = 0;
    mutable std::atomic_uint64_t fetchedBytes = 0;
};

namespace {

std::uint64_t
directoryUsageBytes(const std::filesystem::path &directory) noexcept
{
    std::uint64_t result = 0;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error) && !error) {
            const std::uintmax_t bytes = iterator->file_size(error);
            if (!error) {
                result =
                    saturatingAdd(result, static_cast<std::uint64_t>(bytes));
            }
        }
        iterator.increment(error);
    }
    return result;
}

void activateLease(LocalPointPageSourceState &state)
{
    static std::atomic_uint64_t sequence = 0;
    const std::string name = state.storeDirectory.filename().string() +
                             ".lease-" +
                             std::to_string(localPointCurrentProcessId()) +
                             "-" + std::to_string(sequence.fetch_add(1));
    state.leasePath = state.storeDirectory.parent_path() / name;
    std::ofstream lease(state.leasePath, std::ios::trunc);
    if (!lease) {
        state.leasePath.clear();
        throw std::runtime_error(
            "could not create local page cache usage lease");
    }
}

void saturatedAtomicAdd(std::atomic_uint64_t &destination,
                        const std::uint64_t value) noexcept
{
    std::uint64_t current = destination.load(std::memory_order_relaxed);
    while (true) {
        const std::uint64_t next = saturatingAdd(current, value);
        if (destination.compare_exchange_weak(current,
                                              next,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
            return;
        }
    }
}

// Probes stay out of the Qt-free import layer's dependencies: read the same
// PCI_PROBE_RESIDENCY switch the renderer uses, without linking QtCore here.
bool environmentVariableExists(const char *name) noexcept
{
#ifdef _WIN32
    char *value = nullptr;
    std::size_t valueSize = 0;
    if (_dupenv_s(&value, &valueSize, name) != 0) {
        return false;
    }
    const bool exists = value != nullptr;
    std::free(value);
    return exists;
#else
    return std::getenv(name) != nullptr;
#endif
}

bool probeResidencyEnabled() noexcept
{
    static const bool enabled =
        environmentVariableExists("PCI_PROBE_RESIDENCY");
    return enabled;
}

std::uint64_t estimatedDecodedBytes(const std::uint64_t points) noexcept
{
    constexpr std::uint64_t bytesPerPoint =
        sizeof(GpuPoint) + sizeof(PointAttributes);
    return saturatingMultiply(points, bytesPerPoint);
}

std::uint64_t estimatedGpuBytes(const std::uint64_t points) noexcept
{
    return saturatingMultiply(points,
                              static_cast<std::uint64_t>(sizeof(GpuPoint)));
}

std::vector<std::byte> readPayload(const std::filesystem::path &path,
                                   const LocalPointPageRecord &record)
{
    if (record.payloadBytes > std::numeric_limits<std::size_t>::max() ||
        record.payloadBytes >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("local point page is too large to decode");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open local point payload: " +
                                 path.string());
    }
    input.seekg(static_cast<std::streamoff>(record.payloadOffset));
    std::vector<std::byte> bytes(static_cast<std::size_t>(record.payloadBytes));
    input.read(reinterpret_cast<char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::runtime_error("could not read complete local point page");
    }
    if (localPointCrc32(bytes) != record.payloadChecksum) {
        throw std::runtime_error("local point page checksum mismatch");
    }
    return bytes;
}

} // namespace

LocalPointPageSource::LocalPointPageSource(
    std::shared_ptr<LocalPointPageSourceState> state,
    const std::uint64_t maximumPoints)
    : state_(std::move(state))
    , maximumLevel_(state_->maximumLevel)
{
    if (maximumPoints == 0) {
        throw std::invalid_argument(
            "local point page maximumPoints must be positive");
    }
    if (maximumPoints < state_->metadata.sourcePointCount) {
        std::uint64_t represented = 0;
        {
            const std::scoped_lock lock(state_->mutex);
            const auto root = state_->pages.find(rootPointCloudNode);
            represented = root == state_->pages.end() ? state_->pointsPerLeaf
                                                      : root->second.pointCount;
        }
        maximumLevel_ = 0;
        while (maximumLevel_ < state_->maximumLevel &&
               represented < maximumPoints) {
            ++maximumLevel_;
            represented = saturatingMultiply(represented, std::uint64_t{8});
        }
    }
}

std::shared_ptr<LocalPointPageSource> LocalPointPageSource::createBuilding(
    PointCloudMetadata metadata,
    std::filesystem::path payloadPath,
    std::filesystem::path storeDirectory,
    const LocalPointSourceFingerprint fingerprint,
    const std::uint8_t maximumLevel,
    const std::uint32_t pointsPerLeaf,
    const std::uint64_t maximumPoints)
{
    auto state = std::make_shared<LocalPointPageSourceState>();
    state->metadata = std::move(metadata);
    state->fingerprint = fingerprint;
    state->payloadPath = std::move(payloadPath);
    state->storeDirectory = std::move(storeDirectory);
    state->maximumLevel = maximumLevel;
    state->pointsPerLeaf = pointsPerLeaf;
    return std::shared_ptr<LocalPointPageSource>(
        new LocalPointPageSource(std::move(state), maximumPoints));
}

std::shared_ptr<LocalPointPageSource> LocalPointPageSource::openCommitted(
    const std::filesystem::path &storeDirectory,
    const LocalPointSourceFingerprint &fingerprint,
    const std::uint64_t maximumPoints)
{
    LocalPointPageManifest manifest =
        readLocalPointManifest(storeDirectory / "manifest.pci", fingerprint);
    const std::filesystem::path payloadPath = storeDirectory / "payload.bin";
    std::error_code error;
    const std::uintmax_t payloadBytes =
        std::filesystem::file_size(payloadPath, error);
    if (error || payloadBytes != manifest.payloadFileBytes) {
        throw std::runtime_error("local point payload size mismatch");
    }

    auto state = std::make_shared<LocalPointPageSourceState>();
    if (manifest.metadata.hasIntensity) {
        state->scalarRanges.intensity = PointScalarRange{
            static_cast<double>(manifest.scalarRanges.intensityMinimum),
            static_cast<double>(manifest.scalarRanges.intensityMaximum),
        };
    }
    state->metadata = std::move(manifest.metadata);
    state->fingerprint = fingerprint;
    state->payloadPath = payloadPath;
    state->storeDirectory = storeDirectory;
    state->maximumLevel = manifest.maximumLevel;
    state->pointsPerLeaf = manifest.pointsPerLeaf;
    state->complete = true;
    state->committed = true;
    state->reused = true;
    for (LocalPointPageRecord &record : manifest.pages) {
        state->pages.emplace(record.id, record);
    }
    activateLease(*state);
    return std::shared_ptr<LocalPointPageSource>(
        new LocalPointPageSource(std::move(state), maximumPoints));
}

PointCloudNode LocalPointPageSource::rootNode() const
{
    return node(rootPointCloudNode);
}

PointCloudNode LocalPointPageSource::node(const PointCloudNodeId id) const
{
    const std::uint64_t cells = std::uint64_t{1} << id.level;
    if (id.level > maximumLevel_ || id.x >= cells || id.y >= cells ||
        id.z >= cells) {
        throw std::out_of_range("local point node is outside the hierarchy");
    }

    std::optional<LocalPointPageRecord> page;
    {
        const std::scoped_lock lock(state_->mutex);
        if (const auto found = state_->pages.find(id);
            found != state_->pages.end()) {
            page = found->second;
        }
    }
    std::uint64_t spatialCells = 1;
    for (std::uint8_t level = 0; level < id.level; ++level) {
        spatialCells = saturatingMultiply(spatialCells, std::uint64_t{8});
    }
    const double extent =
        std::max(state_->metadata.sourceBounds.maximumExtent(), 1e-9);
    return {
        .id = id,
        .bounds = page ? page->tightBounds : state_->metadata.sourceBounds,
        .geometricError =
            extent / std::sqrt(static_cast<double>(state_->pointsPerLeaf)) /
            static_cast<double>(std::uint64_t{1} << id.level),
        .estimatedPointCount =
            page ? page->pointCount
                 : std::min<std::uint64_t>(
                       state_->pointsPerLeaf,
                       (state_->metadata.sourcePointCount + spatialCells - 1U) /
                           spatialCells),
        .leaf = id.level == maximumLevel_,
    };
}

PointCloudNodePayloadPtr
LocalPointPageSource::loadNode(const PointCloudNodeId id,
                               const std::stop_token stopToken) const
{
    const PointCloudNode descriptor = node(id);
    state_->requests.fetch_add(1, std::memory_order_relaxed);
    saturatedAtomicAdd(state_->estimatedDecodedBytesRequested,
                       estimatedDecodedBytes(descriptor.estimatedPointCount));
    const auto started = std::chrono::steady_clock::now();
    const auto recordDuration = [this, started] {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started);
        saturatedAtomicAdd(state_->totalQueryNanoseconds,
                           static_cast<std::uint64_t>(elapsed.count()));
    };

    try {
        LocalPointPageRecord record;
        std::filesystem::path payloadPath;
        {
            std::unique_lock lock(state_->mutex);
            const bool present = state_->pages.contains(id) ||
                                 state_->complete || !state_->failure.empty();
            const bool ready = state_->changed.wait(lock, stopToken, [&] {
                return state_->pages.contains(id) || state_->complete ||
                       !state_->failure.empty();
            });
            if (probeResidencyEnabled() && !present) {
                // This call parked a caller thread until the page was built.
                // Decode workers are a fixed, tiny pool, so a long park here
                // starves every other source's decodes.
                std::fprintf(
                    stderr,
                    "[probe] page-wait store=%s node=%u/%u/%u/%u waited_ms=%.1f"
                    " outcome=%s\n",
                    state_->storeDirectory.filename().string().c_str(),
                    static_cast<unsigned>(id.level),
                    static_cast<unsigned>(id.x),
                    static_cast<unsigned>(id.y),
                    static_cast<unsigned>(id.z),
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started)
                        .count(),
                    ready ? "ready" : "cancelled");
            }
            if (!ready || stopToken.stop_requested()) {
                throw PointCloudDataSourceCancelled();
            }
            if (!state_->failure.empty()) {
                throw std::runtime_error(state_->failure);
            }
            const auto found = state_->pages.find(id);
            if (found == state_->pages.end()) {
                auto empty = std::make_shared<PointCloudNodePayload>();
                empty->nodeId = id;
                state_->completed.fetch_add(1, std::memory_order_relaxed);
                recordDuration();
                return empty;
            }
            record = found->second;
            payloadPath = state_->payloadPath;
        }

        const std::vector<std::byte> bytes = readPayload(payloadPath, record);
        auto payload = std::make_shared<PointCloudNodePayload>();
        payload->nodeId = id;
        payload->sourcePointCount = record.sourcePointCount;
        BlockPartitioner partitioner(
            record.tightBounds,
            record.pointCount,
            [&payload](PointBlockPtr block) {
                payload->blocks.push_back(std::move(block));
            },
            record.pointCount);
        for (std::size_t offset = 0; offset < bytes.size();
             offset += localPointDiskBytes) {
            if (stopToken.stop_requested()) {
                throw PointCloudDataSourceCancelled();
            }
            partitioner.add(decodeLocalPoint(
                std::span<const std::byte, localPointDiskBytes>(
                    bytes.data() + offset, localPointDiskBytes)));
        }
        partitioner.finish();
        const std::uint64_t decodedPoints =
            pointCloudNodePayloadPoints(*payload);
        state_->completed.fetch_add(1, std::memory_order_relaxed);
        saturatedAtomicAdd(state_->fetchedBytes, record.payloadBytes);
        saturatedAtomicAdd(state_->sourcePointsVisited, record.pointCount);
        saturatedAtomicAdd(state_->decodedPointsProduced, decodedPoints);
        saturatedAtomicAdd(state_->decodedBytesProduced,
                           pointCloudNodePayloadBytes(*payload));
        recordDuration();
        return payload;
    } catch (const PointCloudDataSourceCancelled &) {
        state_->cancelled.fetch_add(1, std::memory_order_relaxed);
        recordDuration();
        throw;
    } catch (const std::exception &error) {
        state_->failed.fetch_add(1, std::memory_order_relaxed);
        recordDuration();
        throw std::runtime_error("Could not load local point page from '" +
                                 state_->metadata.sourcePath.string() +
                                 "': " + error.what());
    }
}

PointCloudDataSourceMetrics LocalPointPageSource::metrics() const
{
    return {
        .requests = state_->requests.load(std::memory_order_relaxed),
        .completed = state_->completed.load(std::memory_order_relaxed),
        .cancelled = state_->cancelled.load(std::memory_order_relaxed),
        .failed = state_->failed.load(std::memory_order_relaxed),
        .estimatedDecodedBytesRequested =
            state_->estimatedDecodedBytesRequested.load(
                std::memory_order_relaxed),
        .decodedBytesProduced =
            state_->decodedBytesProduced.load(std::memory_order_relaxed),
        .sourcePointsVisited =
            state_->sourcePointsVisited.load(std::memory_order_relaxed),
        .decodedPointsProduced =
            state_->decodedPointsProduced.load(std::memory_order_relaxed),
        .totalQueryNanoseconds =
            state_->totalQueryNanoseconds.load(std::memory_order_relaxed),
        .fetchedBytes = state_->fetchedBytes.load(std::memory_order_relaxed),
        .fetchedBytesKnown = true,
    };
}

PointCloudStorageMetrics LocalPointPageSource::storageMetrics() const
{
    std::filesystem::path directory;
    bool committed = false;
    bool reused = false;
    {
        const std::scoped_lock lock(state_->mutex);
        directory = state_->storeDirectory;
        committed = state_->committed;
        reused = state_->reused;
    }
    return {
        .persistentBytes = directoryUsageBytes(directory),
        .localPersistent = true,
        .committed = committed,
        .reused = reused,
    };
}

PointCloudScalarRanges LocalPointPageSource::scalarRanges() const
{
    const std::scoped_lock lock(state_->mutex);
    return state_->scalarRanges;
}

std::optional<PointCloudFullDetailInfo>
LocalPointPageSource::fullDetailInfo() const
{
    PointCloudFullDetailInfo result;
    {
        const std::scoped_lock lock(state_->mutex);
        if (!state_->complete || !state_->committed ||
            !state_->failure.empty()) {
            return std::nullopt;
        }
        result.leafNodes.reserve(state_->pages.size());
        for (const auto &[id, page] : state_->pages) {
            if (id.level != maximumLevel_) {
                continue;
            }
            result.leafNodes.push_back(id);
            result.pointCount =
                saturatingAdd(result.pointCount, page.pointCount);
        }
    }
    if (result.leafNodes.empty()) {
        return std::nullopt;
    }
    std::ranges::sort(result.leafNodes);
    result.decodedBytes = estimatedDecodedBytes(result.pointCount);
    result.gpuBytes = estimatedGpuBytes(result.pointCount);
    return result;
}

std::optional<std::vector<PointCloudStoredNode>>
LocalPointPageSource::storedNodeIndex() const
{
    std::vector<PointCloudStoredNode> result;
    {
        const std::scoped_lock lock(state_->mutex);
        if (!state_->complete || !state_->committed ||
            !state_->failure.empty() || state_->pages.empty()) {
            return std::nullopt;
        }
        result.reserve(state_->pages.size());
        for (const auto &[id, page] : state_->pages) {
            result.push_back({
                .id = id,
                .bounds = page.tightBounds,
                .pointCount = page.pointCount,
                .localityKey = page.payloadOffset,
            });
        }
    }
    std::ranges::sort(result,
                      [](const PointCloudStoredNode &left,
                         const PointCloudStoredNode &right) {
                          if (left.localityKey != right.localityKey) {
                              return left.localityKey < right.localityKey;
                          }
                          return left.id < right.id;
                      });
    return result;
}

std::uint8_t LocalPointPageSource::maximumLevel() const noexcept
{
    return maximumLevel_;
}

std::filesystem::path LocalPointPageSource::storeDirectory() const
{
    const std::scoped_lock lock(state_->mutex);
    return state_->storeDirectory;
}

bool LocalPointPageSource::committed() const noexcept
{
    const std::scoped_lock lock(state_->mutex);
    return state_->committed;
}

void LocalPointPageSource::publish(LocalPointPageRecord record)
{
    {
        const std::scoped_lock lock(state_->mutex);
        state_->pages.insert_or_assign(record.id, record);
    }
    state_->changed.notify_all();
}

void LocalPointPageSource::setScalarRanges(PointCloudScalarRanges ranges)
{
    {
        const std::scoped_lock lock(state_->mutex);
        state_->scalarRanges = ranges;
    }
    state_->changed.notify_all();
}

void LocalPointPageSource::finishCommit(
    std::filesystem::path finalStoreDirectory)
{
    {
        const std::scoped_lock lock(state_->mutex);
        state_->storeDirectory = std::move(finalStoreDirectory);
        state_->payloadPath = state_->storeDirectory / "payload.bin";
        activateLease(*state_);
        state_->complete = true;
        state_->committed = true;
    }
    state_->changed.notify_all();
}

void LocalPointPageSource::fail(std::string message) noexcept
{
    try {
        {
            const std::scoped_lock lock(state_->mutex);
            state_->failure = std::move(message);
            state_->complete = true;
        }
        state_->changed.notify_all();
    } catch (...) {
        // Failure propagation is best effort during stack unwinding.
    }
}

} // namespace pci
