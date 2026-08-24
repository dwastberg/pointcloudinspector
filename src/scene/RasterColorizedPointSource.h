#pragma once

#include "scene/RasterPointColorize.h"

#include <atomic>

namespace pci {

class RasterColorizedPointSource final : public PointCloudDataSource {
public:
    RasterColorizedPointSource(PointCloudDataSourcePtr source,
                               std::vector<std::uint32_t> colors,
                               std::vector<RasterColorizeRange> ranges,
                               PointMemoryBudget::ReservationPtr reservation);

    [[nodiscard]] const PointCloudDataSourcePtr &base() const noexcept;
    [[nodiscard]] std::uint64_t byteSize() const noexcept;
    [[nodiscard]] std::uint64_t countMismatchCount() const noexcept;

    [[nodiscard]] PointCloudNode rootNode() const override;
    [[nodiscard]] PointCloudNode node(PointCloudNodeId id) const override;
    [[nodiscard]] PointCloudNodePayloadPtr
    loadNode(PointCloudNodeId id, std::stop_token stopToken) const override;
    [[nodiscard]] PointCloudDataSourceMetrics metrics() const override;
    [[nodiscard]] PointCloudStorageMetrics storageMetrics() const override;
    [[nodiscard]] PointCloudScalarRanges scalarRanges() const override;
    [[nodiscard]] std::optional<PointCloudFullDetailInfo>
    fullDetailInfo() const override;
    [[nodiscard]] std::optional<std::vector<PointCloudStoredNode>>
    storedNodeIndex() const override;

private:
    PointCloudDataSourcePtr source_;
    std::vector<std::uint32_t> colors_;
    std::vector<RasterColorizeRange> ranges_;
    PointMemoryBudget::ReservationPtr reservation_;
    mutable std::atomic_uint64_t countMismatches_ = 0;
};

} // namespace pci
