#include "navigation/NavigationInputState.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("navigation input combines movement and modifiers", "[unit][input]")
{
    pci::NavigationInputState input;
    CHECK_FALSE(input.hasMovement());
    input.press(pci::MovementKey::Forward);
    input.press(pci::MovementKey::Right);
    CHECK(input.hasMovement());

    const auto direction = input.movementDirection();
    CHECK(pci::length(direction) == Catch::Approx(1.0));
    CHECK(direction.x > 0.0);
    CHECK(direction.z > 0.0);

    input.setFast(true);
    CHECK(input.speedMultiplier() == Catch::Approx(5.0));
    input.setFine(true);
    CHECK(input.speedMultiplier() == Catch::Approx(0.5));

    input.clearMovement();
    CHECK_FALSE(input.hasMovement());
    CHECK(pci::length(input.movementDirection()) == 0.0);
    CHECK(pci::NavigationInputState::boundedDeltaSeconds(2.0) ==
          Catch::Approx(0.1));
}

TEST_CASE("wheel pick requests coalesce", "[unit][input]")
{
    pci::NavigationInputState input;
    input.queueWheel({10, 20}, 1.0);
    input.queueWheel({12, 22}, 2.0);

    const auto request = input.takePendingPick(7, 11, 13);
    REQUIRE(request.has_value());
    CHECK(request->kind == pci::PickKind::Wheel);
    CHECK(request->wheelUnits == Catch::Approx(3.0));
    CHECK(request->position == pci::PixelPosition{12, 22});
    CHECK(request->cameraRevision == 7);
    CHECK(request->documentRevision == 11);
    CHECK(request->selectionGeneration == 13);
    CHECK_FALSE(input.takePendingPick(7, 11, 13));

    input.completeInFlight();
    CHECK_FALSE(input.hasInFlightPick());
}

TEST_CASE("higher-priority pick requests are retained", "[unit][input]")
{
    pci::NavigationInputState input;
    input.queueWheel({1, 2}, 1.0);
    input.queuePivot({5, 6});
    REQUIRE(input.pendingPick());
    CHECK(input.pendingPick()->kind == pci::PickKind::Pivot);

    input.queueWheel({7, 8}, 3.0);
    REQUIRE(input.pendingPick());
    CHECK(input.pendingPick()->position == pci::PixelPosition{5, 6});

    input.clearMovement();
    input.cancelPicks();
    input.queueRaw({5, 6});
    input.queueWheel({7, 8}, 3.0);
    REQUIRE(input.pendingPick());
    CHECK(input.pendingPick()->kind == pci::PickKind::Raw);
    CHECK(input.pendingPick()->position == pci::PixelPosition{5, 6});
}

TEST_CASE("measurement commits supersede hover while wheel supersedes hover",
          "[unit][input][measurement]")
{
    pci::NavigationInputState input;
    REQUIRE(input.queueMeasureHover({1, 2}) != 0);
    input.queueWheel({3, 4}, 1.0);
    REQUIRE(input.pendingPick());
    CHECK(input.pendingPick()->kind == pci::PickKind::Wheel);

    REQUIRE(input.queueMeasureHover({5, 6}) == 0);
    REQUIRE(input.pendingPick());
    CHECK(input.pendingPick()->kind == pci::PickKind::Wheel);

    REQUIRE(input.queueMeasureCommit({7, 8}) != 0);
    REQUIRE(input.pendingPick());
    CHECK(input.pendingPick()->kind == pci::PickKind::MeasureCommit);
}

TEST_CASE("stale picks requeue current wheel input unless cancelled",
          "[unit][input]")
{
    pci::NavigationInputState input;
    input.queueWheel({10, 20}, 1.0);
    const auto request = input.takePendingPick(7, 11, 13);
    REQUIRE(request.has_value());

    input.queueWheel({30, 40}, 2.0);
    input.markStale(*request);

    CHECK_FALSE(input.hasInFlightPick());
    REQUIRE(input.pendingPick());
    CHECK(input.pendingPick()->kind == pci::PickKind::Wheel);
    CHECK(input.pendingPick()->wheelUnits == Catch::Approx(3.0));
    CHECK(input.pendingPick()->position == pci::PixelPosition{30, 40});

    input.cancelPicks();
    input.markStale(*request);
    CHECK_FALSE(input.pendingPick());
    CHECK_FALSE(input.hasInFlightPick());
}

} // namespace
