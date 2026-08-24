#include "scene/RasterPointColorizer.h"

#include "scene/RasterColorizeRunStore.h"
#include "scene/RasterColorizedPointSource.h"
#include "tasking/TaskScheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

namespace pci {
namespace {

void checkStop(const std::stop_token &stop)
{
    if (stop.stop_requested()) {
        throw PointCloudDataSourceCancelled();
    }
}

[[nodiscard]] std::uint64_t spread24(std::uint32_t value) noexcept
{
    std::uint64_t result = 0;
    for (std::uint32_t bit = 0; bit < 24; ++bit) {
        result |= static_cast<std::uint64_t>((value >> bit) & 1U) << (bit * 2U);
    }
    return result;
}

[[nodiscard]] std::uint32_t compact24(const std::uint64_t value,
                                      const std::uint32_t shift) noexcept
{
    std::uint32_t result = 0;
    for (std::uint32_t bit = 0; bit < 24; ++bit) {
        result |= static_cast<std::uint32_t>((value >> (bit * 2U + shift)) & 1U)
                  << bit;
    }
    return result;
}

[[nodiscard]] std::uint64_t
addressOf(const RasterPixelAddress &address) noexcept
{
    const std::uint64_t morton =
        spread24(address.tile.x) | (spread24(address.tile.y) << 1U);
    return (morton << 16U) |
           (static_cast<std::uint64_t>(address.innerY) << 8U) | address.innerX;
}

[[nodiscard]] std::uint32_t
histogramShift(const RasterLayerMetadata &metadata) noexcept
{
    const std::uint32_t maximumTileX =
        metadata.width == 0 ? 0 : (metadata.width - 1U) >> 8U;
    const std::uint32_t maximumTileY =
        metadata.height == 0 ? 0 : (metadata.height - 1U) >> 8U;
    const std::uint32_t coordinateBits =
        std::bit_width(std::max(maximumTileX, maximumTileY));
    const std::uint32_t mortonBits = coordinateBits * 2U;
    return mortonBits > 12U ? mortonBits - 12U : 0U;
}

[[nodiscard]] RasterTileKey tileOf(const std::uint64_t address) noexcept
{
    const std::uint64_t morton = address >> 16U;
    return RasterTileKey{
        .levelIndex = 0, .x = compact24(morton, 0), .y = compact24(morton, 1)};
}

[[nodiscard]] std::uint32_t texelOffset(const std::uint64_t address) noexcept
{
    const std::uint32_t innerX = static_cast<std::uint8_t>(address);
    const std::uint32_t innerY = static_cast<std::uint8_t>(address >> 8U);
    const std::uint32_t bufferX = innerX + rasterTileGutter;
    const std::uint32_t bufferY = innerY + rasterTileGutter;
    return (bufferY * rasterStoredTilePixels + bufferX) * 4U;
}

[[nodiscard]] PointCloudNodePayloadPtr
coloredPayload(const PointCloudNodePayloadPtr &source,
               const std::vector<std::uint32_t> &colors,
               const std::uint64_t first)
{
    if (!source) {
        return {};
    }
    auto result = std::make_shared<PointCloudNodePayload>();
    result->nodeId = source->nodeId;
    result->sourcePointCount = source->sourcePointCount;
    result->blocks.reserve(source->blocks.size());
    std::uint64_t offset = first;
    for (const PointBlockPtr &block : source->blocks) {
        if (!block) {
            result->blocks.push_back({});
            continue;
        }
        auto copy = std::make_shared<PointBlock>(*block);
        for (GpuPoint &point : copy->points) {
            point.rgba = colors[offset++];
        }
        result->blocks.push_back(std::move(copy));
    }
    return result;
}

[[nodiscard]] std::uint64_t blockBytes(const PointBlock &block) noexcept
{
    return block.points.capacity() * sizeof(GpuPoint) +
           block.attributes.capacity() * sizeof(PointAttributes);
}

template <typename Work>
void runPrivateWorkers(const std::size_t count, Work work)
{
    if (count == 0) {
        return;
    }
    TaskScheduler scheduler(count, std::max<std::uint64_t>(1, count));
    std::mutex failureMutex;
    std::exception_ptr failure;
    std::stop_source failed;
    for (std::size_t worker = 0; worker < count; ++worker) {
        static_cast<void>(
            scheduler.submit(TaskPriority::Background, 1, [&, worker] {
                try {
                    work(worker, failed.get_token());
                } catch (...) {
                    {
                        const std::scoped_lock lock(failureMutex);
                        if (!failure) {
                            failure = std::current_exception();
                        }
                    }
                    failed.request_stop();
                }
            }));
    }
    scheduler.waitForIdle();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

class ProgressReporter final {
public:
    explicit ProgressReporter(
        std::function<void(RasterColorizeProgress)> callback)
        : callback_(std::move(callback))
    {
    }

    void publish(const RasterColorizePhase phase,
                 const std::uint64_t completed,
                 const std::uint64_t total,
                 const std::uint64_t units,
                 const std::uint64_t tileReads = 0,
                 const std::uint64_t temporaryBytesWritten = 0,
                 const std::uint64_t tileFailures = 0,
                 const bool force = false)
    {
        if (!callback_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const std::scoped_lock lock(mutex_);
        const bool phaseChanged = !havePhase_ || phase != phase_;
        if (!force && !phaseChanged && units - lastUnits_ < 64) {
            return;
        }
        if (!force && !phaseChanged &&
            now - lastPublished_ < std::chrono::milliseconds(250)) {
            return;
        }
        phase_ = phase;
        havePhase_ = true;
        lastUnits_ = units;
        lastPublished_ = now;
        callback_({.phase = phase,
                   .completed = completed,
                   .total = total,
                   .tileReads = tileReads,
                   .temporaryBytesWritten = temporaryBytesWritten,
                   .tileFailures = tileFailures});
    }

private:
    std::function<void(RasterColorizeProgress)> callback_;
    std::mutex mutex_;
    std::chrono::steady_clock::time_point lastPublished_{};
    std::uint64_t lastUnits_ = 0;
    RasterColorizePhase phase_ = RasterColorizePhase::BuildingRecords;
    bool havePhase_ = false;
};

} // namespace

std::optional<RasterColorizePreparedPtr> colorizePointCloudFromRaster(
    RasterColorizePreflight preflight,
    RasterTileSourcePtr raster,
    std::shared_ptr<const RasterDecodeParameters> decode,
    const std::uint64_t rasterRenderGeneration,
    RasterColorizeOptions options,
    PointMemoryBudget::ReservationPtr colorReservation,
    PointMemoryBudget::ReservationPtr workingReservation,
    PointMemoryBudget::ReservationPtr rootStagingReservation,
    PointMemoryBudget::ReservationPtr flatStagingReservation,
    const std::stop_token stop,
    std::function<void(RasterColorizeProgress)> progress)
{
    try {
        if (!raster || !decode || !colorReservation) {
            throw RasterColorizeError(RasterColorizeFailureCode::Internal,
                                      "Colorization inputs are incomplete");
        }
        checkStop(stop);
        std::vector<std::uint32_t> colors;
        colors.resize(static_cast<std::size_t>(preflight.tableEntries));
        const std::uint64_t colorBytes =
            colors.capacity() * sizeof(std::uint32_t);
        if (!colorReservation->tryResize(colorBytes)) {
            throw RasterColorizeError(
                RasterColorizeFailureCode::InsufficientPointMemory,
                "Point-memory budget cannot account the allocated color table");
        }

        RasterColorizeRunStore runs(options.temporaryDirectory);
        std::mutex runMutex;
        ProgressReporter reporter(std::move(progress));
        RasterColorizeStatistics statistics;
        statistics.nodesRejected = preflight.rejectedNodes;
        const auto *hierarchy = std::get_if<RasterColorizeHierarchicalTarget>(
            &preflight.target.data);
        const auto *flat =
            std::get_if<RasterColorizeFlatTarget>(&preflight.target.data);
        if (hierarchy) {
            statistics.pointsConsidered = preflight.rejectedPoints;
            for (const RasterColorizeRange &range : preflight.ranges) {
                statistics.pointsConsidered += range.count;
            }
        } else {
            statistics.pointsConsidered = preflight.tableEntries;
        }

        const std::size_t workUnits =
            hierarchy ? preflight.ranges.size() +
                            (hierarchy->sourceRootPayload ? 1U : 0U)
                      : flat->blocks.size();
        const std::uint64_t aggregateBufferLimit = std::min(
            options.maximumScatterRecords, preflight.maximumRecordCount);
        const std::size_t recordLimitedWorkers =
            aggregateBufferLimit == 0
                ? options.workerCount
                : static_cast<std::size_t>(std::min<std::uint64_t>(
                      options.workerCount, aggregateBufferLimit));
        const std::size_t recordWorkers =
            std::min(workUnits, recordLimitedWorkers);
        const std::uint32_t tileHistogramShift =
            histogramShift(raster->metadata());
        std::vector<std::array<std::uint64_t, 4096>> histograms(recordWorkers);
        std::atomic_size_t nextUnit = 0;
        std::atomic_uint64_t built = 0;
        std::atomic_uint64_t completedUnits = 0;
        runPrivateWorkers(
            recordWorkers,
            [&](const std::size_t worker, const std::stop_token failed) {
                const std::uint64_t workerCapacity =
                    recordWorkers == 0
                        ? 0
                        : aggregateBufferLimit / recordWorkers +
                              (worker < aggregateBufferLimit % recordWorkers
                                   ? 1U
                                   : 0U);
                std::vector<RasterSortRecord> buffer;
                buffer.reserve(static_cast<std::size_t>(workerCapacity));
                const auto checkWorkStop = [&] {
                    checkStop(stop);
                    if (failed.stop_requested()) {
                        throw PointCloudDataSourceCancelled();
                    }
                };
                const auto flush = [&] {
                    if (buffer.empty()) {
                        return;
                    }
                    checkWorkStop();
                    std::ranges::sort(buffer);
                    {
                        const std::scoped_lock lock(runMutex);
                        runs.writeSortedRun(buffer);
                    }
                    buffer.clear();
                };
                const auto emit = [&](const RasterPixelAddress &address,
                                      const std::uint64_t destination) {
                    if (workerCapacity == 0) {
                        throw RasterColorizeError(
                            RasterColorizeFailureCode::Internal,
                            "Colorization emitted a record outside its "
                            "preflight bound");
                    }
                    const std::uint64_t encoded = addressOf(address);
                    buffer.push_back({.address = encoded,
                                      .destination = static_cast<std::uint32_t>(
                                          destination)});
                    const std::uint64_t tileMorton = encoded >> 16U;
                    ++histograms[worker][static_cast<std::size_t>(
                        (tileMorton >> tileHistogramShift) & 0xfffU)];
                    if (buffer.size() == workerCapacity) {
                        flush();
                    }
                };
                const auto initializeBlock =
                    [&](const PointBlock &block,
                        const std::uint64_t first,
                        const std::uint32_t count,
                        const std::uint32_t *sourceColors) {
                        if (block.points.size() != count) {
                            throw RasterColorizeError(
                                RasterColorizeFailureCode::SourceChanged,
                                "Point-cloud block count changed during "
                                "colorization");
                        }
                        const auto placement = preflight.placement.forBlock(
                            block.origin.x, block.origin.y, block.scale);
                        for (std::uint32_t index = 0; index < count; ++index) {
                            if ((index & 4095U) == 0) {
                                checkWorkStop();
                            }
                            const GpuPoint &point = block.points[index];
                            colors[first + index] =
                                sourceColors ? sourceColors[index] : point.rgba;
                            if (const auto address =
                                    preflight.placement.addressOf(
                                        placement, point.x, point.y)) {
                                emit(*address, first + index);
                            }
                        }
                    };

                while (true) {
                    checkWorkStop();
                    const std::size_t unit = nextUnit.fetch_add(1);
                    if (unit >= workUnits) {
                        break;
                    }
                    std::uint64_t unitPoints = 0;
                    if (hierarchy && unit < preflight.ranges.size()) {
                        const RasterColorizeRange &range =
                            preflight.ranges[unit];
                        PointCloudNodePayloadPtr payload =
                            hierarchy->baseSource->loadNode(range.node, stop);
                        if (!payload || pointCloudNodePayloadPoints(*payload) !=
                                            range.count) {
                            throw RasterColorizeError(
                                RasterColorizeFailureCode::SourceChanged,
                                "Stored point-cloud node count changed during "
                                "colorization");
                        }
                        std::uint64_t offset = range.offset;
                        for (const PointBlockPtr &block : payload->blocks) {
                            if (block) {
                                initializeBlock(*block,
                                                offset,
                                                static_cast<std::uint32_t>(
                                                    block->points.size()),
                                                nullptr);
                                offset += block->points.size();
                            }
                        }
                        unitPoints = range.count;
                    } else if (hierarchy) {
                        std::uint64_t offset = preflight.rootOffset;
                        for (const PointBlockPtr &block :
                             hierarchy->sourceRootPayload->blocks) {
                            if (block) {
                                initializeBlock(*block,
                                                offset,
                                                static_cast<std::uint32_t>(
                                                    block->points.size()),
                                                nullptr);
                                offset += block->points.size();
                            }
                        }
                        unitPoints = preflight.rootCount;
                    } else {
                        const SceneBlock &entry = flat->blocks[unit];
                        const RasterColorizeFlatRange &range =
                            preflight.flatRanges[unit];
                        const std::uint32_t *source =
                            flat->sourceColors
                                ? flat->sourceColors->data() + range.offset
                                : nullptr;
                        initializeBlock(
                            *entry.block, range.offset, range.count, source);
                        unitPoints = range.count;
                    }
                    const std::uint64_t completed =
                        built.fetch_add(unitPoints) + unitPoints;
                    const std::uint64_t units = completedUnits.fetch_add(1) + 1;
                    reporter.publish(RasterColorizePhase::BuildingRecords,
                                     completed,
                                     preflight.tableEntries,
                                     units);
                }
                flush();
            });
        reporter.publish(RasterColorizePhase::BuildingRecords,
                         built.load(),
                         preflight.tableEntries,
                         completedUnits.load(),
                         0,
                         0,
                         0,
                         true);
        statistics.temporaryBytesWritten = runs.bytesWritten();
        reporter.publish(RasterColorizePhase::SamplingRaster,
                         0,
                         runs.recordCount(),
                         0,
                         0,
                         statistics.temporaryBytesWritten,
                         0,
                         true);

        RasterTileSourcePtr samplingSource =
            raster->detachedReader(options.workerCount);
        const bool hasDetachedReader = static_cast<bool>(samplingSource);
        if (!hasDetachedReader) {
            samplingSource = raster;
        }
        std::vector<std::uint64_t> boundaries;
        // The base interface makes no concurrency guarantee. A source that
        // cannot provide an independently pooled reader is deliberately
        // sampled by one worker.
        if (hasDetachedReader && runs.recordCount() > 0 &&
            options.workerCount > 1) {
            const std::uint64_t desired = std::min<std::uint64_t>(
                options.workerCount, runs.recordCount());
            std::array<std::uint64_t, 4096> histogram{};
            for (const auto &workerHistogram : histograms) {
                for (std::size_t bin = 0; bin < histogram.size(); ++bin) {
                    histogram[bin] += workerHistogram[bin];
                }
            }
            std::uint64_t seen = 0;
            std::uint64_t nextBoundary = 1;
            for (std::size_t bin = 0;
                 bin < histogram.size() && nextBoundary < desired;
                 ++bin) {
                seen += histogram[bin];
                if (seen < runs.recordCount() * nextBoundary / desired ||
                    bin + 1U == histogram.size()) {
                    continue;
                }
                if (seen == runs.recordCount()) {
                    break;
                }
                const std::uint64_t tileBoundary =
                    static_cast<std::uint64_t>(bin + 1U) << tileHistogramShift;
                boundaries.push_back(tileBoundary << 16U);
                do {
                    ++nextBoundary;
                } while (nextBoundary < desired &&
                         seen >= runs.recordCount() * nextBoundary / desired);
            }
        }
        const std::size_t samplingWorkers =
            runs.recordCount() == 0 ? 0 : boundaries.size() + 1;
        std::atomic_uint64_t sampled = 0;
        std::atomic_uint64_t colored = 0;
        std::atomic_uint64_t tileReads = 0;
        std::atomic_uint64_t tileFailures = 0;
        std::atomic_uint64_t completedTiles = 0;
        const bool hierarchicalTarget = hierarchy != nullptr;
        runPrivateWorkers(
            samplingWorkers,
            [&](const std::size_t worker, const std::stop_token failed) {
                const auto checkWorkStop = [&] {
                    checkStop(stop);
                    if (failed.stop_requested()) {
                        throw PointCloudDataSourceCancelled();
                    }
                };
                RasterTileKey currentKey{};
                bool haveTile = false;
                bool tileReadable = false;
                RasterTileData tile;
                const std::uint64_t lower =
                    worker == 0 ? 0 : boundaries[worker - 1];
                const std::optional<std::uint64_t> upper =
                    worker < boundaries.size()
                        ? std::optional<std::uint64_t>{boundaries[worker]}
                        : std::nullopt;
                runs.forEachMergedAddressRange(
                    lower,
                    upper,
                    [&](const RasterSortRecord &record) {
                        checkWorkStop();
                        const RasterTileKey key = tileOf(record.address);
                        if (!haveTile || key != currentKey) {
                            currentKey = key;
                            haveTile = true;
                            tileReadable = false;
                            tileReads.fetch_add(1);
                            completedTiles.fetch_add(1);
                            try {
                                tile = samplingSource->readTile(
                                    RasterTileRequest{
                                        .key = key,
                                        .renderGeneration =
                                            rasterRenderGeneration,
                                        .decode = decode},
                                    stop);
                                tileReadable = true;
                            } catch (const RasterReadCancelled &) {
                                throw;
                            } catch (const RasterReadError &) {
                                const std::uint64_t failures =
                                    tileFailures.fetch_add(1) + 1;
                                if (failures > options.maximumTileFailures) {
                                    throw RasterColorizeError(
                                        RasterColorizeFailureCode::Io,
                                        "Too many raster tiles failed to "
                                        "decode");
                                }
                            }
                        }
                        if (tileReadable) {
                            if (const auto color = rasterTexelToPointColor(
                                    tile, texelOffset(record.address))) {
                                colors[record.destination] = *color;
                                if (!hierarchicalTarget ||
                                    record.destination < preflight.rootOffset) {
                                    colored.fetch_add(1);
                                }
                            }
                        }
                        const std::uint64_t completed =
                            sampled.fetch_add(1) + 1;
                        reporter.publish(RasterColorizePhase::SamplingRaster,
                                         completed,
                                         runs.recordCount(),
                                         completedTiles.load(),
                                         tileReads.load(),
                                         statistics.temporaryBytesWritten,
                                         tileFailures.load());
                    },
                    stop);
            });
        reporter.publish(RasterColorizePhase::SamplingRaster,
                         sampled.load(),
                         runs.recordCount(),
                         completedTiles.load(),
                         tileReads.load(),
                         statistics.temporaryBytesWritten,
                         tileFailures.load(),
                         true);
        statistics.pointsColored = colored.load();
        statistics.tileReads = tileReads.load();
        statistics.tileFailures = tileFailures.load();
        statistics.pointsUnchanged =
            statistics.pointsConsidered - statistics.pointsColored;
        workingReservation.reset();
        checkStop(stop);

        auto prepared = std::make_shared<RasterColorizePrepared>();
        prepared->scene = preflight.target.scene;
        prepared->statistics = statistics;
        if (auto *hierarchyTarget =
                std::get_if<RasterColorizeHierarchicalTarget>(
                    &preflight.target.data)) {
            PointCloudNodePayloadPtr root =
                coloredPayload(hierarchyTarget->sourceRootPayload,
                               colors,
                               preflight.rootOffset);
            auto decorated = std::make_shared<RasterColorizedPointSource>(
                hierarchyTarget->baseSource,
                std::move(colors),
                std::move(preflight.ranges),
                std::move(colorReservation));
            prepared->data = RasterColorizePreparedHierarchy{
                .baseSource = hierarchyTarget->baseSource,
                .colorizedSource = std::move(decorated),
                .sourceRootPayload = hierarchyTarget->sourceRootPayload,
                .coloredRootPayload = std::move(root),
                .expectedRootPayloadRevision =
                    hierarchyTarget->expectedRootPayloadRevision,
                .rootStagingReservation = std::move(rootStagingReservation),
            };
        } else {
            auto &flatTarget =
                std::get<RasterColorizeFlatTarget>(preflight.target.data);
            RasterColorizePreparedFlat preparedFlat;
            preparedFlat.expectedBlocks.reserve(flatTarget.blocks.size());
            preparedFlat.replacementBlocks.reserve(flatTarget.blocks.size());
            std::uint64_t actualStagingBytes = 0;
            for (std::size_t index = 0; index < flatTarget.blocks.size();
                 ++index) {
                const SceneBlock &entry = flatTarget.blocks[index];
                const RasterColorizeFlatRange &range =
                    preflight.flatRanges[index];
                auto replacement = std::make_shared<PointBlock>(*entry.block);
                for (std::uint32_t point = 0; point < range.count; ++point) {
                    replacement->points[point].rgba =
                        colors[range.offset + point];
                }
                actualStagingBytes += blockBytes(*replacement);
                preparedFlat.expectedBlocks.push_back({
                    .id = entry.id,
                    .block = entry.block,
                    .pointCount = entry.block->points.size(),
                    .attributeCount = entry.block->attributes.size(),
                });
                preparedFlat.replacementBlocks.push_back(
                    {.id = entry.id, .block = std::move(replacement)});
            }
            if (!flatStagingReservation ||
                !flatStagingReservation->tryResize(actualStagingBytes)) {
                throw RasterColorizeError(
                    RasterColorizeFailureCode::InsufficientPointMemory,
                    "Point-memory budget cannot stage replacement blocks");
            }
            preparedFlat.stagingReservation = std::move(flatStagingReservation);
            if (!flatTarget.sourceColors) {
                for (std::size_t index = 0; index < flatTarget.blocks.size();
                     ++index) {
                    const SceneBlock &entry = flatTarget.blocks[index];
                    const auto &range = preflight.flatRanges[index];
                    for (std::uint32_t point = 0; point < range.count;
                         ++point) {
                        colors[range.offset + point] =
                            entry.block->points[point].rgba;
                    }
                }
                preparedFlat.displacedSourceColors =
                    std::make_shared<std::vector<std::uint32_t>>(
                        std::move(colors));
                preparedFlat.colorReservation = std::move(colorReservation);
            } else {
                colors.clear();
                colors.shrink_to_fit();
                colorReservation.reset();
            }
            prepared->data = std::move(preparedFlat);
        }
        return prepared;
    } catch (const RasterReadCancelled &) {
        return std::nullopt;
    } catch (const PointCloudDataSourceCancelled &) {
        return std::nullopt;
    }
}

} // namespace pci
