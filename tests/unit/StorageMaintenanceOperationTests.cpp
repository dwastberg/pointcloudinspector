#include "support/DeterministicCompletionExecutor.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <pci/operations/StorageMaintenanceOperation.h>
#include <thread>

namespace {
class MaintenanceProvider final : public pci::StorageMaintenance {
public:
    bool block = false;
    bool fail = false;
    mutable std::atomic_uint calls{0};
    pci::StorageMaintenanceResult run(pci::StorageMaintenanceAction,
                                      std::stop_token stop,
                                      const Progress &progress) const override
    {
        ++calls;
        while (block && !stop.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (fail)
            throw std::runtime_error("injected failure");
        for (std::uint64_t i = 0; i < 10'000; ++i)
            progress(i);
        pci::StorageMaintenanceResult result;
        result.removedBytes = 123;
        result.cancelled = stop.stop_requested();
        return result;
    }
};
struct Fixture {
    pci::TaskScheduler scheduler{1};
    std::shared_ptr<pci::test::DeterministicCompletionExecutor> executor =
        std::make_shared<pci::test::DeterministicCompletionExecutor>();
    std::shared_ptr<MaintenanceProvider> provider =
        std::make_shared<MaintenanceProvider>();
    std::unique_ptr<pci::StorageMaintenanceOperation> operation =
        std::make_unique<pci::StorageMaintenanceOperation>(
            provider, scheduler, executor);
    void drain()
    {
        while (executor->runNext()) {
        }
    }
};
} // namespace
TEST_CASE(
    "storage operations bound progress and recover rejected completion wakes",
    "[operations][storage]")
{
    Fixture f;
    f.executor->rejectWakeRequests();
    std::shared_ptr<const pci::StorageMaintenanceResult> result;
    auto subscription = f.operation->start(
        pci::StorageMaintenanceAction::CleanUnused, [&](const auto &update) {
            result = update.result;
        });
    f.scheduler.waitForIdle();
    CHECK_FALSE(result);
    CHECK(f.executor->pendingWakeCount() <= 2);
    f.operation->recoverDelivery();
    REQUIRE(result);
    CHECK(result->removedBytes == 123);
}
TEST_CASE(
    "storage observers detach and shutdown cancels without a widget callback",
    "[operations][storage]")
{
    Fixture f;
    f.provider->block = true;
    int calls = 0;
    auto subscription = f.operation->start(pci::StorageMaintenanceAction::Scan,
                                           [&](const auto &) {
                                               ++calls;
                                           });
    CHECK_THROWS(f.operation->start(pci::StorageMaintenanceAction::Scan, {}));
    SECTION("observer closes")
    {
        subscription.reset();
    }
    SECTION("session closes")
    {
        f.operation.reset();
    }
    f.scheduler.waitForIdle();
    f.drain();
    CHECK(calls == 0);
}
TEST_CASE(
    "storage failures finish and old subscriptions cannot cancel a new request",
    "[operations][storage]")
{
    Fixture f;
    f.provider->fail = true;
    std::shared_ptr<const pci::StorageMaintenanceResult> result;
    auto first = f.operation->start(pci::StorageMaintenanceAction::Scan,
                                    [&](const auto &update) {
                                        result = update.result;
                                    });
    f.scheduler.waitForIdle();
    f.drain();
    REQUIRE(result);
    CHECK(result->errorCount == 1);
    f.provider->fail = false;
    result.reset();
    auto second = f.operation->start(pci::StorageMaintenanceAction::Scan,
                                     [&](const auto &update) {
                                         result = update.result;
                                     });
    first.reset();
    f.scheduler.waitForIdle();
    f.drain();
    REQUIRE(result);
    CHECK_FALSE(result->cancelled);
    CHECK(result->errorCount == 0);
}
