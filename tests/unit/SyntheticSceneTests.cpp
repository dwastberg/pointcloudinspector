#include <pci/development/SyntheticPointCloud.h>
#include <pci/development/SyntheticScene.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("synthetic scenes chunk points into capacity-bounded blocks",
          "[unit][development-support]")
{
    const std::uint64_t total = 2 * pci::maximumPointsPerBlock + 5;
    const auto scene = pci::buildSyntheticScene(total);

    CHECK(scene->totalPointCount() == total);
    CHECK(scene->metadata().hasColor);
    CHECK(scene->metadata().sourcePointCount == total);
    const auto blocks = scene->blocks();
    REQUIRE(blocks.size() == 3);
    CHECK(blocks[0]->points.size() == pci::maximumPointsPerBlock);
    CHECK(blocks[2]->points.size() == 5);
    CHECK(blocks[0]->points.front() == pci::generatePoint(0));
    CHECK(blocks[1]->points.front() ==
          pci::generatePoint(pci::maximumPointsPerBlock));
    // Same normalized decode as the legacy shader: q/65535*2-1.
    const pci::Vec3d decoded =
        pci::decodeBlockPosition(*blocks[0], blocks[0]->points.front());
    const pci::GpuPoint reference = pci::generatePoint(0);
    CHECK(decoded.x == Catch::Approx(reference.x / 65535.0 * 2.0 - 1.0));
}

} // namespace
