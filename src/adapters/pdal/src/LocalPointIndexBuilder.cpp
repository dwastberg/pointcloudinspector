#include <pci/adapters/pdal/LocalPointIndexBuilder.h>

#include "LeafPageWriter.h"
#include "MortonRunBuilder.h"
#include "ParentHierarchyBuilder.h"
#include "PdalPointMapping.h"
#include <pci/adapters/pdal/LocalPageBuildInfrastructure.h>
#include <pci/foundation/CheckedArithmetic.h>
#include <pci/pointcloud/BlockPartitioner.h>

#include <pdal/Options.hpp>
#include <pdal/PointRef.hpp>
#include <pdal/PointTable.hpp>
#include <pdal/StageFactory.hpp>
#include <pdal/filters/StreamCallbackFilter.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pci {
namespace {

constexpr std::uint64_t rootWarmupPoints = 65'536;
constexpr std::uint8_t maximumSupportedLevel = 20;

std::filesystem::path canonicalPath(const std::filesystem::path &path)
{
    std::error_code error;
    std::filesystem::path result = std::filesystem::canonical(path, error);
    if (!error) {
        return result;
    }
    error.clear();
    result = std::filesystem::absolute(path, error);
    return error ? path.lexically_normal() : result.lexically_normal();
}

std::uint8_t hierarchyLevel(const std::uint64_t pointCount,
                            const std::uint32_t pointsPerLeaf) noexcept
{
    std::uint8_t level = 0;
    std::uint64_t capacity = pointsPerLeaf;
    while (capacity < pointCount && level < maximumSupportedLevel) {
        ++level;
        const auto next = checkedMultiply(capacity, std::uint64_t{8});
        if (!next) {
            break;
        }
        capacity = *next;
    }
    return level;
}

std::uint64_t divideRoundUp(const std::uint64_t value,
                            const std::uint64_t divisor) noexcept
{
    return value / divisor + (value % divisor != 0 ? 1U : 0U);
}

struct StorageEstimate {
    std::uint64_t committedBytes = 0;
    std::uint64_t temporaryPeakBytes = 0;
};

StorageEstimate estimateStorage(const std::uint64_t sourcePoints,
                                const std::uint32_t pointsPerLeaf,
                                const std::uint32_t rootPreviewPoints,
                                const std::uint8_t maximumLevel) noexcept
{
    constexpr std::uint64_t manifestFixedAllowance =
        std::uint64_t{1} * 1024 * 1024;
    constexpr std::uint64_t manifestBytesPerPage = 128;
    std::uint64_t payloadPoints = sourcePoints;
    std::uint64_t pageCount = 1;
    if (maximumLevel > 0) {
        std::uint64_t nodes = divideRoundUp(sourcePoints, pointsPerLeaf);
        pageCount = saturatingAdd(pageCount, nodes);
        // Keep an early preview page so a first-time build can publish a
        // scene promptly, then append a representative root sampled from the
        // completed hierarchy before committing the manifest.
        payloadPoints = saturatingAdd(
            payloadPoints,
            saturatingMultiply<std::uint64_t>(
                2, std::min<std::uint64_t>(sourcePoints, rootPreviewPoints)));
        for (std::uint8_t level = maximumLevel; level > 1; --level) {
            nodes = divideRoundUp(nodes, 8);
            pageCount = saturatingAdd(pageCount, nodes);
            payloadPoints = saturatingAdd(
                payloadPoints,
                saturatingMultiply<std::uint64_t>(nodes, rootPreviewPoints));
        }
    }
    const std::uint64_t payloadBytes =
        saturatingMultiply<std::uint64_t>(payloadPoints, localPointDiskBytes);
    const std::uint64_t manifestBytes =
        saturatingAdd(manifestFixedAllowance,
                      saturatingMultiply(pageCount, manifestBytesPerPage));
    const std::uint64_t committed = saturatingAdd(payloadBytes, manifestBytes);
    return {
        .committedBytes = committed,
        .temporaryPeakBytes = saturatingAdd(
            committed,
            saturatingMultiply<std::uint64_t>(
                sourcePoints, local_index::MortonRunBuilder::recordBytes())),
    };
}

class SpatialRootPreview final {
public:
    SpatialRootPreview(const Bounds3d &bounds,
                       const std::uint32_t maximumPoints,
                       const bool retainAll)
        : bounds_(bounds)
        , retainAll_(retainAll)
    {
        samples_.reserve(maximumPoints);
        if (retainAll_) {
            return;
        }
        while (true) {
            const std::uint64_t cells = counts_[0] * counts_[1] * counts_[2];
            std::optional<std::size_t> axis;
            for (std::size_t candidate = 0; candidate < 3; ++candidate) {
                const std::uint64_t next =
                    cells / counts_[candidate] * (counts_[candidate] + 1U);
                if (next <= maximumPoints &&
                    (!axis || counts_[candidate] < counts_[*axis])) {
                    axis = candidate;
                }
            }
            if (!axis) {
                break;
            }
            ++counts_[*axis];
        }
        totalCells_ = counts_[0] * counts_[1] * counts_[2];
        for (std::size_t i = 0; i < 3; ++i) {
            const double extent = bounds_.maximum[i] - bounds_.minimum[i];
            invExtent_[i] =
                extent > 0.0 ? static_cast<double>(counts_[i]) / extent : 0.0;
        }
        occupied_.reserve(maximumPoints);
    }

