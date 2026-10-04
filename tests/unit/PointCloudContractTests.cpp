#include <pci/pointcloud/GpuPoint.h>
#include <pci/pointcloud/GpuPointProperties.h>
#include <pci/pointcloud/PointAttributes.h>
#include <pci/pointcloud/PointClassificationFilter.h>
#include <pci/pointcloud/PointCloudMetadata.h>
#include <pci/pointcloud/SourcePoint.h>

#include <pci/pointcloud/PointIdentity.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <type_traits>

namespace {

static_assert(std::is_constructible_v<pci::PointCloudSourceId, std::uint64_t>);
static_assert(!std::is_convertible_v<std::uint64_t, pci::PointCloudSourceId>);

TEST_CASE("point attribute defaults are neutral", "[unit][pointcloud]")
{
    const pci::PointAttributes attributes;
    CHECK(attributes.intensity == 0);
    CHECK(attributes.classification == 0);
    CHECK(attributes.returnNumber == 0);
    CHECK(attributes.numberOfReturns == 0);
    static_assert(std::is_trivially_copyable_v<pci::PointAttributes>);
    static_assert(sizeof(pci::PointAttributes) <= 8);
}

TEST_CASE("classification filters address all 256 class codes",
          "[unit][pointcloud][classification]")
{
    pci::PointClassificationFilter filter;
    CHECK(filter.allVisible());
    CHECK(filter.visibleCount() == pci::pointClassificationCount);

    for (const std::uint8_t classification : {std::uint8_t{0},
                                              std::uint8_t{31},
                                              std::uint8_t{32},
                                              std::uint8_t{255}}) {
        filter.setVisible(classification, false);
        CHECK_FALSE(filter.isVisible(classification));
    }
    CHECK(filter.visibleCount() == 252);

    filter.setAllVisible(false);
    CHECK_FALSE(filter.anyVisible());
    filter.setVisible(255, true);
    CHECK(filter.isVisible(255));
    CHECK(filter.visibleCount() == 1);
}

TEST_CASE("source point defaults remain renderable", "[unit][pointcloud]")
{
    const pci::SourcePoint point;
    CHECK(point.rgba == 0xffffffffU);
    CHECK(point.intensity == 0);
    CHECK(point.classification == 0);
    CHECK(point.returnNumber == 0);
    CHECK(point.numberOfReturns == 0);
    CHECK(point.sourceOrdinal == 0);
}

TEST_CASE("GPU property packing preserves known fields", "[unit][pointcloud]")
{
    static_assert(sizeof(pci::GpuPoint) == 16);

    const std::uint32_t packed = pci::packGpuPointProperties(513, 7, 9);
    CHECK(pci::gpuPointIntensity(packed) == 513);
    CHECK(pci::gpuPointReturnNumber(packed) == 7);
    CHECK(pci::gpuPointNumberOfReturns(packed) == 9);

    const std::uint32_t saturated =
        pci::packGpuPointProperties(65535, 255, 255);
    CHECK(pci::gpuPointIntensity(saturated) == 65535);
    CHECK(pci::gpuPointReturnNumber(saturated) == 255);
    CHECK(pci::gpuPointNumberOfReturns(saturated) == 255);
}

} // namespace
