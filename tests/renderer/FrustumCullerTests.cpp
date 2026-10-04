#include <pci/rendering/planning/FrustumCuller.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <vector>

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

namespace {

// A camera at the origin looking down -Z, so the visible region around z = -10
// spans roughly +/-10 in x and y.
[[nodiscard]] pci::FrustumCuller downwardCuller()
{
    return pci::FrustumCuller::fromCamera({0.0, 0.0, 0.0},
                                          {0.0, 0.0, -1.0},
                                          {0.0, 1.0, 0.0},
                                          {1.0, 0.0, 0.0},
                                          90.0,
                                          1.0,
                                          1.0,
                                          100.0);
}

[[nodiscard]] std::array<pci::Vec3d, 4> quadAt(const double z,
                                               const double half)
{
    return {pci::Vec3d{-half, -half, z},
            pci::Vec3d{half, -half, z},
            pci::Vec3d{half, half, z},
            pci::Vec3d{-half, half, z}};
}

} // namespace

TEST_CASE("frustum clipping keeps a wholly inside polygon",
          "[renderer][frustum]")
{
    const pci::FrustumCuller culler = downwardCuller();
    const std::array<pci::Vec3d, 4> quad = quadAt(-10.0, 2.0);

    const std::vector<pci::Vec3d> clipped = culler.clipConvexPolygon(quad);
    REQUIRE(clipped.size() == 4);
    for (std::size_t index = 0; index < quad.size(); ++index) {
        CHECK(clipped[index].x == Catch::Approx(quad[index].x));
        CHECK(clipped[index].y == Catch::Approx(quad[index].y));
        CHECK(clipped[index].z == Catch::Approx(quad[index].z));
    }
}

TEST_CASE("frustum clipping rejects a wholly outside polygon",
          "[renderer][frustum]")
{
    const pci::FrustumCuller culler = downwardCuller();

    // Behind the camera.
    CHECK(culler.clipConvexPolygon(quadAt(10.0, 2.0)).empty());
    // Beyond the far plane.
    CHECK(culler.clipConvexPolygon(quadAt(-500.0, 2.0)).empty());
    // Far off to one side at a valid depth.
    const std::array<pci::Vec3d, 4> aside{pci::Vec3d{100.0, -1.0, -10.0},
                                          pci::Vec3d{110.0, -1.0, -10.0},
                                          pci::Vec3d{110.0, 1.0, -10.0},
                                          pci::Vec3d{100.0, 1.0, -10.0}};
    CHECK(culler.clipConvexPolygon(aside).empty());
}

TEST_CASE("frustum clipping trims a polygon straddling each plane",
          "[renderer][frustum]")
{
    const pci::FrustumCuller culler = downwardCuller();
    // Far wider than the frustum at this depth, so every side plane cuts it.
    const std::vector<pci::Vec3d> clipped =
        culler.clipConvexPolygon(quadAt(-10.0, 1000.0));

    REQUIRE(clipped.size() >= 3);
    // The survivors sit inside the visible extent, which is about +/-10 here.
    for (const pci::Vec3d vertex : clipped) {
        CHECK(std::abs(vertex.x) <= 10.5);
        CHECK(std::abs(vertex.y) <= 10.5);
        CHECK(vertex.z == Catch::Approx(-10.0));
    }
    // Clipping a convex polygon against six half-spaces cannot exceed 4 + 6
    // vertices, which is why no general-polygon machinery is required.
    CHECK(clipped.size() <= 10);

    // A polygon crossing the near plane is trimmed rather than dropped.
    const std::array<pci::Vec3d, 4> crossing{pci::Vec3d{-1.0, -1.0, 5.0},
                                             pci::Vec3d{1.0, -1.0, 5.0},
                                             pci::Vec3d{1.0, 1.0, -20.0},
                                             pci::Vec3d{-1.0, 1.0, -20.0}};
    const std::vector<pci::Vec3d> trimmed = culler.clipConvexPolygon(crossing);
    REQUIRE_FALSE(trimmed.empty());
    for (const pci::Vec3d vertex : trimmed) {
        CHECK(vertex.z <= -1.0 + 1.0e-9);
    }
}

TEST_CASE("frustum clipping degenerates safely", "[renderer][frustum]")
{
    const pci::FrustumCuller culler = downwardCuller();

    // Fewer than three vertices cannot bound an area.
    CHECK(culler.clipConvexPolygon({}).empty());
    const std::array<pci::Vec3d, 2> segment{pci::Vec3d{0.0, 0.0, -10.0},
                                            pci::Vec3d{1.0, 0.0, -10.0}};
    CHECK(culler.clipConvexPolygon(segment).empty());

    // A zero-area quad collapsed to a point yields no visible region.
    const std::array<pci::Vec3d, 4> point{pci::Vec3d{0.0, 0.0, -10.0},
                                          pci::Vec3d{0.0, 0.0, -10.0},
                                          pci::Vec3d{0.0, 0.0, -10.0},
                                          pci::Vec3d{0.0, 0.0, -10.0}};
    const std::vector<pci::Vec3d> collapsed = culler.clipConvexPolygon(point);
    for (const pci::Vec3d vertex : collapsed) {
        CHECK(vertex.x == Catch::Approx(0.0));
        CHECK(vertex.y == Catch::Approx(0.0));
    }
}

TEST_CASE("frustum clipping agrees with the bounds test", "[renderer][frustum]")
{
    const pci::FrustumCuller culler = downwardCuller();
    // A visible polygon must also intersect as a box, and an invisible one
    // must not: the two queries answer different questions but cannot
    // contradict each other on a planar quad.
    const std::array<pci::Vec3d, 4> visible = quadAt(-10.0, 2.0);
    pci::Bounds3d visibleBounds;
    visibleBounds.minimum = {-2.0, -2.0, -10.0};
    visibleBounds.maximum = {2.0, 2.0, -10.0};
    CHECK_FALSE(culler.clipConvexPolygon(visible).empty());
    CHECK(culler.intersects(visibleBounds));

    const std::array<pci::Vec3d, 4> hidden = quadAt(10.0, 2.0);
    pci::Bounds3d hiddenBounds;
    hiddenBounds.minimum = {-2.0, -2.0, 10.0};
    hiddenBounds.maximum = {2.0, 2.0, 10.0};
    CHECK(culler.clipConvexPolygon(hidden).empty());
    CHECK_FALSE(culler.intersects(hiddenBounds));
}
