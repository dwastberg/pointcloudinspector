#include <pci/rendering/planning/RenderActivity.h>

#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("settled render activity sleeps", "[unit][renderer-planning][idle]")
{
    CHECK_FALSE(pci::shouldContinueRendering({}));
}

TEST_CASE("each asynchronous render activity requests continuation",
          "[unit][renderer-planning][idle]")
{
    CHECK(pci::shouldContinueRendering({.movement = true}));
    CHECK(pci::shouldContinueRendering({.pendingPick = true}));
    CHECK(pci::shouldContinueRendering({.pickReadback = true}));
    CHECK(pci::shouldContinueRendering({.pendingUploads = true}));
    CHECK(pci::shouldContinueRendering({.sceneInvalidation = true}));
    CHECK(pci::shouldContinueRendering({.pendingSmokeFrames = true}));
}

} // namespace
