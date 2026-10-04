#include <pci/navigation/QualificationCameraPath.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace {

TEST_CASE("qualification camera path has fixed phase boundaries",
          "[unit][navigation][qualification]")
{
    pci::QualificationCameraPath path({
        .minimum = {100.0, 200.0, 10.0},
        .maximum = {300.0, 300.0, 30.0},
    });
    std::vector<pci::QualificationCameraFrame> frames;
    do {
        frames.push_back(path.current());
    } while (path.advance());

    REQUIRE(frames.size() == 360);
    CHECK(frames.front().frameIndex == 0);
    CHECK(frames.front().phase == pci::QualificationFramePhase::Warmup);
    CHECK(frames[29].phase == pci::QualificationFramePhase::Warmup);
    CHECK(frames[30].phase == pci::QualificationFramePhase::Interacting);
    CHECK(frames[239].phase == pci::QualificationFramePhase::Interacting);
    CHECK(frames[240].phase == pci::QualificationFramePhase::Dwell);
    CHECK(frames.back().phase == pci::QualificationFramePhase::Dwell);
    CHECK(frames.back().finalFrame);
    CHECK(frames.back().totalFrames == 360);

    // The dwell is truly stationary, while the interaction contains both a
    // seam-crossing pan and a substantial zoom.
    CHECK(frames[240].pose.position == frames.back().pose.position);
    CHECK(frames[240].pose.pivot == frames.back().pose.pivot);
    CHECK(pci::length(frames[30].pose.position - frames[30].pose.pivot) >
          pci::length(frames[239].pose.position - frames[239].pose.pivot) *
              3.0);
    CHECK(frames[89].pose.pivot.x < frames[149].pose.pivot.x);
}

TEST_CASE("qualification camera path is scene relative",
          "[unit][navigation][qualification]")
{
    pci::QualificationCameraPath small({
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {10.0, 10.0, 10.0},
    });
    pci::QualificationCameraPath large({
        .minimum = {100.0, 200.0, 300.0},
        .maximum = {200.0, 300.0, 400.0},
    });
    const auto smallFrame = small.current();
    const auto largeFrame = large.current();
    CHECK(pci::length(largeFrame.pose.position - largeFrame.pose.pivot) ==
          Catch::Approx(
              pci::length(smallFrame.pose.position - smallFrame.pose.pivot) *
              10.0));
}

} // namespace
