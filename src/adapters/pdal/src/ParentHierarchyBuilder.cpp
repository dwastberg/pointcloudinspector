#include "ParentHierarchyBuilder.h"

#include <pci/foundation/CheckedArithmetic.h>
#include <pci/operations/PointCloudImport.h>
#include <pci/pointcloud/GpuPointProperties.h>

#include <QByteArrayView>
#include <QCryptographicHash>

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace pci::local_index {

std::uint64_t splitMix64(std::uint64_t value) noexcept
{
    value += std::uint64_t{0x9e3779b97f4a7c15};
    value = (value ^ (value >> 30U)) * std::uint64_t{0xbf58476d1ce4e5b9};
    value = (value ^ (value >> 27U)) * std::uint64_t{0x94d049bb133111eb};
    return value ^ (value >> 31U);
}

void ParentAccumulator::init(const PointCloudNodeId parentId,
                             const std::uint32_t capacity)
{
    id = parentId;
    samples.reserve(capacity);
    seed = splitMix64(id.level ^ (static_cast<std::uint64_t>(id.x) << 8U) ^
                      (static_cast<std::uint64_t>(id.y) << 29U) ^
                      (static_cast<std::uint64_t>(id.z) << 50U));
}

void ParentAccumulator::addSample(const PointSample &sample,
                                  const std::uint32_t maximumPoints)
{
    ++visited;
    if (samples.size() < maximumPoints) {
        samples.push_back(sample);
    } else {
        const std::uint64_t candidate = splitMix64(seed + visited) % visited;
        if (candidate < samples.size()) {
            samples[static_cast<std::size_t>(candidate)] = sample;
        }
    }
}

void ParentAccumulator::extendBounds(const Bounds3d &childBounds)
{
    if (!hasBounds) {
        bounds = childBounds;
        hasBounds = true;
    } else {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            bounds.minimum[axis] =
                std::min(bounds.minimum[axis], childBounds.minimum[axis]);
            bounds.maximum[axis] =
                std::max(bounds.maximum[axis], childBounds.maximum[axis]);
        }
    }
}

void accumulateChildPage(ParentAccumulator &parent,
                         const LocalPointPageRecord &childPage,
                         const std::span<const PointSample> childSamples,
                         const std::uint32_t parentMaxPoints)
{
    parent.sourcePoints =
        saturatingAdd(parent.sourcePoints, childPage.sourcePointCount);
    parent.extendBounds(childPage.tightBounds);
    for (const PointSample &sample : childSamples) {
        parent.addSample(sample, parentMaxPoints);
    }
}

void accumulateChildPage(ParentMap &parents,
                         const LocalPointPageRecord &childPage,
                         const std::span<const PointSample> childSamples,
                         const std::uint32_t parentMaxPoints)
{
    const PointCloudNodeId pid = ParentHierarchyBuilder::parentId(childPage.id);
    auto it = parents.find(pid);
    if (it == parents.end()) {
        it = parents.emplace(pid, ParentAccumulator{}).first;
        it->second.init(pid, parentMaxPoints);
    }
    accumulateChildPage(it->second, childPage, childSamples, parentMaxPoints);
}

PointCloudNodeId
ParentHierarchyBuilder::parentId(const PointCloudNodeId child) noexcept
{
    return {
        .level = static_cast<std::uint8_t>(child.level - 1U),
        .x = child.x >> 1U,
        .y = child.y >> 1U,
        .z = child.z >> 1U,
    };
}

std::vector<PointSample> ParentHierarchyBuilder::sampleChildrenDirect(
    const std::span<const std::span<const PointSample>> childSamples,
    const std::uint32_t maximumPoints,
    const PointCloudNodeId parent)
{
    std::vector<PointSample> samples;
    samples.reserve(maximumPoints);
    std::uint64_t visited = 0;
    const std::uint64_t seed =
        splitMix64(parent.level ^ (static_cast<std::uint64_t>(parent.x) << 8U) ^
                   (static_cast<std::uint64_t>(parent.y) << 29U) ^
                   (static_cast<std::uint64_t>(parent.z) << 50U));
    for (const auto &childSpan : childSamples) {
        for (const PointSample &sample : childSpan) {
            ++visited;
            if (samples.size() < maximumPoints) {
                samples.push_back(sample);
            } else {
                const std::uint64_t candidate =
                    splitMix64(seed + visited) % visited;
                if (candidate < samples.size()) {
                    samples[static_cast<std::size_t>(candidate)] = sample;
                }
            }
        }
    }
    return samples;
}

