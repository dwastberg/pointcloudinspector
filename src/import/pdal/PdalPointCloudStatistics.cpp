#include "import/pdal/PdalPointCloudStatistics.h"

#include "pointcloud/PointCloudMetadata.h"

#include <pdal/Dimension.hpp>
#include <pdal/Options.hpp>
#include <pdal/PointRef.hpp>
#include <pdal/PointTable.hpp>
#include <pdal/StageFactory.hpp>
#include <pdal/filters/StreamCallbackFilter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace pci {
namespace {

struct Position {
    std::array<double, 3> value{};
};

class NumericAccumulator final {
public:
    void add(const double value)
    {
        if (!std::isfinite(value)) {
            return;
        }
        if (count_ == 0) {
            minimum_ = value;
            maximum_ = value;
        } else {
            minimum_ = std::min(minimum_, value);
            maximum_ = std::max(maximum_, value);
        }
        ++count_;
        const double delta = value - mean_;
        mean_ += delta / static_cast<double>(count_);
        const double deltaAfterMean = value - mean_;
        sumSquaredDelta_ += delta * deltaAfterMean;
    }

    [[nodiscard]] NumericPointStatistics finish() const
    {
        return {
            .count = count_,
            .minimum = minimum_,
            .maximum = maximum_,
            .mean = mean_,
            .standardDeviation =
                count_ > 0
                    ? std::sqrt(sumSquaredDelta_ / static_cast<double>(count_))
                    : 0.0,
        };
    }

private:
    std::uint64_t count_ = 0;
    double minimum_ = 0.0;
    double maximum_ = 0.0;
    double mean_ = 0.0;
    double sumSquaredDelta_ = 0.0;
};

std::uint64_t splitMix64(std::uint64_t value) noexcept
{
    value += std::uint64_t{0x9e3779b97f4a7c15};
    value = (value ^ (value >> 30U)) * std::uint64_t{0xbf58476d1ce4e5b9};
    value = (value ^ (value >> 27U)) * std::uint64_t{0x94d049bb133111eb};
    return value ^ (value >> 31U);
}

double squaredDistance(const Position &left, const Position &right) noexcept
{
    double result = 0.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double difference = left.value[axis] - right.value[axis];
        result += difference * difference;
    }
    return result;
}

class KdTree final {
public:
    explicit KdTree(const std::vector<Position> &positions)
        : positions_(positions)
        , indices_(positions.size())
    {
        std::iota(indices_.begin(), indices_.end(), std::size_t{0});
        nodes_.reserve(positions.size());
        root_ = build(0, indices_.size(), 0);
    }

    [[nodiscard]] double
    meanNeighbourDistance(const std::size_t targetIndex,
                          const std::size_t neighbourCount) const
    {
        std::priority_queue<double> nearestSquared;
        query(root_, targetIndex, neighbourCount, nearestSquared);
        if (nearestSquared.empty()) {
            return 0.0;
        }
        double sum = 0.0;
        const std::size_t count = nearestSquared.size();
        while (!nearestSquared.empty()) {
            sum += std::sqrt(nearestSquared.top());
            nearestSquared.pop();
        }
        return sum / static_cast<double>(count);
    }

private:
    struct Node {
        std::size_t pointIndex = 0;
        std::size_t axis = 0;
        int left = -1;
        int right = -1;
    };

    int build(const std::size_t begin,
              const std::size_t end,
              const std::size_t depth)
    {
        if (begin >= end) {
            return -1;
        }
        const std::size_t axis = depth % 3;
        const std::size_t middle = begin + (end - begin) / 2;
        std::nth_element(
            indices_.begin() + static_cast<std::ptrdiff_t>(begin),
            indices_.begin() + static_cast<std::ptrdiff_t>(middle),
            indices_.begin() + static_cast<std::ptrdiff_t>(end),
            [this, axis](const std::size_t left, const std::size_t right) {
                return positions_[left].value[axis] <
                       positions_[right].value[axis];
            });
        const int nodeIndex = static_cast<int>(nodes_.size());
        nodes_.push_back({
            .pointIndex = indices_[middle],
            .axis = axis,
        });
        const int left = build(begin, middle, depth + 1);
        const int right = build(middle + 1, end, depth + 1);
        nodes_[static_cast<std::size_t>(nodeIndex)].left = left;
        nodes_[static_cast<std::size_t>(nodeIndex)].right = right;
        return nodeIndex;
    }

    void query(const int nodeIndex,
               const std::size_t targetIndex,
               const std::size_t neighbourCount,
               std::priority_queue<double> &nearestSquared) const
    {
        if (nodeIndex < 0) {
            return;
        }
        const Node &node = nodes_[static_cast<std::size_t>(nodeIndex)];
        const Position &target = positions_[targetIndex];
        const Position &candidate = positions_[node.pointIndex];
        if (node.pointIndex != targetIndex) {
            const double distance = squaredDistance(target, candidate);
            if (nearestSquared.size() < neighbourCount) {
                nearestSquared.push(distance);
            } else if (distance < nearestSquared.top()) {
                nearestSquared.pop();
                nearestSquared.push(distance);
            }
        }

        const double axisDifference =
            target.value[node.axis] - candidate.value[node.axis];
        const int nearNode = axisDifference < 0.0 ? node.left : node.right;
        const int farNode = axisDifference < 0.0 ? node.right : node.left;
        query(nearNode, targetIndex, neighbourCount, nearestSquared);
        if (nearestSquared.size() < neighbourCount ||
            axisDifference * axisDifference < nearestSquared.top()) {
            query(farNode, targetIndex, neighbourCount, nearestSquared);
        }
    }

