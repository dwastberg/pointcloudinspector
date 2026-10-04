#include <pci/adapters/pdal/PdalHierarchicalPointSource.h>

#include "PdalPointMapping.h"
#include <pci/foundation/CheckedArithmetic.h>
#include <pci/pointcloud/BlockPartitioner.h>

#include <pdal/Options.hpp>
#include <pdal/PointRef.hpp>
#include <pdal/PointTable.hpp>
#include <pdal/StageFactory.hpp>
#include <pdal/filters/StreamCallbackFilter.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pci {
namespace {

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

std::uint64_t estimatedDecodedBytes(const std::uint64_t points) noexcept
{
    constexpr std::uint64_t bytesPerPoint =
        sizeof(GpuPoint) + sizeof(PointAttributes);
    return saturatingMultiply(points, bytesPerPoint);
}

std::string pdalBounds(const Bounds3d &bounds)
{
    std::ostringstream value;
    value << std::setprecision(17) << "([" << bounds.minimum[0] << ','
          << bounds.maximum[0] << "],[" << bounds.minimum[1] << ','
          << bounds.maximum[1] << "],[" << bounds.minimum[2] << ','
          << bounds.maximum[2] << "])";
    return value.str();
}

std::uint64_t splitMix64(std::uint64_t value) noexcept
{
    value += std::uint64_t{0x9e3779b97f4a7c15};
    value = (value ^ (value >> 30U)) * std::uint64_t{0xbf58476d1ce4e5b9};
    value = (value ^ (value >> 27U)) * std::uint64_t{0x94d049bb133111eb};
    return value ^ (value >> 31U);
}

std::uint64_t nodeSeed(const PointCloudNodeId id) noexcept
{
    std::uint64_t seed = id.level;
    seed = splitMix64(seed ^ id.x);
    seed = splitMix64(seed ^ (static_cast<std::uint64_t>(id.y) << 21U));
    return splitMix64(seed ^ (static_cast<std::uint64_t>(id.z) << 42U));
}

} // namespace

PdalHierarchicalPointSource::PdalHierarchicalPointSource(
    PointCloudMetadata metadata,
    const std::uint64_t maximumPoints,
    const std::size_t pointsPerNode)
    : metadata_(std::move(metadata))
    , selectedPointCount_(std::min(metadata_.sourcePointCount, maximumPoints))
    , pointsPerNode_(pointsPerNode)
{
    if (metadata_.sourceDriver != "readers.copc" &&
        metadata_.sourceDriver != "readers.ept") {
        throw std::invalid_argument(
            "hierarchical PDAL source requires COPC or EPT");
    }
    if (maximumPoints == 0 || pointsPerNode == 0) {
        throw std::invalid_argument(
            "hierarchical source point limits must be positive");
    }
    constexpr std::uint8_t maximumSupportedLevel = 20;
    std::uint64_t represented = pointsPerNode_;
    while (represented < selectedPointCount_ &&
           maximumLevel_ < maximumSupportedLevel) {
        ++maximumLevel_;
        const auto next = checkedMultiply(represented, std::uint64_t{4});
        if (!next) {
            break;
        }
        represented = *next;
    }
}

PointCloudNode PdalHierarchicalPointSource::rootNode() const
{
    return node(rootPointCloudNode);
}

PointCloudNode
PdalHierarchicalPointSource::node(const PointCloudNodeId id) const
{
    const std::uint64_t cells = std::uint64_t{1} << id.level;
    if (id.level > maximumLevel_ || id.x >= cells || id.y >= cells ||
        id.z >= cells) {
        throw std::out_of_range("point-cloud node is outside the hierarchy");
    }
    std::uint64_t spatialCells = 1;
    for (std::uint8_t level = 0; level < id.level; ++level) {
        const auto next = checkedMultiply(spatialCells, std::uint64_t{8});
        if (!next) {
            spatialCells = std::numeric_limits<std::uint64_t>::max();
            break;
        }
        spatialCells = *next;
    }
    // COPC/EPT expose spatial query domains rather than a finite local page
    // table. Every in-range node remains a valid query; an empty region is
    // discovered by loadNode() and is not a local-page-style phantom node.
    const bool terminalLevel = id.level == maximumLevel_;
    const bool capped = selectedPointCount_ < metadata_.sourcePointCount;
    return {
        .id = id,
        .bounds = pointCloudNodeBounds(metadata_.sourceBounds, id),
        .geometricError = resolution(id),
        .estimatedPointCount = std::min<std::uint64_t>(
            pointsPerNode_,
            (selectedPointCount_ + spatialCells - 1U) / spatialCells),
        .leaf = terminalLevel && !capped,
        .detailLimited = terminalLevel && capped,
    };
}

