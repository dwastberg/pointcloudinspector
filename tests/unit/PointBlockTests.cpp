#include "scene/PointBlock.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <random>

namespace {

TEST_CASE("block quantization round-trips within half a step", "[unit][scene]")
{
    const pci::Vec3d origin{1000.0, 2000.0, 10.0};
    const double edge = 10.0;
    const double scale = pci::blockScaleForEdge(edge);
    CHECK(scale == Catch::Approx(10.0 / 65535.0));

    pci::PointBlock block{.origin = origin, .scale = scale};
    // A fixed seed makes this numerical regression test reproducible.
    std::mt19937 rng(42); // NOLINT(bugprone-random-generator-seed)
    std::uniform_real_distribution<double> offset(0.0, edge);
    for (int i = 0; i < 1000; ++i) {
        const pci::Vec3d position{
            origin.x + offset(rng),
            origin.y + offset(rng),
            origin.z + offset(rng),
        };
        const auto q = pci::quantizeToBlock(position, origin, scale);
        const pci::GpuPoint point{.x = q[0], .y = q[1], .z = q[2]};
        const pci::Vec3d decoded = pci::decodeBlockPosition(block, point);
        CHECK(std::abs(decoded.x - position.x) <= scale * 0.5 + 1e-12);
        CHECK(std::abs(decoded.y - position.y) <= scale * 0.5 + 1e-12);
        CHECK(std::abs(decoded.z - position.z) <= scale * 0.5 + 1e-12);
    }
}

TEST_CASE("block quantization clamps positions outside the cell",
          "[unit][scene]")
{
    const pci::Vec3d origin{};
    const double scale = pci::blockScaleForEdge(1.0);
    CHECK(pci::quantizeToBlock({-5.0, 0.5, 2.0}, origin, scale) ==
          std::array<std::uint16_t, 3>{0, 32768, 65535});
}

TEST_CASE("point blocks share immutable ownership", "[unit][scene]")
{
    static_assert(std::is_same_v<pci::PointBlockPtr,
                                 std::shared_ptr<const pci::PointBlock>>);
    CHECK(pci::maximumPointsPerBlock == 65'536U);
}

} // namespace