    const std::vector<Position> &positions_;
    std::vector<std::size_t> indices_;
    std::vector<Node> nodes_;
    int root_ = -1;
};

std::optional<SpatialOutlierStatistics>
calculateOutliers(const std::vector<Position> &positions,
                  const std::uint64_t sourcePointCount,
                  const std::stop_token stopToken)
{
    if (positions.size() < 2) {
        return std::nullopt;
    }
    const std::size_t neighbourCount =
        std::min(PdalPointCloudStatistics::defaultOutlierNeighbourCount,
                 positions.size() - 1);
    KdTree tree(positions);
    std::vector<double> neighbourDistances;
    neighbourDistances.reserve(positions.size());
    NumericAccumulator distances;
    for (std::size_t index = 0; index < positions.size(); ++index) {
        if ((index & 1023U) == 0U && stopToken.stop_requested()) {
            throw PointCloudStatisticsCancelled();
        }
        const double distance =
            tree.meanNeighbourDistance(index, neighbourCount);
        neighbourDistances.push_back(distance);
        distances.add(distance);
    }
    const NumericPointStatistics distribution = distances.finish();
    const double threshold =
        distribution.mean + 3.0 * distribution.standardDeviation;
    const std::uint64_t outlierCount =
        static_cast<std::uint64_t>(std::ranges::count_if(
            neighbourDistances, [threshold](const double distance) {
                return distance > threshold;
            }));
    const double sampleCount = static_cast<double>(positions.size());
    const double proportion = static_cast<double>(outlierCount) / sampleCount;

    // Wilson score interval for a binomial proportion, using z = 1.96.
    constexpr double z = 1.96;
    const double denominator = 1.0 + z * z / sampleCount;
    const double centre =
        (proportion + z * z / (2.0 * sampleCount)) / denominator;
    const double halfWidth =
        z / denominator *
        std::sqrt(proportion * (1.0 - proportion) / sampleCount +
                  z * z / (4.0 * sampleCount * sampleCount));
    const auto scaledCount = [sourcePointCount](const double value) {
        return static_cast<std::uint64_t>(
            std::llround(std::clamp(value, 0.0, 1.0) *
                         static_cast<double>(sourcePointCount)));
    };

    return SpatialOutlierStatistics{
        .samplePointCount = static_cast<std::uint64_t>(positions.size()),
        .neighbourCount = neighbourCount,
        .meanNeighbourDistance = distribution.mean,
        .neighbourDistanceStandardDeviation = distribution.standardDeviation,
        .distanceThreshold = threshold,
        .sampleOutlierCount = outlierCount,
        .estimatedPercentage = proportion * 100.0,
        .estimatedSourceOutlierCount = scaledCount(proportion),
        .confidenceLowerCount = scaledCount(centre - halfWidth),
        .confidenceUpperCount = scaledCount(centre + halfWidth),
    };
}

std::optional<double> positiveProduct(const double left, const double right)
{
    if (!(left > 0.0) || !(right > 0.0) ||
        left > std::numeric_limits<double>::max() / right) {
        return std::nullopt;
    }
    const double product = left * right;
    return std::isfinite(product) ? std::optional<double>(product)
                                  : std::nullopt;
}

} // namespace

