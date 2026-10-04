#include <pci/vector/VectorGeometry.h>
#include <pci/vector/VectorLayerData.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <numbers>

namespace {

TEST_CASE("vector geometry enforces emitted fill and retained byte limits",
          "[unit][vector]")
{
    pci::VectorImportLimits limits;
    limits.maximumEmittedFillVertices = 3;
    pci::VectorGeometryBuilder builder({}, limits);
    const pci::VectorRing ring{
        {0.0, 0.0}, {4.0, 0.0}, {4.0, 4.0}, {0.0, 4.0}, {0.0, 0.0}};
    const std::array<pci::VectorRing, 1> rings{ring};
    CHECK_THROWS_AS(builder.addPolygon(rings), pci::VectorImportLimitExceeded);

    limits = {};
    limits.maximumMarkers = 1;
    pci::VectorGeometryBuilder markerBuilder({}, limits);
    REQUIRE(markerBuilder.addPoint(0.0, 0.0));
    CHECK_THROWS_AS(markerBuilder.addPoint(1.0, 1.0),
                    pci::VectorImportLimitExceeded);
}

TEST_CASE(
    "connected polygons partition triangle streams at the 16-bit boundary",
    "[unit][vector]")
{
    constexpr std::size_t vertexCount = pci::maximumVerticesPerFillBatch + 9;
    pci::VectorRing ring;
    ring.reserve(vertexCount + 1);
    for (std::size_t index = 0; index < vertexCount; ++index) {
        const double angle = 2.0 * std::numbers::pi *
                             static_cast<double>(index) /
                             static_cast<double>(vertexCount);
        ring.push_back({1000.0 * std::cos(angle), 1000.0 * std::sin(angle)});
    }
    ring.push_back(ring.front());
    pci::VectorGeometryBuilder builder({}, {});
    REQUIRE(builder.addPolygon(std::array<pci::VectorRing, 1>{ring}));
    const pci::VectorLayerData data = std::move(builder).build();
    REQUIRE(data.fillBatches.size() >= 2);
    std::uint64_t emitted = 0;
    for (const auto &batch : data.fillBatches) {
        CHECK(batch.vertices.size() <= pci::maximumVerticesPerFillBatch);
        emitted += batch.vertices.size();
    }
    CHECK(emitted > vertexCount); // boundary triangles duplicate local vertices
}

TEST_CASE(
    "fill batch boundary duplication counts against emitted vertex limits",
    "[unit][vector]")
{
    constexpr std::size_t vertexCount = pci::maximumVerticesPerFillBatch + 9;
    pci::VectorRing ring;
    ring.reserve(vertexCount + 1);
    for (std::size_t index = 0; index < vertexCount; ++index) {
        const double angle = 2.0 * std::numbers::pi *
                             static_cast<double>(index) /
                             static_cast<double>(vertexCount);
        ring.push_back({1000.0 * std::cos(angle), 1000.0 * std::sin(angle)});
    }
    ring.push_back(ring.front());
    pci::VectorImportLimits limits;
    limits.maximumEmittedFillVertices = pci::maximumVerticesPerFillBatch;
    pci::VectorGeometryBuilder builder({}, limits);
    CHECK_THROWS_AS(
        builder.addPolygon(std::array<pci::VectorRing, 1>{std::move(ring)}),
        pci::VectorImportLimitExceeded);
}

TEST_CASE("vector import limits validate curve and working-byte configuration",
          "[unit][vector]")
{
    pci::VectorImportLimits limits;
    limits.curveMaximumAngleStepDegrees = 0.0;
    CHECK_FALSE(pci::validVectorImportLimits(limits));
    CHECK_THROWS_AS(pci::VectorGeometryBuilder({}, limits),
                    std::invalid_argument);

    limits = {};
    limits.maximumApplicationWorkingBytes = 1;
    pci::VectorGeometryBuilder builder({}, limits);
    CHECK_THROWS_AS(builder.addPoint(0.0, 0.0), pci::VectorImportLimitExceeded);
}

} // namespace
