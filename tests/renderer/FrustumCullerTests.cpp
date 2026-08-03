#include "renderer/planning/FrustumCuller.h"

#include <catch2/catch_test_macros.hpp>

namespace {

pci::FrustumCuller defaultCuller()
{
    // Matches the default navigation camera: at {0,-4,0} looking +y, z up.
    return pci::FrustumCuller::fromCamera({0.0, -4.0, 0.0}, // position
                                          {0.0, 1.0, 0.0},  // forward
                                          {0.0, 0.0, 1.0},  // up
                                          {1.0, 0.0, 0.0},  // right
                                          60.0,
                                          1.0,
                                          0.04,
                                          12.0);
}

pci::Bounds3d cubeAt(const double x,
                     const double y,
                     const double z,
                     const double halfEdge = 1.0)
{
    return {
        .minimum = {x - halfEdge, y - halfEdge, z - halfEdge},
        .maximum = {x + halfEdge, y + halfEdge, z + halfEdge},
    };
}

TEST_CASE("boxes ahead of the camera are visible", "[unit][scene]")
{
    CHECK(defaultCuller().intersects(cubeAt(0.0, 0.0, 0.0)));
}

TEST_CASE("boxes behind the camera are culled", "[unit][scene]")
{
    CHECK_FALSE(defaultCuller().intersects(cubeAt(0.0, -10.0, 0.0)));
}

TEST_CASE("boxes far outside the field of view are culled", "[unit][scene]")
{
    CHECK_FALSE(defaultCuller().intersects(cubeAt(40.0, 4.0, 0.0)));
    CHECK_FALSE(defaultCuller().intersects(cubeAt(0.0, 4.0, 40.0)));
}

TEST_CASE("boxes beyond the far plane are culled", "[unit][scene]")
{
    CHECK_FALSE(defaultCuller().intersects(cubeAt(0.0, 100.0, 0.0)));
}

TEST_CASE("boxes straddling a frustum edge remain visible", "[unit][scene]")
{
    // Large box that overlaps the left frustum plane.
    CHECK(defaultCuller().intersects(cubeAt(-4.0, 4.0, 0.0, 4.0)));
}

} // namespace
