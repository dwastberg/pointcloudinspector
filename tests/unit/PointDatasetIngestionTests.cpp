#include <pci/operations/PointDatasetIngestion.h>
#include <pci/operations/PointDatasetInstallation.h>

#include <catch2/catch_test_macros.hpp>

#include <future>
#include <latch>
#include <thread>

namespace {

pci::PointBlockPtr block()
{
    auto result = std::make_shared<pci::PointBlock>();
    result->points.resize(2);
    result->attributes.resize(2);
    result->bounds = {{0, 0, 0}, {1, 1, 1}};
    return result;
}

pci::PointDatasetEvent event(std::uint64_t index = 0)
{
    return {
        {pci::SessionGeneration{1}, 1},
        index + 1,
        pci::PreparedPointBlock{pci::PointCloudSourceId{1}, index, block()}};
}

} // namespace

TEST_CASE("ingestion charges taken events until installation releases them",
          "[point-import][queue][memory]")
{
    const auto bytes = event().retainedBytes();
    pci::PointDatasetIngestion queue(bytes, [] {});
    REQUIRE(queue.push(event(), {}));
    auto taken = queue.takeEvent();
    REQUIRE(taken);
    CHECK(queue.retainedBytes() == bytes);
    std::latch entered{1};
    std::promise<bool> result;
    auto completed = result.get_future();
    std::jthread producer([&](std::stop_token stop) {
        entered.count_down();
        result.set_value(queue.push(event(1), stop));
    });
    entered.wait();
    CHECK(completed.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::timeout);
    taken.reset();
    queue.releaseEvent();
    REQUIRE(completed.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready);
    CHECK(completed.get());
    CHECK(queue.retainedBytes() == bytes);
    queue.close();
    CHECK(queue.retainedBytes() == 0);
}

TEST_CASE(
    "full ingestion queues wake cancelled producers without owner processing",
    "[point-import][queue][shutdown]")
{
    pci::PointDatasetIngestion queue(1024 * 1024, [] {});
    for (std::uint64_t i = 0; i < pci::PointDatasetIngestion::eventCapacity;
         ++i) {
        REQUIRE(queue.push(event(i), {}));
    }
    std::latch entered{1};
    std::promise<bool> result;
    auto completed = result.get_future();
    std::jthread producer([&](std::stop_token stop) {
        entered.count_down();
        result.set_value(queue.push(event(64), stop));
    });
    entered.wait();
    SECTION("stop token interrupts backpressure")
    {
        producer.request_stop();
    }
    SECTION("queue invalidation interrupts backpressure")
    {
        queue.close();
    }
    REQUIRE(completed.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready);
    CHECK_FALSE(completed.get());
}

TEST_CASE(
    "ingestion preserves its terminal slot under saturated data and progress",
    "[point-import][queue][terminal]")
{
    pci::PointDatasetIngestion queue(1024 * 1024, [] {});
    for (std::uint64_t i = 0; i < pci::PointDatasetIngestion::eventCapacity;
         ++i) {
        REQUIRE(queue.push(event(i), {}));
        queue.progress({pci::PointCloudImportStage::Reading, i, 64});
    }
    pci::PointDatasetPreparation preparation({});
    auto dataset = preparation.finish();
    queue.finish(dataset);
    queue.progress({pci::PointCloudImportStage::Reading, 999, 999});
    REQUIRE(queue.takeProgress()->processed == 63);
    CHECK_FALSE(queue.takeOutcome());
    for (std::uint64_t i = 0; i < 64; ++i) {
        auto next = queue.takeEvent();
        REQUIRE(next);
        CHECK(next->sequence == i + 1);
        next.reset();
        queue.releaseEvent();
    }
    auto outcome = queue.takeOutcome();
    REQUIRE(outcome);
    REQUIRE(*outcome);
    CHECK(**outcome == dataset);
    queue.finish(dataset);
    CHECK_FALSE(queue.takeOutcome());
}

