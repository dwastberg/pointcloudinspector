#include <pci/navigation/NavigationCamera.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("default camera framing is the normalized cube", "[unit][navigation]")
{
    pci::NavigationCamera camera;
    camera.frameScene();
    CHECK(camera.position() == pci::Vec3d{0.0, -4.0, 0.0});
    CHECK(camera.pivot() == pci::Vec3d{});
    CHECK(camera.sceneDiameter() == 2.0);
}

TEST_CASE("scene bounds reposition framing and scale speed",
          "[unit][navigation]")
{
    pci::NavigationCamera camera;
    camera.setScene({100.0, 50.0, 20.0}, 40.0);
    camera.frameScene();

    CHECK(camera.position() == pci::Vec3d{100.0, -30.0, 20.0});
    CHECK(camera.pivot() == pci::Vec3d{100.0, 50.0, 20.0});
    CHECK(camera.sceneDiameter() == 40.0);
    // Speed derives from pivot distance (80), clamped to 2x diameter.
    CHECK(camera.movementSpeed() == Catch::Approx(80.0));
    CHECK(camera.clipPlanes().farPlane >= 160.0);
}

TEST_CASE("invalid scene parameters are ignored", "[unit][navigation]")
{
    pci::NavigationCamera camera;
    camera.setScene({100.0, 50.0, 20.0}, 40.0);
    camera.setScene({0.0, 0.0, 0.0}, 0.0);
    camera.setScene({0.0, 0.0, 0.0}, -5.0);
    CHECK(camera.sceneDiameter() == 40.0);
}

TEST_CASE("projection switching preserves camera framing",
          "[unit][navigation][camera]")
{
    pci::NavigationCamera camera;
    camera.setScene({10.0, 20.0, 30.0}, 40.0);
    camera.frameScene(1.15);
    const auto perspectivePosition = camera.position();
    const auto perspectivePivot = camera.pivot();
    const auto perspectiveForward = camera.forward();

    camera.setOrthographic(true);
    const double orthographicScale = camera.orthographicScale();
    CHECK(camera.isOrthographic());
    CHECK(orthographicScale > 0.0);

    camera.setOrthographic(false);
    CHECK_FALSE(camera.isOrthographic());
    CHECK(camera.pivot() == perspectivePivot);
    CHECK(camera.forward() == perspectiveForward);
    CHECK(pci::length(camera.position() - perspectivePosition) ==
          Catch::Approx(0.0));
}

} // namespace
