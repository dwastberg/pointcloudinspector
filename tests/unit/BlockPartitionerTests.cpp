#include "scene/BlockPartitioner.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <stdexcept>
#include <vector>

namespace {

const pci::Bounds3d unitBounds{
    .minimum = {0.0, 0.0, 0.0},
    .maximum = {1.0, 1.0, 1.0},
};

TEST_CASE("small inputs produce one block in source order", "[unit][scene]")
{
    std::vector<pci::PointBlockPtr> blocks;
    pci::BlockPartitioner partitioner(
        unitBounds, 8, [&blocks](pci::PointBlockPtr b) {
            blocks.push_back(std::move(b));
        });
    partitioner.add({.position = {0.25, 0.25, 0.25}, .rgba = 1});
    partitioner.add({.position = {0.75, 0.75, 0.75}, .rgba = 2});
    partitioner.finish();

    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks.front()->points.size() == 2);
    CHECK(blocks.front()->points[0].rgba == 1);
    CHECK(blocks.front()->points[1].rgba == 2);
    CHECK(blocks.front()->attributes.size() == 2);
    const pci::Vec3d decoded =
        pci::decodeBlockPosition(*blocks.front(), blocks.front()->points[0]);
    CHECK(decoded.x == Catch::Approx(0.25).margin(1e-4));
    CHECK(decoded.y == Catch::Approx(0.25).margin(1e-4));
    CHECK(decoded.z == Catch::Approx(0.25).margin(1e-4));
}

TEST_CASE("dense cells seal blocks at capacity during streaming",
          "[unit][scene]")
{
    std::vector<pci::PointBlockPtr> blocks;
    pci::BlockPartitioner partitioner(
        unitBounds, 2'500'000, [&blocks](pci::PointBlockPtr b) {
            blocks.push_back(std::move(b));
        });
    // All samples land in one grid cell.
    const std::uint64_t total = 2 * pci::maximumPointsPerBlock + 5;
    for (std::uint64_t i = 0; i < total; ++i) {
        partitioner.add({.position = {0.01, 0.01, 0.01}});
    }
    CHECK(blocks.size() == 2); // sealed mid-stream, before finish()
    partitioner.finish();

    REQUIRE(blocks.size() == 3);
    std::uint64_t counted = 0;
    for (const auto &block : blocks) {
        CHECK(block->points.size() <= pci::maximumPointsPerBlock);
        CHECK(block->points.size() == block->attributes.size());
        CHECK(block->origin == blocks.front()->origin);
        CHECK(block->scale == blocks.front()->scale);
        counted += block->points.size();
    }
    CHECK(counted == total);
}

TEST_CASE("partitioner can publish the largest sparse cell before finish",
          "[unit][scene]")
{
    std::vector<pci::PointBlockPtr> blocks;
    pci::BlockPartitioner partitioner(
        unitBounds, 100'000'000, [&blocks](pci::PointBlockPtr block) {
            blocks.push_back(std::move(block));
        });
    partitioner.add({.position = {0.01, 0.01, 0.01}, .rgba = 1});
    partitioner.add({.position = {0.02, 0.01, 0.01}, .rgba = 2});
    partitioner.add({.position = {0.03, 0.01, 0.01}, .rgba = 3});
    partitioner.add({.position = {0.99, 0.99, 0.99}, .rgba = 4});

    CHECK(partitioner.flushLargest(0) == 0);
    CHECK(partitioner.flushLargest(1) == 1);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks.front()->points.size() == 3);

    partitioner.finish();
    REQUIRE(blocks.size() == 2);
    CHECK(blocks.back()->points.size() == 1);
}

TEST_CASE("blocks carry tight bounds and intensity statistics", "[unit][scene]")
{
    std::vector<pci::PointBlockPtr> blocks;
    pci::BlockPartitioner partitioner(
        unitBounds, 4, [&blocks](pci::PointBlockPtr b) {
            blocks.push_back(std::move(b));
        });
    partitioner.add({
        .position = {0.2, 0.3, 0.4},
        .attributes = {.intensity = 700},
    });
    partitioner.add({
        .position = {0.6, 0.5, 0.4},
        .attributes = {.intensity = 200},
    });
    partitioner.finish();

    REQUIRE(blocks.size() == 1);
    CHECK(blocks.front()->intensityMinimum == 200);
    CHECK(blocks.front()->intensityMaximum == 700);
    CHECK(blocks.front()->bounds.minimum == std::array{0.2, 0.3, 0.4});
    CHECK(blocks.front()->bounds.maximum == std::array{0.6, 0.5, 0.4});
}

TEST_CASE("degenerate bounds still partition", "[unit][scene]")
{
    const pci::Bounds3d point{
        .minimum = {5.0, 5.0, 5.0},
        .maximum = {5.0, 5.0, 5.0},
    };
    std::vector<pci::PointBlockPtr> blocks;
    pci::BlockPartitioner partitioner(
        point, 1, [&blocks](pci::PointBlockPtr b) {
            blocks.push_back(std::move(b));
        });
    partitioner.add({.position = {5.0, 5.0, 5.0}});
    partitioner.finish();
    REQUIRE(blocks.size() == 1);
    CHECK(blocks.front()->points.size() == 1);
}

TEST_CASE("points in distinct cells produce distinct blocks", "[unit][scene]")
{
    std::vector<pci::PointBlockPtr> blocks;
    // Large expected count forces a multi-cell grid (cellsPerAxis == 8
    // for the unit bounds: cbrt(1e8 / 262144) rounds up to 8).
    pci::BlockPartitioner partitioner(
        unitBounds, 100'000'000, [&blocks](pci::PointBlockPtr b) {
            blocks.push_back(std::move(b));
        });
    REQUIRE(partitioner.cellEdge() == Catch::Approx(1.0 / 8.0));
    partitioner.add({.position = {0.05, 0.05, 0.05}});
    partitioner.add({.position = {0.95, 0.05, 0.05}});
    partitioner.add({.position = {0.05, 0.95, 0.05}});
    partitioner.add({.position = {0.95, 0.95, 0.95}});
    partitioner.finish();

    REQUIRE(blocks.size() == 4);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        for (std::size_t j = i + 1; j < blocks.size(); ++j) {
            CHECK_FALSE(blocks[i]->origin == blocks[j]->origin);
        }
        REQUIRE(blocks[i]->points.size() == 1);
        const pci::Vec3d decoded =
            pci::decodeBlockPosition(*blocks[i], blocks[i]->points.front());
        CHECK(std::abs(decoded.x - blocks[i]->bounds.minimum[0]) <=
              blocks[i]->scale * 0.5 + 1e-12);
    }
}

TEST_CASE("partitioner requires a block-ready callback", "[unit][scene]")
{
    CHECK_THROWS_AS(pci::BlockPartitioner(unitBounds, 1, nullptr),
                    std::invalid_argument);
}

} // namespace
