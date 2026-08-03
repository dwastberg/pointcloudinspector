#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>

namespace pci {

struct PointCloudMetadata;

struct NumericPointStatistics {
    std::uint64_t count = 0;
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    double standardDeviation = 0.0;
};

struct SpatialOutlierStatistics {
    std::uint64_t samplePointCount = 0;
    std::size_t neighbourCount = 0;
    double meanNeighbourDistance = 0.0;
    double neighbourDistanceStandardDeviation = 0.0;
    double distanceThreshold = 0.0;
    std::uint64_t sampleOutlierCount = 0;
    double estimatedPercentage = 0.0;
    std::uint64_t estimatedSourceOutlierCount = 0;
    std::uint64_t confidenceLowerCount = 0;
    std::uint64_t confidenceUpperCount = 0;
};

struct PointCloudStatistics {
    std::uint64_t sourcePointCount = 0;
    std::uint64_t scannedPointCount = 0;
    NumericPointStatistics x;
    NumericPointStatistics y;
    NumericPointStatistics z;
    std::optional<NumericPointStatistics> intensity;
    std::optional<NumericPointStatistics> red;
    std::optional<NumericPointStatistics> green;
    std::optional<NumericPointStatistics> blue;
    std::array<std::uint64_t, 256> classificationCounts{};
    std::array<std::uint64_t, 256> returnNumberCounts{};
    std::array<std::uint64_t, 256> numberOfReturnsCounts{};
    bool hasClassification = false;
    bool hasReturnNumber = false;
    bool hasNumberOfReturns = false;
    std::optional<double> horizontalBoundingArea;
    std::optional<double> boundingVolume;
    std::optional<double> horizontalDensity;
    std::optional<double> volumetricDensity;
    std::optional<double> nominalHorizontalSpacing;
    std::optional<SpatialOutlierStatistics> spatialOutliers;
};

class PointCloudStatisticsCancelled final : public std::runtime_error {
public:
    PointCloudStatisticsCancelled()
        : std::runtime_error("point-cloud statistics calculation cancelled")
    {
    }
};

class PointCloudStatisticsProvider {
public:
    using Progress =
        std::function<void(std::uint64_t processed, std::uint64_t total)>;

    virtual ~PointCloudStatisticsProvider() = default;

    [[nodiscard]] virtual PointCloudStatistics
    calculate(const PointCloudMetadata &metadata,
              std::stop_token stopToken = {},
              Progress progress = {}) const = 0;
};

} // namespace pci
