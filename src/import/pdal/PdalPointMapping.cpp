#include "import/pdal/PdalPointMapping.h"

#include "pointcloud/GpuPointProperties.h"

#include <pdal/Dimension.hpp>
#include <pdal/PointRef.hpp>

#include <cstdint>

namespace pci {
namespace {

std::uint8_t color8(const std::uint16_t value)
{
    return static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(value) + 128U) / 257U);
}

} // namespace

PointSample mapPdalPoint(const pdal::PointRef &point,
                         const PointCloudMetadata &metadata)
{
    const auto classification = metadata.hasClassification
                                    ? point.getFieldAs<std::uint8_t>(
                                          pdal::Dimension::Id::Classification)
                                    : std::uint8_t{0};
    const auto intensity =
        metadata.hasIntensity
            ? point.getFieldAs<std::uint16_t>(pdal::Dimension::Id::Intensity)
            : std::uint16_t{0};
    const auto returnNumber =
        metadata.hasReturnNumber
            ? point.getFieldAs<std::uint8_t>(pdal::Dimension::Id::ReturnNumber)
            : std::uint8_t{0};
    const auto numberOfReturns = metadata.hasNumberOfReturns
                                     ? point.getFieldAs<std::uint8_t>(
                                           pdal::Dimension::Id::NumberOfReturns)
                                     : std::uint8_t{0};

    std::uint32_t rgba = 0xffffffffU;
    if (metadata.hasColor) {
        const auto red =
            color8(point.getFieldAs<std::uint16_t>(pdal::Dimension::Id::Red));
        const auto green =
            color8(point.getFieldAs<std::uint16_t>(pdal::Dimension::Id::Green));
        const auto blue =
            color8(point.getFieldAs<std::uint16_t>(pdal::Dimension::Id::Blue));
        rgba = static_cast<std::uint32_t>(red) |
               (static_cast<std::uint32_t>(green) << 8U) |
               (static_cast<std::uint32_t>(blue) << 16U) | 0xff000000U;
    }

    return {
        .position =
            {
                point.getFieldAs<double>(pdal::Dimension::Id::X),
                point.getFieldAs<double>(pdal::Dimension::Id::Y),
                point.getFieldAs<double>(pdal::Dimension::Id::Z),
            },
        .rgba = rgba,
        .packedAttributes = static_cast<std::uint16_t>(
            classification | ((intensity >> 8U) << 8U)),
        .packedProperties =
            packGpuPointProperties(intensity, returnNumber, numberOfReturns),
        .attributes =
            {
                .intensity = intensity,
                .classification = classification,
                .returnNumber = returnNumber,
                .numberOfReturns = numberOfReturns,
            },
    };
}

} // namespace pci