PointCloudNodePayloadPtr
PdalHierarchicalPointSource::loadNode(const PointCloudNodeId id,
                                      const std::stop_token stopToken) const
{
    const PointCloudNode descriptor = node(id);
    requests_.fetch_add(1, std::memory_order_relaxed);
    saturatedAtomicAdd(estimatedDecodedBytesRequested_,
                       estimatedDecodedBytes(descriptor.estimatedPointCount));
    const auto queryStarted = std::chrono::steady_clock::now();
    const auto recordDuration = [this, queryStarted] {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - queryStarted);
        saturatedAtomicAdd(totalQueryNanoseconds_,
                           static_cast<std::uint64_t>(elapsed.count()));
    };
    if (stopToken.stop_requested()) {
        cancelled_.fetch_add(1, std::memory_order_relaxed);
        recordDuration();
        throw PointCloudDataSourceCancelled();
    }

    std::uint64_t sourcePoints = 0;
    try {
        pdal::StageFactory factory;
        pdal::Stage *reader = factory.createStage(metadata_.sourceDriver);
        if (!reader) {
            throw std::runtime_error("PDAL reader is unavailable: " +
                                     metadata_.sourceDriver);
        }
        pdal::Options options;
        options.add("filename", metadata_.sourcePath.string());
        options.add("bounds", pdalBounds(descriptor.bounds));
        if (!descriptor.leaf ||
            selectedPointCount_ < metadata_.sourcePointCount) {
            options.add("resolution", descriptor.geometricError);
        }
        if (metadata_.sourceDriver == "readers.copc") {
            options.add("requests", 2);
            options.add("keep_alive", 2);
        }
        reader->setOptions(options);

        std::vector<PointSample> samples;
        samples.reserve(pointsPerNode_);
        const std::uint64_t seed = nodeSeed(id);

        pdal::StreamCallbackFilter callback;
        callback.setInput(*reader);
        callback.setCallback([&](pdal::PointRef &point) {
            if (stopToken.stop_requested()) {
                throw PointCloudDataSourceCancelled();
            }
            PointSample sample = mapPdalPoint(point, metadata_);
            if (!contains(id, sample.position)) {
                return false;
            }
            ++sourcePoints;
            if (samples.size() < pointsPerNode_) {
                samples.push_back(sample);
                return true;
            }
            const std::uint64_t candidate =
                splitMix64(seed + sourcePoints) % sourcePoints;
            if (candidate < samples.size()) {
                samples[static_cast<std::size_t>(candidate)] = sample;
            }
            return true;
        });

        pdal::FixedPointTable table(4096);
        callback.prepare(table);
        if (!callback.pipelineStreamable()) {
            throw std::runtime_error("PDAL hierarchy query is not streamable");
        }
        callback.execute(table);

        auto payload = std::make_shared<PointCloudNodePayload>();
        payload->nodeId = id;
        payload->sourcePointCount = sourcePoints;
        if (!samples.empty()) {
            BlockPartitioner partitioner(descriptor.bounds,
                                         samples.size(),
                                         [&payload](PointBlockPtr block) {
                                             payload->blocks.push_back(
                                                 std::move(block));
                                         });
            for (const PointSample &sample : samples) {
                partitioner.add(sample);
            }
            partitioner.finish();
        }
        const std::uint64_t decodedPoints =
            pointCloudNodePayloadPoints(*payload);
        completed_.fetch_add(1, std::memory_order_relaxed);
        saturatedAtomicAdd(sourcePointsVisited_, sourcePoints);
        saturatedAtomicAdd(decodedPointsProduced_, decodedPoints);
        saturatedAtomicAdd(decodedBytesProduced_,
                           pointCloudNodePayloadBytes(*payload));
        recordDuration();
        return payload;
    } catch (const PointCloudDataSourceCancelled &) {
        cancelled_.fetch_add(1, std::memory_order_relaxed);
        saturatedAtomicAdd(sourcePointsVisited_, sourcePoints);
        recordDuration();
        throw;
    } catch (const std::exception &error) {
        failed_.fetch_add(1, std::memory_order_relaxed);
        saturatedAtomicAdd(sourcePointsVisited_, sourcePoints);
        recordDuration();
        throw std::runtime_error("Could not load hierarchy node from '" +
                                 metadata_.sourcePath.string() +
                                 "': " + error.what());
    }
}

PointCloudDataSourceMetrics PdalHierarchicalPointSource::metrics() const
{
    return {
        .requests = requests_.load(std::memory_order_relaxed),
        .completed = completed_.load(std::memory_order_relaxed),
        .cancelled = cancelled_.load(std::memory_order_relaxed),
        .failed = failed_.load(std::memory_order_relaxed),
        .estimatedDecodedBytesRequested =
            estimatedDecodedBytesRequested_.load(std::memory_order_relaxed),
        .decodedBytesProduced =
            decodedBytesProduced_.load(std::memory_order_relaxed),
        .sourcePointsVisited =
            sourcePointsVisited_.load(std::memory_order_relaxed),
        .decodedPointsProduced =
            decodedPointsProduced_.load(std::memory_order_relaxed),
        .totalQueryNanoseconds =
            totalQueryNanoseconds_.load(std::memory_order_relaxed),
        .fetchedBytes = 0,
        .fetchedBytesKnown = false,
    };
}

bool PdalHierarchicalPointSource::detailLimited() const noexcept
{
    return selectedPointCount_ < metadata_.sourcePointCount;
}

std::uint8_t PdalHierarchicalPointSource::maximumLevel() const noexcept
{
    return maximumLevel_;
}

double PdalHierarchicalPointSource::resolution(const PointCloudNodeId id) const
{
    const double extent =
        std::max(metadata_.sourceBounds.maximumExtent(), 1e-9);
    const double rootResolution =
        extent / std::sqrt(static_cast<double>(pointsPerNode_));
    return rootResolution / static_cast<double>(std::uint64_t{1} << id.level);
}

bool PdalHierarchicalPointSource::contains(const PointCloudNodeId id,
                                           const Vec3d &position) const noexcept
{
    const Bounds3d bounds = pointCloudNodeBounds(metadata_.sourceBounds, id);
    const std::uint64_t cells = std::uint64_t{1} << id.level;
    const std::array<double, 3> components{position.x, position.y, position.z};
    const std::array<std::uint32_t, 3> indices{id.x, id.y, id.z};
    for (std::size_t axis = 0; axis < components.size(); ++axis) {
        if (components[axis] < bounds.minimum[axis]) {
            return false;
        }
        const bool lastCell = indices[axis] + 1U == cells;
        if (lastCell) {
            if (components[axis] > bounds.maximum[axis]) {
                return false;
            }
        } else if (components[axis] >= bounds.maximum[axis]) {
            return false;
        }
    }
    return true;
}

} // namespace pci
