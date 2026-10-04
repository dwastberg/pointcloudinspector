#include "support/DeterministicCompletionExecutor.h"
#include <pci/operations/StatisticsOperation.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <semaphore>
#include <thread>

namespace {
class StatisticsProvider final : public pci::PointCloudStatisticsProvider {
public:
    mutable std::atomic_uint calls{0};
    bool block = false;
    pci::PointCloudStatistics calculate(const pci::PointCloudMetadata &,
                                        std::stop_token stop,
                                        Progress progress) const override
    {
        ++calls;
        while (block && !stop.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (stop.stop_requested())
            throw pci::PointCloudStatisticsCancelled();
        for (std::uint64_t i = 0; i < 10'000; ++i)
            progress(i, 10'000);
        return {.sourcePointCount = 10'000, .scannedPointCount = 10'000};
    }
};
struct Fixture {
    pci::TaskScheduler scheduler{1};
    pci::OperationRegistry registry;
    std::shared_ptr<pci::test::DeterministicCompletionExecutor> executor =
        std::make_shared<pci::test::DeterministicCompletionExecutor>();
    std::shared_ptr<StatisticsProvider> provider =
        std::make_shared<StatisticsProvider>();
    std::shared_ptr<pci::LoadJobIdSequence> ids =
        std::make_shared<pci::LoadJobIdSequence>();
    bool valid = true;
    std::unique_ptr<pci::StatisticsOperation> operation =
        std::make_unique<pci::StatisticsOperation>(
            provider, scheduler, executor, ids, registry, [this](const auto &) {
                return valid;
            });
    pci::PointCloudMetadata metadata{.sourcePath = "fixture.laz",
                                     .sourceDriver = "readers.las"};
    pci::StatisticsBindingToken binding{pci::SessionGeneration{1},
                                        pci::PointCloudLayerId{1},
                                        pci::PointCloudSourceId{1},
                                        pci::BindingGeneration{1}};
    void drain()
    {
        while (executor->runNext()) {
        }
    }
};
} // namespace

TEST_CASE("statistics uses bounded operation delivery and preserves results",
          "[operations][statistics][unit]")
{
    Fixture fixture;
    int updates = 0;
    std::shared_ptr<const pci::JobResult<pci::PointCloudStatistics>> result;
    auto subscription = fixture.operation->start(
        fixture.metadata, fixture.binding, [&](const auto &update) {
            ++updates;
            result = update.result;
        });
    fixture.scheduler.waitForIdle();
    CHECK_FALSE(result);
    CHECK(fixture.executor->pendingWakeCount() <= 2);
    fixture.drain();
    REQUIRE(result);
    REQUIRE(*result);
    CHECK((*result)->scannedPointCount == 10'000);
    CHECK(updates <= 3);
    const auto row = fixture.registry.find(subscription.id());
    REQUIRE(row);
    CHECK(row->state == pci::OperationState::Succeeded);
    CHECK_FALSE(row->capabilities.canRetry);
}

TEST_CASE("statistics stale source results cancel and rejected wakes recover",
          "[operations][statistics][stale][unit]")
{
    Fixture fixture;
    fixture.executor->rejectWakeRequests();
    std::shared_ptr<const pci::JobResult<pci::PointCloudStatistics>> result;
    auto subscription = fixture.operation->start(
        fixture.metadata, fixture.binding, [&](const auto &update) {
            result = update.result;
        });
    fixture.scheduler.waitForIdle();
    CHECK_FALSE(result);
    fixture.valid = false;
    fixture.operation->recoverDelivery();
    REQUIRE(result);
    REQUIRE_FALSE(*result);
    CHECK(result->error().code == pci::JobErrorCode::Cancelled);
    CHECK(fixture.registry.find(subscription.id())->state ==
          pci::OperationState::Cancelled);
}

TEST_CASE("statistics queued cancellation never needs a free worker",
          "[operations][statistics][shutdown][unit]")
{
    Fixture fixture;
    std::binary_semaphore entered{0};
    std::binary_semaphore release{0};
    static_cast<void>(
        fixture.scheduler.submit(pci::TaskPriority::Inspection, 1, [&] {
            entered.release();
            release.acquire();
        }));
    entered.acquire();
    auto subscription =
        fixture.operation->start(fixture.metadata, fixture.binding, {});
    CHECK(fixture.registry.cancel(subscription.id()));
    fixture.drain();
    const auto terminal = fixture.registry.find(subscription.id());
    release.release();
    fixture.scheduler.waitForIdle();
    REQUIRE(terminal);
    CHECK(terminal->state == pci::OperationState::Cancelled);
    CHECK(fixture.provider->calls == 0);
}

TEST_CASE("statistics observers detach before cancellation and survive owner "
          "shutdown",
          "[operations][statistics][shutdown][unit]")
{
    Fixture fixture;
    fixture.provider->block = true;
    int updates = 0;
    auto subscription = fixture.operation->start(
        fixture.metadata, fixture.binding, [&](const auto &) {
            ++updates;
        });
    const auto before = updates;
    SECTION("dialog close")
    {
        subscription.reset();
    }
    SECTION("session destruction")
    {
        fixture.operation.reset();
    }
    fixture.scheduler.waitForIdle();
    fixture.drain();
    CHECK(updates == before);
}

TEST_CASE("statistics reruns receive fresh globally allocated identities",
          "[operations][statistics][identity][unit]")
{
    Fixture fixture;
    const auto importId = fixture.ids->next("exhausted");
    auto first =
        fixture.operation->start(fixture.metadata, fixture.binding, {});
    fixture.scheduler.waitForIdle();
    fixture.drain();
    auto second =
        fixture.operation->start(fixture.metadata, fixture.binding, {});
    fixture.scheduler.waitForIdle();
    fixture.drain();
    CHECK(importId != first.id());
    CHECK(first.id() != second.id());
    fixture.registry.clear();
    CHECK(fixture.ids->next("exhausted") > second.id());
}
