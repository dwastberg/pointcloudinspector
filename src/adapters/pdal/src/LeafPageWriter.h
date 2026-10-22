#pragma once

#include "MortonRunRecord.h"
#include <pci/adapters/pdal/LocalPointPageSource.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace pci::local_index {

class LeafPageWriter final {
public:
    using OnLeafPage = std::function<void(const LocalPointPageRecord &,
                                          std::span<const PointSample>)>;

    LeafPageWriter(const std::filesystem::path &payloadPath,
                   const LocalPointPageSourcePtr &source);
    ~LeafPageWriter();

    LeafPageWriter(LeafPageWriter &&) noexcept;
    LeafPageWriter &operator=(LeafPageWriter &&) noexcept;
    LeafPageWriter(const LeafPageWriter &) = delete;
    LeafPageWriter &operator=(const LeafPageWriter &) = delete;

    [[nodiscard]] LocalPointPageRecord
    appendPage(PointCloudNodeId id,
               std::span<const PointSample> samples,
               std::uint64_t sourcePointCount,
               std::optional<Bounds3d> hierarchyBounds = std::nullopt);
    [[nodiscard]] std::vector<LocalPointPageRecord>
    writeLeafPages(std::span<const MortonRunRecord> records,
                   std::uint8_t level,
                   std::uint32_t pointsPerLeaf,
                   std::stop_token stopToken,
                   const std::function<void()> &heartbeat,
                   const OnLeafPage &onLeafPage = {});
    [[nodiscard]] std::vector<LocalPointPageRecord>
    mergeRuns(const std::vector<std::filesystem::path> &runPaths,
              std::uint8_t level,
              std::uint32_t pointsPerLeaf,
              std::stop_token stopToken,
              const std::function<void()> &heartbeat,
              const OnLeafPage &onLeafPage = {});
    void close();
    [[nodiscard]] std::uint64_t bytesWritten() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pci::local_index
