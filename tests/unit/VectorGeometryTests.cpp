#include <pci/vector/VectorGeometry.h>
#include <pci/vector/VectorLayerData.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

namespace {

pci::VectorRing square(const double minimum, const double maximum)
{
    return {
        {minimum, minimum},
        {maximum, minimum},
        {maximum, maximum},
        {minimum, maximum},
        {minimum, minimum},
    };
}

TEST_CASE("vector geometry is stored relative to a snapped origin",
          "[unit][vector]")
{
    const pci::Vec3d origin = pci::vectorLayerOrigin(334'123.2, 7'400'400.8);
    CHECK(origin == pci::Vec3d{333'824.0, 7'399'424.0, 0.0});
}

TEST_CASE("vector polygon triangulation strips closed ring duplicates",
          "[unit][vector]")
{
    pci::VectorGeometryBuilder builder({1000.0, 2000.0, 0.0}, {});
    const std::array<pci::VectorRing, 1> rings{square(1000.0, 1010.0)};
    REQUIRE(builder.addPolygon(rings));
    const pci::VectorLayerData data = std::move(builder).build();

    REQUIRE(data.fillBatches.size() == 1);
    CHECK(data.fillBatches.front().vertices.size() == 4);
    CHECK(data.fillBatches.front().indices.size() == 6);
    CHECK(data.segments.size() == 4);
    CHECK(data.triangleCount() == 2);
}

TEST_CASE("vector polygon holes exclude fill area and retain outlines",
          "[unit][vector]")
{
    pci::VectorRing exterior{
        {0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}};
    pci::VectorRing hole{
        {2.0, 2.0}, {2.0, 8.0}, {8.0, 8.0}, {8.0, 2.0}, {2.0, 2.0}};
    pci::VectorGeometryBuilder builder({}, {});
    const std::array<pci::VectorRing, 2> rings{exterior, hole};
    REQUIRE(builder.addPolygon(rings));
    const pci::VectorLayerData data = std::move(builder).build();

    CHECK(data.triangleCount() == 8);
    CHECK(data.segments.size() == 8);
    CHECK(data.summary.unfilledPolygons == 0);
}

TEST_CASE("vector line and point builders reject degenerate geometry",
          "[unit][vector]")
{
    pci::VectorGeometryBuilder builder({}, {});
    CHECK_FALSE(builder.addPoint(NAN, 0.0));
    const std::array<double, 2> duplicate{1.0, 1.0};
    CHECK_FALSE(builder.addLineString({&duplicate, 1}, false));
    CHECK(builder.addPoint(1.0, 2.0));
    const pci::VectorLayerData data = std::move(builder).build();
    CHECK(data.markers.size() == 1);
    CHECK(data.summary.skippedParts == 2);
}

} // namespace
