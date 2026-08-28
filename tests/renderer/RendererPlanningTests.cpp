#include "renderer/planning/AdaptivePointBudget.h"
#include "renderer/planning/FrameCamera.h"
#include "renderer/planning/Measurement.h"
#include "renderer/planning/PointSizePolicy.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

TEST_CASE("adaptive point budget responds to frame timing",
          "[unit][renderer-planning]")
{
    using Milliseconds = std::chrono::duration<double, std::milli>;
    const auto sample = [](const double milliseconds,
                           const bool uploads = false,
                           const bool pick = false,
                           const bool gpu = false) {
        return pci::FrameSample{
            .frameTime = Milliseconds(milliseconds),
            .gpuTiming = gpu,
            .includedUploads = uploads,
            .includedPick = pick,
            .submittedPoints = 1'000'000,
        };
    };

    SECTION("it rejects empty clouds")
    {
        REQUIRE_THROWS_AS(pci::AdaptivePointBudget(0), std::invalid_argument);
    }

    SECTION("it requires sustained timing evidence")
    {
        pci::AdaptivePointBudget budget(10'000'000);
        CHECK(budget.current() == 1'000'000);
        budget.update(sample(20.0));
        CHECK(budget.current() == 1'000'000);
        budget.update(sample(20.0));
        CHECK(budget.current() == 1'000'000);
        budget.update(sample(20.0));
        CHECK(budget.current() == 900'000);

        pci::AdaptivePointBudget fast(10'000'000);
        for (int i = 0; i < 5; ++i) {
            fast.update(sample(10.0, false, false, true));
        }
        CHECK(fast.current() == 1'000'000);
        fast.update(sample(10.0, false, false, true));
        CHECK(fast.current() == 1'050'000);
        CHECK(fast.smoothedFrameMilliseconds() == Catch::Approx(10.0));
    }

    SECTION("it keeps the target budget in the dead band")
    {
        pci::AdaptivePointBudget steady(10'000'000);
        steady.update(sample(16.67));
        CHECK(steady.current() == 1'000'000);
    }

    SECTION("it ignores upload, pick, empty, and invalid samples")
    {
        pci::AdaptivePointBudget budget(10'000'000);
        for (int i = 0; i < 10; ++i) {
            budget.update(sample(30.0, true));
            budget.update(sample(30.0, false, true));
        }
        budget.update({.frameTime = Milliseconds(30.0)});
        budget.update({
            .frameTime = Milliseconds(std::numeric_limits<double>::quiet_NaN()),
            .submittedPoints = 1'000'000,
        });
        CHECK(budget.current() == 1'000'000);
        CHECK(budget.smoothedFrameMilliseconds() == 0.0);
    }

    SECTION("it respects lower and upper limits")
    {
        pci::AdaptivePointBudget lowerBound(10'000'000);
        for (int i = 0; i < 300; ++i) {
            lowerBound.update(sample(30.0));
        }
        CHECK(lowerBound.current() == 100'000);

        pci::AdaptivePointBudget upperBound(2'000'000);
        for (int i = 0; i < 100; ++i) {
            upperBound.update(sample(1.0));
        }
        CHECK(upperBound.current() == 2'000'000);
    }

    SECTION("capacity changes preserve the learned budget")
    {
        pci::AdaptivePointBudget budget(10'000'000);
        budget.update(sample(30.0));
        budget.update(sample(30.0));
        budget.update(sample(30.0));
        REQUIRE(budget.current() == 900'000);

        budget.setTotal(20'000'000);
        CHECK(budget.current() == 900'000);
        CHECK(budget.total() == 20'000'000);

        budget.setTotal(500'000);
        CHECK(budget.current() == 500'000);
        CHECK(budget.total() == 500'000);
        budget.setCurrent(250'000);
        CHECK(budget.current() == 250'000);
        budget.setCurrent(1);
        CHECK(budget.current() == 100'000);
    }

    SECTION("it starts at the entire cloud when smaller than the target")
    {
        pci::AdaptivePointBudget tiny(50'000);
        CHECK(tiny.current() == 50'000);
    }
}

TEST_CASE("frame camera converts pixels and projection depth consistently",
          "[unit][renderer-planning][camera]")
{
    pci::FrameCamera perspective{
        .outputHeight = 1'000,
        .nearPlane = 1.0,
        .verticalFovDegrees = 60.0,
    };
    CHECK(perspective.worldUnitsPerPixelAtDepth(20.0) ==
          Catch::Approx(2.0 * perspective.worldUnitsPerPixelAtDepth(10.0)));
    CHECK(perspective.shaderNearPlaneW() == 1.0F);

    pci::FrameCamera orthographic = perspective;
    orthographic.orthographic = true;
    orthographic.orthographicScale = 200.0;
    CHECK(orthographic.worldUnitsPerPixelAtDepth(1.0) == Catch::Approx(0.2));
    CHECK(orthographic.worldUnitsPerPixelAtDepth(50.0) == Catch::Approx(0.2));
    CHECK(orthographic.shaderNearPlaneW() == 0.0F);
}

TEST_CASE("point size follows projected node spacing",
          "[unit][renderer-planning][point-size]")
{
    const pci::Bounds3d bounds{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {0.0, 0.0, 0.0},
    };
    pci::FrameCamera camera{
        .eye = {0.0, 0.0, 10.0},
        .outputHeight = 1'000,
        .verticalFovDegrees = 90.0,
    };

    CHECK(pci::projectedPointSpacingPixels(bounds, 0.02, camera) ==
          Catch::Approx(1.0));
    CHECK(pci::adaptivePointSizePixels(
              bounds, 0.02, 2.0, 1.0, 1.0F, 8.0F, camera) ==
          Catch::Approx(2.0F));
    CHECK(pci::adaptivePointSizePixels(
              bounds, 0.02, 2.0, 2.0, 1.0F, 8.0F, camera) ==
          Catch::Approx(4.0F));

    camera.orthographic = true;
    camera.orthographicScale = 10.0;
    CHECK(pci::projectedPointSpacingPixels(bounds, 0.01, camera) ==
          Catch::Approx(1.0));
    camera.eye = {0.0, 0.0, 1'000.0};
    CHECK(pci::projectedPointSpacingPixels(bounds, 0.01, camera) ==
          Catch::Approx(1.0));
}

TEST_CASE("point size policy bounds invalid and extreme inputs",
          "[unit][renderer-planning][point-size]")
{
    const pci::Bounds3d bounds{
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    const pci::FrameCamera camera{
        .eye = {0.0, 0.0, 10.0},
        .outputHeight = 1'000,
        .verticalFovDegrees = 60.0,
    };
    const pci::Bounds3d invalidBounds{
        .minimum = {1.0, 1.0, 1.0},
        .maximum = {-1.0, -1.0, -1.0},
    };

    CHECK(pci::adaptivePointSizePixels(
              bounds, 1e-9, 1.0, 1.0, 1.0F, 8.0F, camera) == 1.0F);
    CHECK(pci::adaptivePointSizePixels(
              bounds, 1.0, 100.0, 1.0, 1.0F, 8.0F, camera) == 8.0F);
    CHECK(pci::adaptivePointSizePixels(
              invalidBounds, 1.0, 1.0, 1.0, 1.0F, 8.0F, camera) == 1.0F);
    CHECK(pci::adaptivePointSizePixels(
              bounds, 1.0, 1.0, 1.0, 8.0F, 1.0F, camera) == 1.0F);
}

TEST_CASE("measurement distances retain horizontal and vertical components",
          "[unit][renderer-planning][measurement]")
{
    const pci::DistanceMeasurement measurement = pci::makeDistanceMeasurement(
        {1000000.0, 2000000.0, 10.0}, {1000003.0, 2000004.0, -2.0});

    CHECK(measurement.horizontalDistance == Catch::Approx(5.0));
    CHECK(measurement.verticalDelta == Catch::Approx(-12.0));
    CHECK(std::abs(measurement.verticalDelta) == Catch::Approx(12.0));
    CHECK(measurement.distance3d == Catch::Approx(13.0));
}

} // namespace