    void add(const PointSample &sample)
    {
        if (retainAll_) {
            samples_.push_back(sample);
            return;
        }
        if (samples_.size() >= totalCells_) {
            return;
        }
        const std::array<double, 3> components{
            sample.position.x,
            sample.position.y,
            sample.position.z,
        };
        std::array<std::uint64_t, 3> indices{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double normalized =
                (components[axis] - bounds_.minimum[axis]) * invExtent_[axis];
            indices[axis] = static_cast<std::uint32_t>(std::clamp(
                std::floor(normalized),
                0.0,
                static_cast<double>(counts_[axis] - 1U)));
        }
        const std::uint64_t key =
            indices[0] + counts_[0] * (indices[1] + counts_[1] * indices[2]);
        if (occupied_.insert(key).second) {
            samples_.push_back(sample);
        }
    }

    [[nodiscard]] const std::vector<PointSample> &samples() const noexcept
    {
        return samples_;
    }

private:
    Bounds3d bounds_;
    bool retainAll_ = false;
    std::array<std::uint64_t, 3> counts_{1, 1, 1};
    std::array<double, 3> invExtent_{0.0, 0.0, 0.0};
    std::uint64_t totalCells_ = 1;
    std::unordered_set<std::uint64_t> occupied_;
    std::vector<PointSample> samples_;
};

PointCloudNodePayloadPtr makePayload(const PointCloudNodeId id,
                                     const Bounds3d &bounds,
                                     const std::span<const PointSample> samples,
                                     const std::uint64_t sourcePointCount)
{
    auto payload = std::make_shared<PointCloudNodePayload>();
    payload->nodeId = id;
    payload->sourcePointCount = sourcePointCount;
    if (!samples.empty()) {
        BlockPartitioner partitioner(
            bounds,
            samples.size(),
            [&payload](PointBlockPtr block) {
                payload->blocks.push_back(std::move(block));
            },
            samples.size());
        for (const PointSample &sample : samples) {
            partitioner.add(sample);
        }
        partitioner.finish();
    }
    return payload;
}

} // namespace