std::vector<PointSample> ParentHierarchyBuilder::sampleChildren(
    const std::filesystem::path &payloadPath,
    const std::span<const LocalPointPageRecord> children,
    const std::uint32_t maximumPoints,
    const PointCloudNodeId parent,
    const std::stop_token stopToken,
    const std::function<void()> &heartbeat)
{
    std::ifstream input(payloadPath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not reopen local point payload");
    }
    std::vector<PointSample> samples;
    samples.reserve(maximumPoints);
    std::uint64_t visited = 0;
    const std::uint64_t seed =
        splitMix64(parent.level ^ (static_cast<std::uint64_t>(parent.x) << 8U) ^
                   (static_cast<std::uint64_t>(parent.y) << 29U) ^
                   (static_cast<std::uint64_t>(parent.z) << 50U));
    std::vector<GpuPoint> pageBuffer;
    for (const LocalPointPageRecord &child : children) {
        if (stopToken.stop_requested()) {
            throw PointCloudImportCancelled();
        }
        heartbeat();
        input.clear();
        input.seekg(static_cast<std::streamoff>(child.payloadOffset));
        pageBuffer.resize(child.pointCount);
        const auto byteSpan = std::as_bytes(std::span(pageBuffer));
        input.read(reinterpret_cast<char *>(pageBuffer.data()),
                   static_cast<std::streamsize>(byteSpan.size()));
        if (!input) {
            throw std::runtime_error(
                "could not read child page while building parent");
        }
        const auto digest = localPointPayloadDigest(byteSpan);
        if (digest != child.payloadDigest) {
            throw std::runtime_error(
                "child digest changed while building local page parent");
        }
        const double extent = std::max(child.tightBounds.maximumExtent(), 1e-9);
        const double scale = blockScaleForEdge(extent);
        const Vec3d origin{
            child.tightBounds.minimum[0],
            child.tightBounds.minimum[1],
            child.tightBounds.minimum[2],
        };
        for (const GpuPoint &pt : pageBuffer) {
            PointSample sample{
                .position =
                    {
                        origin.x + static_cast<double>(pt.x) * scale,
                        origin.y + static_cast<double>(pt.y) * scale,
                        origin.z + static_cast<double>(pt.z) * scale,
                    },
                .rgba = pt.rgba,
                .packedAttributes = pt.attributes,
                .packedProperties = pt.packedProperties,
                .attributes =
                    {
                        .intensity = gpuPointIntensity(pt.packedProperties),
                        .classification =
                            static_cast<std::uint8_t>(pt.attributes & 0xffU),
                        .returnNumber =
                            gpuPointReturnNumber(pt.packedProperties),
                        .numberOfReturns =
                            gpuPointNumberOfReturns(pt.packedProperties),
                    },
            };
            ++visited;
            if (samples.size() < maximumPoints) {
                samples.push_back(sample);
            } else {
                const std::uint64_t candidate =
                    splitMix64(seed + visited) % visited;
                if (candidate < samples.size()) {
                    samples[static_cast<std::size_t>(candidate)] = sample;
                }
            }
        }
    }
    return samples;
}

std::vector<LocalPointPageRecord> ParentHierarchyBuilder::buildLevel(
    const std::filesystem::path &payloadPath,
    const std::span<const LocalPointPageRecord> children,
    const std::uint8_t parentLevel,
    const std::uint32_t maximumPoints,
    const AppendPage &appendPage,
    const std::stop_token stopToken,
    const std::function<void()> &heartbeat)
{
    std::unordered_map<PointCloudNodeId,
                       std::vector<LocalPointPageRecord>,
                       PointCloudNodeIdHash>
        grouped;
    for (const LocalPointPageRecord &child : children) {
        grouped[parentId(child.id)].push_back(child);
    }
    std::vector<PointCloudNodeId> parents;
    parents.reserve(grouped.size());
    for (const auto &[id, pages] : grouped) {
        static_cast<void>(pages);
        if (id.level == parentLevel) {
            parents.push_back(id);
        }
    }
    std::ranges::sort(parents);

    std::vector<LocalPointPageRecord> result;
    result.reserve(parents.size());
    for (const PointCloudNodeId id : parents) {
        if (stopToken.stop_requested()) {
            throw PointCloudImportCancelled();
        }
        auto &childPages = grouped.at(id);
        std::ranges::sort(childPages, [](const auto &left, const auto &right) {
            return left.id < right.id;
        });
        std::uint64_t sourcePoints = 0;
        Bounds3d hierarchyBounds;
        bool firstBounds = true;
        for (const LocalPointPageRecord &child : childPages) {
            sourcePoints = saturatingAdd(sourcePoints, child.sourcePointCount);
            if (firstBounds) {
                hierarchyBounds = child.tightBounds;
                firstBounds = false;
            } else {
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    hierarchyBounds.minimum[axis] =
                        std::min(hierarchyBounds.minimum[axis],
                                 child.tightBounds.minimum[axis]);
                    hierarchyBounds.maximum[axis] =
                        std::max(hierarchyBounds.maximum[axis],
                                 child.tightBounds.maximum[axis]);
                }
            }
        }
        const std::vector<PointSample> samples = sampleChildren(
            payloadPath, childPages, maximumPoints, id, stopToken, heartbeat);
        result.push_back(
            appendPage(id, samples, sourcePoints, hierarchyBounds));
    }
    return result;
}

} // namespace pci::local_index
