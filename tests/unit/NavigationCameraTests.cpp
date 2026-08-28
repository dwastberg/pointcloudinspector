#include "navigation/NavigationCamera.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace {

TEST_CASE("navigation camera defaults and framing are predictable",
          "[unit][navigation]")
{
    const pci::Vec3d expectedPosition{0.0, -4.0, 0.0};
    const pci::Vec3d expectedForward{0.0, 1.0, 0.0};
    const pci::Vec3d expectedRight{1.0, 0.0, 0.0};
    const pci::Vec3d expectedUp{0.0, 0.0, 1.0};
    pci::NavigationCamera camera;
    CHECK(camera.position() == expectedPosition);
    CHECK(camera.forward() == expectedForward);
    CHECK(camera.right() == expectedRight);
    CHECK(camera.up() == expectedUp);
    CHECK(pci::NavigationCamera::worldUp == expectedUp);

    camera.translate({1.0, 2.0, 3.0});
    camera.frameScene();
    CHECK(camera.position() == expectedPosition);
    CHECK(camera.pivot() == pci::Vec3d{});
    CHECK(camera.forward() == expectedForward);
}

TEST_CASE("navigation translation moves the pivot with the camera",
          "[unit][navigation]")
{
    pci::NavigationCamera camera;
    camera.translate({0.0, 5.0, 0.0});

    CHECK(camera.position().y == Catch::Approx(1.0));
    CHECK(camera.pivot().y == Catch::Approx(5.0));
}

TEST_CASE("navigation camera accepts deterministic replay poses",
          "[unit][navigation]")
{
    pci::NavigationCamera camera;
    camera.setView({10.0, -20.0, 15.0}, {2.0, 3.0, 4.0});

    CHECK(camera.position() == pci::Vec3d{10.0, -20.0, 15.0});
    CHECK(camera.pivot() == pci::Vec3d{2.0, 3.0, 4.0});
    CHECK(camera.forward() ==
          pci::normalized(camera.pivot() - camera.position()));
}

TEST_CASE("navigation dolly has no arbitrary minimum distance",
          "[unit][navigation]")
{
    pci::NavigationCamera camera;
    const pci::Vec3d target{0.5, 0.0, 0.0};
    camera.dollyToward(target, 20.0);

    CHECK(pci::length(camera.position() - target) < 0.03);
}

TEST_CASE("navigation orbit preserves camera-pivot distance",
          "[unit][navigation]")
{
    pci::NavigationCamera camera;
    const double initialDistance =
        pci::length(camera.position() - camera.pivot());
    camera.orbitFromDrag(12.0, -8.0);

    CHECK(camera.position().x < 0.0);
    CHECK(camera.position().z < 0.0);
    CHECK(pci::length(camera.position() - camera.pivot()) ==
          Catch::Approx(initialDistance));

    camera.setPivot({0.5, 0.25, 0.0});
    const double offCenterDistance =
        pci::length(camera.position() - camera.pivot());
    camera.orbitFromDrag(30.0, -20.0);
    CHECK(pci::length(camera.position() - camera.pivot()) ==
          Catch::Approx(offCenterDistance));
}

TEST_CASE("navigation pan uses the screen plane", "[unit][navigation]")
{
    pci::NavigationCamera camera;
    camera.panFromDrag(100.0, -50.0, 1000.0);

    CHECK(camera.pivot().x == Catch::Approx(-0.46188).margin(0.0001));
    CHECK(camera.pivot().z == Catch::Approx(-0.23094).margin(0.0001));
    CHECK(camera.position().x == Catch::Approx(camera.pivot().x));
    CHECK(camera.position().z == Catch::Approx(camera.pivot().z));

    const auto revision = camera.revision();
    camera.panFromDrag(100.0, 50.0, 0.0);
    CHECK(camera.revision() == revision);
    CHECK(camera.pivot().x == Catch::Approx(-0.46188));
}

TEST_CASE("navigation speed and clip planes remain finite",
          "[unit][navigation]")
{
    pci::NavigationCamera camera;
    CHECK(camera.movementSpeed() == Catch::Approx(4.0));
    CHECK(camera.movementSpeed(0.1) == Catch::Approx(0.4));

    camera.setNavigationReference({0.0, 0.0, 3.999999});
    const auto close = camera.clipPlanes();
    CHECK(std::isfinite(close.nearPlane));
    CHECK(close.nearPlane >= camera.sceneDiameter() * 1e-7);
    CHECK(close.farPlane > close.nearPlane);
}

} // namespace
