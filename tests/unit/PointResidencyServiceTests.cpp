#include <pci/runtime/point/PointResidencyService.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <vector>

TEST_CASE("point residency service owns one coherent shared resource set",
          "[point-runtime][runtime][residency][unit]")
{
    constexpr std::uint64_t byteBudget = 4096;
    constexpr std::uint64_t reservationBytes = 1024;
    auto memory = std::make_shared<pci::PointMemoryBudget>(byteBudget);
    auto admission = std::make_shared<pci::HierarchyDecodeAdmission>(3);

    pci::PointResidencyService service(byteBudget, 3, admission, memory);

    CHECK(service.memoryBudget() == memory);
    CHECK(service.coordinator()->decodeAdmission() == admission);
    CHECK(service.decodedByteBudget() == byteBudget);
    CHECK(service.cache()->byteBudget() == byteBudget);
    CHECK(service.scheduler()->maximumWorkers() == 3);

    const auto reservation = memory->tryReserve(reservationBytes);
    REQUIRE(reservation);
    service.syncCacheBudget();

    CHECK(service.cache()->byteBudget() == byteBudget - reservationBytes);
    CHECK(service.decodedResidentBytes() == 0);
}

TEST_CASE("point completion slots bound queued running and prepared work",
          "[point-runtime][residency][completion][unit]")
{
    auto budget = std::make_shared<pci::PointMemoryBudget>(1024);
    pci::PointDecodeQueue queue(budget);
    std::vector<std::shared_ptr<pci::PointDecodeQueue::Slot>> slots;
    for (std::size_t i = 0; i < pci::PointDecodeQueue::capacity; ++i) {
        auto slot = queue.reserve(8);
        REQUIRE(slot);
        slots.push_back(std::move(slot));
    }
    CHECK_FALSE(queue.reserve(1));
    CHECK(budget->reservedBytes() == 512);
    slots.pop_back();
    REQUIRE(queue.reserve(8));
    slots.clear();
    CHECK(budget->reservedBytes() == 0);
    auto full = queue.reserve(1024);
    REQUIRE(full);
    CHECK_FALSE(queue.reserve(1));
    CHECK_FALSE(full->fit(1025));
    CHECK(budget->reservedBytes() == 1024);
    full.reset();
    CHECK(budget->reservedBytes() == 0);
}

TEST_CASE(
    "frame protection spans sources through cache admission and budget shrink",
    "[point-runtime][residency][protection][unit]")
{
    pci::DecodedPageCache cache(1024);
    const auto page = [](pci::PointCloudNodeId id) {
        auto payload = std::make_shared<pci::PointCloudNodePayload>();
        auto block = std::make_shared<pci::PointBlock>();
        block->points.resize(1);
        payload->nodeId = id;
        payload->blocks.push_back(std::move(block));
        return payload;
    };
    const auto child = pci::childNodeId(pci::rootPointCloudNode, 0);
    cache.insert(pci::PointCloudSourceId{1}, page(child));
    cache.insert(pci::PointCloudSourceId{2}, page(child));
    const std::array protect{
        pci::DecodedPageKey{pci::PointCloudSourceId{1}, child}};
    cache.setFrameProtection(protect);
    cache.setByteBudget(1);
    CHECK(cache.peek(pci::PointCloudSourceId{1}, child));
    CHECK_FALSE(cache.peek(pci::PointCloudSourceId{2}, child));
    cache.insert(pci::PointCloudSourceId{3}, page(child));
    cache.trim();
    CHECK(cache.peek(pci::PointCloudSourceId{1}, child));
    cache.setFrameProtection({});
    cache.trim();
    CHECK(cache.size() == 0);
}