TEST_CASE(
    "ingestion failure discards queued ownership and oversized events fail",
    "[point-import][queue][failure]")
{
    pci::PointDatasetIngestion queue(event().retainedBytes(), [] {});
    auto value = event();
    std::weak_ptr<const pci::PointBlock> lifetime =
        std::get<pci::PreparedPointBlock>(value.data).block;
    const bool queued = queue.push(std::move(value), {});
    REQUIRE(queued);
    queue.finish(std::unexpected(pci::JobError{.code = pci::JobErrorCode::Io,
                                               .message = "read failed"}));
    CHECK(lifetime.expired());
    CHECK(queue.retainedBytes() == 0);
    REQUIRE(queue.takeOutcome());
    pci::PointDatasetIngestion tooSmall(1, [] {});
    CHECK_THROWS_AS(tooSmall.push(event(), {}), pci::PointCloudImportError);
    CHECK(tooSmall.retainedBytes() == 0);
}

TEST_CASE("prepared flat allocations retain admission after runtime removal",
          "[point-import][ownership][snapshot]")
{
    const auto budget = std::make_shared<pci::PointMemoryBudget>(4096);
    auto reservation = budget->tryReserve(2048);
    REQUIRE(reservation);
    pci::PointDatasetPreparation preparation({});
    preparation.setResidentMemoryReservation(*reservation);
    preparation.addBlock(block());
    auto prepared = preparation.finish();
    auto runtime = pci::createPointDatasetRuntime(prepared);
    auto snapshot = runtime->snapshot();
    auto upload = snapshot.flatBlocks.front().block;
    reservation.reset();
    prepared.reset();
    runtime.reset();
    snapshot = {};
    CHECK(budget->reservedBytes() == 2048);
    upload.reset();
    CHECK(budget->reservedBytes() == 0);
}

TEST_CASE("prepared publications preserve active data until an explicit commit",
          "[point-import][transaction]")
{
    auto runtime =
        std::make_shared<pci::PointDatasetRuntime>(pci::PointCloudMetadata{});
    const auto original = runtime->snapshot();
    auto abandoned = runtime->preparePublication(block());
    CHECK(abandoned->view().availability.pointCount == 2);
    CHECK(runtime->snapshot().revision == original.revision);
    CHECK(runtime->totalPointCount() == 0);
    abandoned.reset();
    auto first = runtime->preparePublication(block());
    auto stale = runtime->preparePublication(block());
    REQUIRE(runtime->commitPublication(*first));
    CHECK(runtime->totalPointCount() == 2);
    CHECK_FALSE(runtime->commitPublication(*stale));
    CHECK_FALSE(runtime->commitPublication(*first));
    CHECK(runtime->totalPointCount() == 2);
}

TEST_CASE("completion rejects missing or duplicated ingested blocks",
          "[point-import][sequence]")
{
    pci::PreparedPointDatasetPtr preview;
    std::vector<pci::PointDatasetEvent> events;
    pci::PointDatasetPreparation preparation({});
    preparation.publish({.dataReady = [&](pci::PointDatasetEvent value) {
        events.push_back(std::move(value));
    }});
    preparation.addBlock(block());
    auto dataset = preparation.finish();
    preview = std::get<pci::PreparedPointDatasetPtr>(events.front().data);
    auto runtime = pci::createPointDatasetRuntime(preview);
    CHECK_THROWS_AS(pci::completePointDatasetRuntime(runtime, dataset),
                    std::invalid_argument);
    runtime->addBlock(
        std::get<pci::PreparedPointBlock>(events.back().data).block);
    pci::completePointDatasetRuntime(runtime, dataset);
    CHECK(runtime->snapshot().loadingComplete);
    CHECK(runtime->blocks() == dataset->blocks);
    CHECK(preview->blocks.empty());
    CHECK_FALSE(preview->loadingComplete);
}

