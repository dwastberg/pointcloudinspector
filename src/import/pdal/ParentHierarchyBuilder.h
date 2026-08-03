#pragma once

#include "import/local/LocalPointPageFormat.h"

#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
#include <vector>

namespace pci::local_index {

class ParentHierarchyBuilder final {
public:
    using AppendPage =
        std::function<LocalPointPageRecord(PointCloudNodeId,
                                           std::span<const PointSample>,
                                           std::uint64_t,
                                           const Bounds3d &)>;

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