PointCloudStatistics
PdalPointCloudStatistics::calculate(const PointCloudMetadata &metadata,
                                    const std::stop_token stopToken,
                                    Progress progress) const
{
    if (metadata.sourcePath.empty()) {
        throw std::invalid_argument(
            "point-cloud statistics require a source path");
    }
    if (metadata.sourceDriver.empty()) {
        throw std::invalid_argument(
            "point-cloud statistics require a source driver");
    }

    PointCloudStatistics result{
        .sourcePointCount = metadata.sourcePointCount,
        .scannedPointCount = 0,
        .x = {},
        .y = {},
        .z = {},
        .intensity = std::nullopt,
        .red = std::nullopt,
        .green = std::nullopt,
        .blue = std::nullopt,
        .classificationCounts = {},
        .returnNumberCounts = {},
        .numberOfReturnsCounts = {},
        .hasClassification = metadata.hasClassification,
        .hasReturnNumber = metadata.hasReturnNumber,
        .hasNumberOfReturns = metadata.hasNumberOfReturns,
        .horizontalBoundingArea = std::nullopt,
        .boundingVolume = std::nullopt,
        .horizontalDensity = std::nullopt,
        .volumetricDensity = std::nullopt,
        .nominalHorizontalSpacing = std::nullopt,
        .spatialOutliers = std::nullopt,
    };
    NumericAccumulator x;
    NumericAccumulator y;
    NumericAccumulator z;
    NumericAccumulator intensity;
    NumericAccumulator red;
    NumericAccumulator green;
    NumericAccumulator blue;
    std::vector<Position> outlierSample;
    outlierSample.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
        metadata.sourcePointCount, maximumOutlierSamplePoints)));

    try {
        pdal::StageFactory factory;
        pdal::Stage *reader = factory.createStage(metadata.sourceDriver);
        if (!reader) {
            throw std::runtime_error("PDAL reader is unavailable: " +
                                     metadata.sourceDriver);
        }
        pdal::Options options;
        options.add("filename", metadata.sourcePath.string());
        if (metadata.sourceDriver == "readers.copc") {
            options.add("requests", 2);
            options.add("keep_alive", 2);
        }
        reader->setOptions(options);

        pdal::StreamCallbackFilter callback;
        callback.setInput(*reader);
        callback.setCallback([&](pdal::PointRef &point) {
            if (stopToken.stop_requested()) {
                throw PointCloudStatisticsCancelled();
            }
            const Position position{{
                point.getFieldAs<double>(pdal::Dimension::Id::X),
                point.getFieldAs<double>(pdal::Dimension::Id::Y),
                point.getFieldAs<double>(pdal::Dimension::Id::Z),
            }};
            x.add(position.value[0]);
            y.add(position.value[1]);
            z.add(position.value[2]);
            if (metadata.hasIntensity) {
                intensity.add(
                    point.getFieldAs<double>(pdal::Dimension::Id::Intensity));
            }
            if (metadata.hasColor) {
                red.add(point.getFieldAs<double>(pdal::Dimension::Id::Red));
                green.add(point.getFieldAs<double>(pdal::Dimension::Id::Green));
                blue.add(point.getFieldAs<double>(pdal::Dimension::Id::Blue));
            }
            if (metadata.hasClassification) {
                ++result.classificationCounts[point.getFieldAs<std::uint8_t>(
                    pdal::Dimension::Id::Classification)];
            }
            if (metadata.hasReturnNumber) {
                ++result.returnNumberCounts[point.getFieldAs<std::uint8_t>(
                    pdal::Dimension::Id::ReturnNumber)];
            }
            if (metadata.hasNumberOfReturns) {
                ++result.numberOfReturnsCounts[point.getFieldAs<std::uint8_t>(
                    pdal::Dimension::Id::NumberOfReturns)];
            }

            ++result.scannedPointCount;
            if (outlierSample.size() < maximumOutlierSamplePoints) {
                outlierSample.push_back(position);
            } else {
                const std::uint64_t candidate =
                    splitMix64(result.scannedPointCount) %
                    result.scannedPointCount;
                if (candidate < outlierSample.size()) {
                    outlierSample[static_cast<std::size_t>(candidate)] =
                        position;
                }
            }
            if (progress && (result.scannedPointCount % 65'536 == 0)) {
                progress(result.scannedPointCount, metadata.sourcePointCount);
            }
            return true;
        });

        pdal::FixedPointTable table(4096);
        callback.prepare(table);
        if (!callback.pipelineStreamable()) {
            throw std::runtime_error(
                "PDAL pipeline is not streamable for point-cloud statistics");
        }
        callback.execute(table);
    } catch (const PointCloudStatisticsCancelled &) {
        throw;
    } catch (const std::exception &error) {
        throw std::runtime_error("Could not calculate statistics for '" +
                                 metadata.sourcePath.string() +
                                 "': " + error.what());
    }

    if (progress) {
        progress(result.scannedPointCount, metadata.sourcePointCount);
    }
    if (result.scannedPointCount == 0) {
        throw std::runtime_error(
            "Point-cloud statistics source contains no points");
    }

    result.x = x.finish();
    result.y = y.finish();
    result.z = z.finish();
    if (metadata.hasIntensity) {
        result.intensity = intensity.finish();
    }
    if (metadata.hasColor) {
        result.red = red.finish();
        result.green = green.finish();
        result.blue = blue.finish();
    }

    const double xExtent = result.x.maximum - result.x.minimum;
    const double yExtent = result.y.maximum - result.y.minimum;
    const double zExtent = result.z.maximum - result.z.minimum;
    result.horizontalBoundingArea = positiveProduct(xExtent, yExtent);
    if (result.horizontalBoundingArea && zExtent > 0.0 &&
        *result.horizontalBoundingArea <=
            std::numeric_limits<double>::max() / zExtent) {
        result.boundingVolume = *result.horizontalBoundingArea * zExtent;
    }
    if (result.horizontalBoundingArea) {
        result.horizontalDensity =
            static_cast<double>(result.scannedPointCount) /
            *result.horizontalBoundingArea;
        result.nominalHorizontalSpacing =
            std::sqrt(*result.horizontalBoundingArea /
                      static_cast<double>(result.scannedPointCount));
    }
    if (result.boundingVolume) {
        result.volumetricDensity =
            static_cast<double>(result.scannedPointCount) /
            *result.boundingVolume;
    }
    result.spatialOutliers =
        calculateOutliers(outlierSample, result.scannedPointCount, stopToken);
    return result;
}

} // namespace pci
