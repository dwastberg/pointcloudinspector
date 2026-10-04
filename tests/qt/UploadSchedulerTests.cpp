#include <pci/rendering/rhi/UploadScheduler.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

namespace {

TEST_CASE("GPU block hashes cover every key field", "[qt][renderer][hash]")
{
    const pci::GpuBlockKey key{
        .layerId = pci::PointCloudLayerId{7},
        .nodeId = {.level = 2, .x = 1, .y = 2, .z = 3},
        .nodeBlockIndex = 4,
        .sceneBlockId = 5,
    };
    const pci::GpuBlockKey same = key;
    const pci::GpuBlockKey differentLayer{.layerId = pci::PointCloudLayerId{8},
                                          .nodeId = key.nodeId,
                                          .nodeBlockIndex = 4,
                                          .sceneBlockId = 5};
    const pci::GpuBlockKey differentNode{
        .layerId = pci::PointCloudLayerId{7},
        .nodeId = {.level = 2, .x = 9, .y = 2, .z = 3},
        .nodeBlockIndex = 4,
        .sceneBlockId = 5,
    };
    const pci::GpuBlockKey differentIndex{.layerId = pci::PointCloudLayerId{7},
                                          .nodeId = key.nodeId,
                                          .nodeBlockIndex = 6,
                                          .sceneBlockId = 5};
    const pci::GpuBlockKey differentSceneBlock{.layerId =
                                                   pci::PointCloudLayerId{7},
                                               .nodeId = key.nodeId,
                                               .nodeBlockIndex = 4,
                                               .sceneBlockId = 6};
    const pci::GpuBlockKeyHash hash;
    CHECK(hash(key) == hash(same));
    CHECK(hash(key) != hash(differentLayer));
    CHECK(hash(key) != hash(differentNode));
    CHECK(hash(key) != hash(differentIndex));
    CHECK(hash(key) != hash(differentSceneBlock));
}

TEST_CASE("upload planning fills the byte budget in order", "[qt][renderer]")
{
    const std::uint64_t mb = 1024 * 1024;
    CHECK(pci::planUploadCount({16 * mb, 16 * mb, 16 * mb, 16 * mb}, 48 * mb) ==
          3);
    CHECK(pci::planUploadCount({16 * mb, 40 * mb}, 48 * mb) == 1);
    CHECK(pci::planUploadCount({}, 48 * mb) == 0);
}

TEST_CASE("GPU eviction planning is LRU and honors frame protection",
          "[qt][renderer][cache]")
{
    const pci::GpuBlockKey first{.layerId = pci::PointCloudLayerId{1},
                                 .sceneBlockId = 1};
    const pci::GpuBlockKey second{.layerId = pci::PointCloudLayerId{1},
                                  .sceneBlockId = 2};
    const pci::GpuBlockKey protectedOldest{.layerId = pci::PointCloudLayerId{2},
                                           .sceneBlockId = 1};
    const std::array records{
        pci::GpuResidencyRecord{
            .key = first,
            .bytes = 16,
            .lastUsedFrame = 2,
            .lastVisibleFrame = 1,
        },
        pci::GpuResidencyRecord{
            .key = second,
            .bytes = 16,
            .lastUsedFrame = 3,
            .lastVisibleFrame = 2,
        },
        pci::GpuResidencyRecord{
            .key = protectedOldest,
            .bytes = 16,
            .lastUsedFrame = 1,
            .lastVisibleFrame = 0,
        },
    };

    CHECK(pci::planGpuEvictions(
              records, 48, 16, 48, std::array{protectedOldest}) ==
          std::vector{first});
    CHECK(pci::planGpuEvictions(
              records, 48, 32, 48, std::array{protectedOldest}) ==
          std::vector{first, second});
}

TEST_CASE("upload planning always makes progress", "[qt][renderer]")
{
    CHECK(pci::planUploadCount({100 * 1024 * 1024}, 48 * 1024 * 1024) == 1);
}

} // namespace
