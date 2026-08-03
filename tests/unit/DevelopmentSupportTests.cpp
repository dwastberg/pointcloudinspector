#include "development/SyntheticPointCloud.h"
#include "pointcloud/GpuPoint.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <ranges>

namespace {

TEST_CASE("synthetic point generation is stable and spatially varied",
          "[unit][development-support]")
{
    static_assert(sizeof(pci::GpuPoint) == 16);

    const auto first = pci::generatePointChunk(0, 4096);
    const auto repeated = pci::generatePointChunk(0, 4096);
    CHECK(first == repeated);
    REQUIRE(first.size() == 4096);
    CHECK(pci::generatePointChunk(4096, 128) !=
          pci::generatePointChunk(0, 128));
    for (std::size_t offset = 0; offset < first.size(); ++offset) {
        CHECK(first[offset] == pci::generatePoint(offset));
    }
    CHECK(pci::generatePoint(4096) == pci::generatePointChunk(4096, 1).front());

    std::array<bool, 8> octants{};
    for (const auto &point : first) {
        const auto index = (point.x >= 32768 ? 1 : 0) |
                           (point.y >= 32768 ? 2 : 0) |
                           (point.z >= 32768 ? 4 : 0);
        octants[index] = true;
    }
    CHECK(std::ranges::all_of(octants, std::identity{}));
}

} // namespace