LocalPointPageBuildResult LocalPointIndexBuilder::openOrBuild(
    const PointCloudImportPreflight &preflight,
    const std::uint64_t maximumPoints,
    const LocalPointPageStoreOptions &options,
    const std::stop_token stopToken,
    std::function<void(PointCloudImportProgress)> progress,
    RootReady rootReady) const
{
    if (preflight.hierarchical &&
        preflight.metadata.sourceDriver != "readers.las") {
        throw std::invalid_argument(
            "local page builder requires an ordinary LAS/LAZ source");
    }
    if (maximumPoints == 0 || options.pointsPerLeaf == 0 ||
        options.rootPreviewPoints == 0 ||
        options.sortMemoryBytes <
            local_index::MortonRunBuilder::recordBytes() ||
        !options.cache) {
        throw std::invalid_argument("invalid local point page options");
    }
    if (stopToken.stop_requested()) {
        throw PointCloudImportCancelled();
    }
    const std::filesystem::path &cacheDirectory = options.cache->directory();
    const local_index::LocalPageCacheLocator cache(preflight, options);
    const LocalPointSourceFingerprint &fingerprint = cache.fingerprint();
    const std::filesystem::path &finalDirectory = cache.finalDirectory();
    if (auto existing = cache.tryOpen(maximumPoints, rootReady)) {
        return std::move(*existing);
    }

    const std::uint8_t maximumLevel = hierarchyLevel(
        preflight.metadata.sourcePointCount, options.pointsPerLeaf);
    const StorageEstimate storage =
        estimateStorage(preflight.metadata.sourcePointCount,
                        options.pointsPerLeaf,
                        options.rootPreviewPoints,
                        maximumLevel);
    if (options.diskCacheBytes != 0) {
        if (storage.committedBytes > options.diskCacheBytes) {
            throw std::runtime_error("estimated local page entry exceeds the "
                                     "configured disk cache allowance");
        }
        const std::uint64_t targetBeforeBuild =
            options.diskCacheBytes - storage.committedBytes;
        cache.prune(std::max<std::uint64_t>(1, targetBeforeBuild));
    }
    std::error_code spaceError;
    const std::filesystem::space_info space =
        std::filesystem::space(cacheDirectory, spaceError);
    if (!spaceError && space.available < storage.temporaryPeakBytes) {
        throw std::runtime_error("insufficient free disk space to construct "
                                 "the local point page entry");
    }

    local_index::LocalPageBuildLock buildLock(cache.lockPath());
    while (!buildLock.tryAcquire()) {
        if (stopToken.stop_requested()) {
            throw PointCloudImportCancelled();
        }
        if (auto existing = cache.tryOpen(maximumPoints, rootReady)) {
            return std::move(*existing);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (auto existing = cache.tryOpen(maximumPoints, rootReady)) {
        return std::move(*existing);
    }

    const local_index::LocalPageBuildSession buildSession(cacheDirectory,
                                                          cache.key());
    const std::filesystem::path &temporaryDirectory = buildSession.directory();

    const std::filesystem::path payloadPath =
        temporaryDirectory / "payload.bin";
    auto source = LocalPointPageSource::createBuilding(preflight.metadata,
                                                       payloadPath,
                                                       temporaryDirectory,
                                                       fingerprint,
                                                       maximumLevel,
                                                       options.pointsPerLeaf,
                                                       maximumPoints);

    try {
        local_index::LeafPageWriter payloadWriter(payloadPath, source);
        const std::uint32_t rootLimit =
            maximumLevel == 0
                ? static_cast<std::uint32_t>(std::min<std::uint64_t>(
                      preflight.metadata.sourcePointCount,
                      std::numeric_limits<std::uint32_t>::max()))
                : options.rootPreviewPoints;
        SpatialRootPreview rootPreview(
            preflight.metadata.sourceBounds, rootLimit, maximumLevel == 0);
        const std::uint64_t rootPublishAfter =
            maximumLevel == 0 ? preflight.metadata.sourcePointCount
                              : std::min(preflight.metadata.sourcePointCount,
                                         rootWarmupPoints);
        PointCloudNodePayloadPtr rootPayload;
        std::vector<LocalPointPageRecord> pages;
        local_index::MortonRunBuilder runBuilder(
            temporaryDirectory / "runs",
            options.sortMemoryBytes,
            preflight.metadata.sourceBounds,
            maximumSupportedLevel);
        LocalPointScalarRanges ranges;
        bool haveRanges = false;
        std::uint64_t processed = 0;
        auto lastTouchTime = std::chrono::steady_clock::now();

        const auto publishRoot = [&] {
            if (rootPayload || rootPreview.samples().empty()) {
                return;
            }
            pages.push_back(
                payloadWriter.appendPage(rootPointCloudNode,
                                         rootPreview.samples(),
                                         preflight.metadata.sourcePointCount,
                                         preflight.metadata.sourceBounds));
            rootPayload = makePayload(rootPointCloudNode,
                                      pages.back().tightBounds,
                                      rootPreview.samples(),
                                      preflight.metadata.sourcePointCount);
            if (rootReady) {
                rootReady(source, rootPayload);
            }
        };
        if (progress) {
            progress({
                .stage = PointCloudImportStage::Reading,
                .processed = 0,
                .total = preflight.metadata.sourcePointCount,
            });
        }
        pdal::StageFactory factory;
        pdal::Stage *reader =
            factory.createStage(preflight.metadata.sourceDriver);
        if (!reader) {
            throw std::runtime_error("PDAL reader is unavailable: " +
                                     preflight.metadata.sourceDriver);
        }
        pdal::Options readerOptions;
        readerOptions.add("filename", preflight.metadata.sourcePath.string());
        reader->setOptions(readerOptions);
        pdal::StreamCallbackFilter callback;
        callback.setInput(*reader);
        callback.setCallback([&](pdal::PointRef &point) {
            if (stopToken.stop_requested()) {
                throw PointCloudImportCancelled();
            }
            PointSample sample = mapPdalPoint(point, preflight.metadata);
            if (!rootPayload) {
                rootPreview.add(sample);
            }
            runBuilder.add(sample, processed);
            const PointAttributes attributes = sample.attributes;
            if (!haveRanges) {
                ranges = {
                    .intensityMinimum = attributes.intensity,
                    .intensityMaximum = attributes.intensity,
                    .classificationMinimum = attributes.classification,
                    .classificationMaximum = attributes.classification,
                    .returnNumberMinimum = attributes.returnNumber,
                    .returnNumberMaximum = attributes.returnNumber,
                };
                haveRanges = true;
            } else {
                ranges.intensityMinimum =
                    std::min(ranges.intensityMinimum, attributes.intensity);
                ranges.intensityMaximum =
                    std::max(ranges.intensityMaximum, attributes.intensity);
                ranges.classificationMinimum = std::min(
                    ranges.classificationMinimum, attributes.classification);
                ranges.classificationMaximum = std::max(
                    ranges.classificationMaximum, attributes.classification);
                ranges.returnNumberMinimum = std::min(
                    ranges.returnNumberMinimum, attributes.returnNumber);
                ranges.returnNumberMaximum = std::max(
                    ranges.returnNumberMaximum, attributes.returnNumber);
            }
            ++processed;
            if (processed == rootPublishAfter) {
                publishRoot();
            }
            if (processed % 65'536 == 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastTouchTime >= std::chrono::seconds(2)) {
                    buildLock.touch();
                    lastTouchTime = now;
                }
                if (progress) {
                    progress({
                        .stage = PointCloudImportStage::Reading,
                        .processed = processed,
                        .total = preflight.metadata.sourcePointCount,
                    });
                }
            }
            return true;
        });
        pdal::FixedPointTable table(4096);
        callback.prepare(table);
        if (!callback.pipelineStreamable()) {
            throw std::runtime_error(
                "PDAL pipeline is not streamable for local paging");
        }
        callback.execute(table);
        runBuilder.finish();
        publishRoot();
        if (!rootPayload || processed == 0) {
            throw std::runtime_error(
                "local point source did not produce a root preview");
        }
        if (processed != preflight.metadata.sourcePointCount) {
            throw std::runtime_error(
                "local point source count changed during indexing");
        }
        if (progress) {
            progress({
                .stage = PointCloudImportStage::Optimizing,
                .processed = 0,
                .total = preflight.metadata.sourcePointCount,
            });
        }

        if (maximumLevel > 0) {
            buildLock.touch();
            local_index::ParentMap currentParents;
            local_index::ParentAccumulator rootAccumulator;
            rootAccumulator.init(rootPointCloudNode, options.rootPreviewPoints);

            const auto onLeaf = [&](const LocalPointPageRecord &page,
                                    const std::span<const PointSample> samples) {
                if (maximumLevel == 1) {
                    local_index::accumulateChildPage(
                        rootAccumulator, page, samples, options.rootPreviewPoints);
                } else {
                    local_index::accumulateChildPage(
                        currentParents, page, samples, options.rootPreviewPoints);
                }
            };

            const auto inMemory = runBuilder.inMemoryRecords();
            std::vector<LocalPointPageRecord> levelPages;
            if (!inMemory.empty()) {
                levelPages = payloadWriter.writeLeafPages(
                    inMemory,
                    maximumLevel,
                    options.pointsPerLeaf,
                    stopToken,
                    [&] { buildLock.touch(); },
                    onLeaf);
            } else {
                levelPages = payloadWriter.mergeRuns(
                    runBuilder.runs(),
                    maximumLevel,
                    options.pointsPerLeaf,
                    stopToken,
                    [&] { buildLock.touch(); },
                    onLeaf);
            }
            pages.insert(pages.end(), levelPages.begin(), levelPages.end());

            for (std::uint8_t level = maximumLevel; level > 1; --level) {
                buildLock.touch();
                const std::uint8_t parentLevel =
                    static_cast<std::uint8_t>(level - 1U);
                std::vector<PointCloudNodeId> parentIds;
                parentIds.reserve(currentParents.size());
                for (const auto &[id, _] : currentParents) {
                    parentIds.push_back(id);
                }
                std::ranges::sort(parentIds);

                local_index::ParentMap nextParents;
                for (const PointCloudNodeId id : parentIds) {
                    if (stopToken.stop_requested()) {
                        throw PointCloudImportCancelled();
                    }
                    auto &acc = currentParents[id];
                    LocalPointPageRecord pageRecord = payloadWriter.appendPage(
                        id, acc.samples, acc.sourcePoints, acc.bounds);
                    pages.push_back(pageRecord);
                    if (parentLevel > 1) {
                        local_index::accumulateChildPage(
                            nextParents,
                            pageRecord,
                            acc.samples,
                            options.rootPreviewPoints);
                    } else {
                        local_index::accumulateChildPage(
                            rootAccumulator,
                            pageRecord,
                            acc.samples,
                            options.rootPreviewPoints);
                    }
                }
                currentParents = std::move(nextParents);
            }

            if (rootAccumulator.samples.empty()) {
                throw std::runtime_error(
                    "could not construct final local point root preview");
            }
            const LocalPointPageRecord finalRoot =
                payloadWriter.appendPage(rootPointCloudNode,
                                         rootAccumulator.samples,
                                         preflight.metadata.sourcePointCount,
                                         preflight.metadata.sourceBounds);
            rootPayload = makePayload(rootPointCloudNode,
                                      finalRoot.tightBounds,
                                      rootAccumulator.samples,
                                      preflight.metadata.sourcePointCount);
            const auto storedRoot = std::ranges::find_if(
                pages, [](const LocalPointPageRecord &page) {
                    return page.id == rootPointCloudNode;
                });
            if (storedRoot == pages.end()) {
                throw std::logic_error("local page table lost its root record");
            }
            *storedRoot = finalRoot;
        }
        payloadWriter.close();
        std::error_code removeError;
        std::filesystem::remove_all(temporaryDirectory / "runs", removeError);
        if (removeError) {
            throw std::runtime_error(
                "could not remove local point temporary sort runs");
        }

        LocalPointPageManifest manifest{
            .fingerprint = fingerprint,
            .metadata = preflight.metadata,
            .canonicalSourcePath = canonicalPath(preflight.metadata.sourcePath),
            .sourceFileBytes = preflight.sourceFileBytes,
            .sourceModificationTime = preflight.sourceModificationTime,
            .payloadFileBytes = payloadWriter.bytesWritten(),
            .maximumLevel = maximumLevel,
            .pointsPerLeaf = options.pointsPerLeaf,
            .rootPreviewPoints = options.rootPreviewPoints,
            .scalarRanges = ranges,
            .pages = pages,
        };
        if (stopToken.stop_requested()) {
            throw PointCloudImportCancelled();
        }
        local_index::LocalPageCommitter::commit(
            temporaryDirectory, finalDirectory, manifest, *options.cache);
        PointCloudScalarRanges completeRanges;
        if (preflight.metadata.hasIntensity && haveRanges) {
            completeRanges.intensity = PointScalarRange{
                static_cast<double>(ranges.intensityMinimum),
                static_cast<double>(ranges.intensityMaximum),
            };
        }
        source->setScalarRanges(completeRanges);
        source->finishCommit(finalDirectory);
        cache.prune(options.diskCacheBytes);
        if (progress) {
            progress({
                .stage = PointCloudImportStage::Optimizing,
                .processed = preflight.metadata.sourcePointCount,
                .total = preflight.metadata.sourcePointCount,
            });
        }
        return {
            .source = std::move(source),
            .rootPayload = std::move(rootPayload),
            .storeDirectory = finalDirectory,
            .sourcePointsScanned = processed,
            .payloadBytes = manifest.payloadFileBytes,
            .pageCount = pages.size(),
            .reused = false,
        };
    } catch (const PointCloudImportCancelled &) {
        source->fail("local point page construction cancelled");
        throw;
    } catch (const std::exception &error) {
        source->fail(error.what());
        throw;
    }
}

} // namespace pci
