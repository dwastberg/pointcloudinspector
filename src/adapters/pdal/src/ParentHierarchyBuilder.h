#pragma once

#include <pci/adapters/pdal/LocalPointPageFormat.h>

#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace pci::local_index {

[[nodiscard]] std::uint64_t splitMix64(std::uint64_t value) noexcept;

struct ParentAccumulator {
    PointCloudNodeId id{};
    std::uint64_t sourcePoints = 0;
    Bounds3d bounds{};
    bool hasBounds = false;
    std::vector<PointSample> samples;
    std::uint64_t visited = 0;
    std::uint64_t seed = 0;

    void init(PointCloudNodeId parentId, std::uint32_t capacity);
    void addSample(const PointSample &sample, std::uint32_t maximumPoints);
    void extendBounds(const Bounds3d &childBounds);
};

using ParentMap = std::
    unordered_map<PointCloudNodeId, ParentAccumulator, PointCloudNodeIdHash>;

void accumulateChildPage(ParentMap &parents,
                         const LocalPointPageRecord &childPage,
                         std::span<const PointSample> childSamples,
                         std::uint32_t parentMaxPoints);

void accumulateChildPage(ParentAccumulator &parent,
                         const LocalPointPageRecord &childPage,
                         std::span<const PointSample> childSamples,
                         std::uint32_t parentMaxPoints);

class ParentHierarchyBuilder final {
public:
    using AppendPage =
        std::function<LocalPointPageRecord(PointCloudNodeId,
                                           std::span<const PointSample>,
                                           std::uint64_t,
                                           const Bounds3d &)>;

    [[nodiscard]] static PointCloudNodeId
    parentId(PointCloudNodeId child) noexcept;

    [[nodiscard]] static std::vector<PointSample> sampleChildrenDirect(
        std::span<const std::span<const PointSample>> childSamples,
        std::uint32_t maximumPoints,
        PointCloudNodeId parent);

    [[nodiscard]] static std::vector<PointSample>
    sampleChildren(const std::filesystem::path &payloadPath,
                   std::span<const LocalPointPageRecord> children,
                   std::uint32_t maximumPoints,
                   PointCloudNodeId parent,
                   std::stop_token stopToken,
                   const std::function<void()> &heartbeat);

    [[nodiscard]] static std::vector<LocalPointPageRecord>
    buildLevel(const std::filesystem::path &payloadPath,
               std::span<const LocalPointPageRecord> children,
               std::uint8_t parentLevel,
               std::uint32_t maximumPoints,
               const AppendPage &appendPage,
               std::stop_token stopToken,
               const std::function<void()> &heartbeat);
};

} // namespace pci::local_index