namespace {
class PreparedRootSource final : public pci::PointCloudDataSource {
public:
    pci::PointCloudNode rootNode() const override
    {
        return {};
    }
    pci::PointCloudNode node(pci::PointCloudNodeId id) const override
    {
        return {.id = id};
    }
    pci::PointCloudNodePayloadPtr loadNode(pci::PointCloudNodeId,
                                           std::stop_token) const override
    {
        throw std::logic_error(
            "installing prepared roots must not read the source again");
    }
};
} // namespace

TEST_CASE("prepared hierarchy reservations follow the last exported root block",
          "[point-import][memory][hierarchy]")
{
    auto root = std::make_shared<pci::PointCloudNodePayload>();
    root->blocks.push_back(block());
    const auto bytes = pci::pointCloudNodePayloadBytes(*root);
    auto budget = std::make_shared<pci::PointMemoryBudget>(bytes * 2);
    pci::PointDatasetPreparation preparation(
        {}, std::make_shared<PreparedRootSource>(), root);
    preparation.reserveRoot(budget, bytes);
    auto dataset = preparation.finish();
    CHECK(dataset->payloadBytes() == bytes);
    CHECK(budget->reservedBytes() == bytes);
    auto runtime = pci::createPointDatasetRuntime(dataset, bytes * 2);
    CHECK(runtime->sourceId() == dataset->descriptor.sourceId);
    CHECK(runtime->reservedRootBytes() == bytes);
    auto upload =
        runtime->peekNodePayload(pci::rootPointCloudNode)->blocks.front();
    CHECK(upload->points.data() == root->blocks.front()->points.data());
    dataset.reset();
    runtime.reset();
    root.reset();
    CHECK(budget->reservedBytes() == bytes);
    CHECK(budget->availableBytes() == bytes);
    upload.reset();
    CHECK(budget->reservedBytes() == 0);
}

TEST_CASE(
    "root admission rejects exhausted budgets without growing the allowance",
    "[point-import][memory][hierarchy]")
{
    auto root = std::make_shared<pci::PointCloudNodePayload>();
    root->blocks.push_back(block());
    const auto bytes = pci::pointCloudNodePayloadBytes(*root);
    auto budget = std::make_shared<pci::PointMemoryBudget>(bytes);
    auto oldReservation = budget->tryReserve(bytes);
    REQUIRE(oldReservation);
    pci::PointDatasetPreparation preparation(
        {}, std::make_shared<PreparedRootSource>(), root);
    CHECK_THROWS_AS(preparation.reserveRoot(budget, bytes), std::length_error);
    CHECK(budget->byteBudget() == bytes);
    CHECK(budget->reservedBytes() == bytes);
}

TEST_CASE("a fully reserved root can detach to standalone residency",
          "[point-import][memory][hierarchy][regression]")
{
    auto root = std::make_shared<pci::PointCloudNodePayload>();
    root->blocks.push_back(block());
    const auto bytes = pci::pointCloudNodePayloadBytes(*root);
    auto memory = std::make_shared<pci::PointMemoryBudget>(bytes);
    pci::PointDatasetPreparation preparation(
        {}, std::make_shared<PreparedRootSource>(), root);
    preparation.reserveRoot(memory, bytes);
    auto runtime = pci::createPointDatasetRuntime(preparation.finish(), bytes);
    auto coordinator = std::make_shared<pci::HierarchyResidencyCoordinator>(
        bytes, 1, pci::HierarchyDecodeAdmissionPtr{}, memory);
    auto cache = std::make_shared<pci::DecodedPageCache>(bytes);
    auto scheduler = std::make_shared<pci::TaskScheduler>(1);
    auto attachment =
        runtime->prepareAttachment(coordinator, cache, scheduler, true, bytes);
    runtime->quiesceForAttachment();
    runtime->commitAttachment(*attachment);
    attachment.reset();
    CHECK(memory->availableBytes() == 0);
    CHECK_NOTHROW(runtime->useStandaloneHierarchyResidency());
    CHECK(runtime->peekNodePayload(pci::rootPointCloudNode));
    CHECK(memory->reservedBytes() == bytes);
}
