#pragma once

#include "import/PointCloudStatistics.h"

#include <cstddef>

namespace pci {

class PdalPointCloudStatistics final : public PointCloudStatisticsProvider {
public:
    static constexpr std::size_t maximumOutlierSamplePoints = 50'000;
    static constexpr std::size_t defaultOutlierNeighbourCount = 8;

    [[nodiscard]] PointCloudStatistics
    calculate(const PointCloudMetadata &metadata,
              std::stop_token stopToken = {},
              Progress progress = {}) const override;
};

} // namespace pci
