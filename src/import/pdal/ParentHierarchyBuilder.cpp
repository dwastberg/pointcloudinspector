#include "import/pdal/ParentHierarchyBuilder.h"

#include "foundation/CheckedArithmetic.h"
#include "import/PointCloudImport.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace pci::local_index {
namespace {

std::uint64_t splitMix64(std::uint64_t value) noexcept
{
    value += std::uint64_t{0x9e3779b97f4a7c15};
    value = (value ^ (value >> 30U)) * std::uint64_t{0xbf58476d1ce4e5b9};
    value = (value ^ (value >> 27U)) * std::uint64_t{0x94d049bb133111eb};
    return value ^ (value >> 31U);
}

PointCloudNodeId parentId(const PointCloudNodeId child) noexcept
{
    return {
        .level = static_cast<std::uint8_t>(child.level - 1U),
        .x = child.x >> 1U,
        .y = child.y >> 1U,
        .z = child.z >> 1U,
    };
}

} // namespace

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
    std::array<std::byte, localPointDiskBytes> bytes{};
    for (const LocalPointPageRecord &child : children) {
        input.clear();
        input.seekg(static_cast<std::streamoff>(child.payloadOffset));
        std::uint32_t checksum = 0;
        for (std::uint64_t index = 0; index < child.pointCount; ++index) {
            if ((visited & 0xffffU) == 0) {
                if (stopToken.stop_requested()) {
                    throw PointCloudImportCancelled();
                }
                heartbeat();
            }
            input.read(reinterpret_cast<char *>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            if (!input) {
                throw std::runtime_error(
                    "could not read child page while building parent");
            }
            checksum = localPointCrc32(bytes, checksum);
            PointSample sample = decodeLocalPoint(bytes);
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
        if (checksum != child.payloadChecksum) {
            throw std::runtime_error(
                "child checksum changed while building local page parent");
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
