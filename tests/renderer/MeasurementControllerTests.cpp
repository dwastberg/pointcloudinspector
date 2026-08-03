#include "renderer/planning/MeasurementController.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace {

using namespace std::chrono_literals;

TEST_CASE("measurement hover debounce uses supplied time",
          "[unit][renderer-planning][measurement]")
{
    pci::MeasurementController controller;
    const auto start = pci::MeasurementController::TimePoint{};

    CHECK(controller.scheduleHover({1, 2}, start) == 33ms);
    CHECK_FALSE(controller.scheduleHover({3, 4}, start + 10ms));
    const auto position = controller.takeScheduledHover();
    REQUIRE(position);
    CHECK(*position == pci::PixelPosition{3, 4});
    CHECK_FALSE(controller.takeScheduledHover());
}

TEST_CASE("measurement hover scheduling tracks latest position and interval",
          "[unit][renderer-planning][measurement]")
{
    pci::MeasurementController controller;
    const auto start = pci::MeasurementController::TimePoint{};
    static_cast<void>(controller.scheduleHover({1, 2}, start));
    REQUIRE(controller.takeScheduledHover());
    controller.recordHoverQueued(7, start + 33ms);

    CHECK(controller.scheduleHover({5, 6}, start + 43ms) == 23ms);
    REQUIRE(controller.takeScheduledHover());
    CHECK(controller.state().hoverPosition == pci::PixelPosition{5, 6});
    CHECK(controller.scheduleHover({7, 8}, start + 100ms) == 0ms);
}

TEST_CASE("measurement hover rejects stale serials and revisions",
          "[unit][renderer-planning][measurement]")
{
    pci::MeasurementController controller;
    const pci::MeasurementRevisions current{1, 2, 3};
    controller.recordHoverQueued(9, {});

    CHECK_FALSE(controller.acceptHoverResult(
        8, current, current, pci::Vec3d{1.0, 2.0, 3.0}));
    CHECK_FALSE(controller.acceptHoverResult(
        9, {2, 2, 3}, current, pci::Vec3d{1.0, 2.0, 3.0}));
    CHECK(controller.acceptHoverResult(
        9, current, current, pci::Vec3d{1.0, 2.0, 3.0}));
    REQUIRE(controller.state().hover);
    controller.invalidateHover({1, 2, 4});
    CHECK_FALSE(controller.state().hover);
}

TEST_CASE("measurement commits anchors distances and starts over",
          "[unit][renderer-planning][measurement]")
{
    pci::MeasurementController controller;
    controller.acceptCommitResult(pci::Vec3d{0.0, 0.0, 0.0}, {});
    REQUIRE(controller.state().anchor);
    controller.acceptCommitResult(pci::Vec3d{3.0, 4.0, 12.0}, {});
    REQUIRE(controller.state().measurement);
    CHECK(controller.state().measurement->distance3d == 13.0);
    CHECK_FALSE(controller.state().anchor);

    controller.acceptCommitResult(pci::Vec3d{1.0, 1.0, 1.0}, {});
    CHECK_FALSE(controller.state().measurement);
    CHECK(controller.state().anchor == pci::Vec3d{1.0, 1.0, 1.0});
    controller.acceptCommitResult(std::nullopt, {});
    CHECK_FALSE(controller.state().hover);
}

} // namespace
