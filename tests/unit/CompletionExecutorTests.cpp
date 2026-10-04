#include "support/DeterministicCompletionExecutor.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <barrier>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

TEST_CASE("completion delivery is always deferred and accepts move-only work",
          "[unit][runtime-resources][dispatch]")
{
    pci::test::DeterministicCompletionExecutor executor;
    auto owned = std::make_unique<int>(41);
    int delivered = 0;

    REQUIRE(executor.post(pci::CompletionExecutor::Completion{
        [value = std::move(owned), &delivered] {
            delivered = *value + 1;
        }}));
    CHECK(delivered == 0);
    CHECK(executor.pendingWakeCount() == 1);

    REQUIRE(executor.runNext());
    CHECK(delivered == 42);
    CHECK(executor.pendingWakeCount() == 0);
    CHECK_FALSE(executor.runNext());
}

TEST_CASE("concurrent completion enqueue schedules one owner wake",
          "[unit][runtime-resources][dispatch][concurrency]")
{
    constexpr std::size_t threadCount = 8;
    constexpr std::size_t callbacksPerThread = 64;
    pci::test::DeterministicCompletionExecutor executor;
    std::barrier startLine(static_cast<std::ptrdiff_t>(threadCount));
    std::atomic<std::size_t> deliveries = 0;
    std::atomic<bool> accepted = true;
    std::vector<std::jthread> workers;
    workers.reserve(threadCount);

    for (std::size_t thread = 0; thread < threadCount; ++thread) {
        workers.emplace_back([&] {
            startLine.arrive_and_wait();
            for (std::size_t callback = 0; callback < callbacksPerThread;
                 ++callback) {
                if (!executor.post(
                        pci::CompletionExecutor::Completion{[&deliveries] {
                            deliveries.fetch_add(1, std::memory_order_relaxed);
                        }})) {
                    accepted.store(false, std::memory_order_relaxed);
                }
            }
        });
    }
    workers.clear();

    REQUIRE(accepted.load(std::memory_order_relaxed));
    CHECK(deliveries.load(std::memory_order_relaxed) == 0);
    CHECK(executor.pendingWakeCount() == 1);
    REQUIRE(executor.runNext());
    CHECK(deliveries.load(std::memory_order_relaxed) ==
          threadCount * callbacksPerThread);
}

TEST_CASE("completion queued during drain receives a later wake",
          "[unit][runtime-resources][dispatch]")
{
    pci::test::DeterministicCompletionExecutor executor;
    int first = 0;
    int second = 0;

    auto completion = pci::CompletionExecutor::Completion{[&] {
        ++first;
        const bool posted =
            executor.post(pci::CompletionExecutor::Completion{[&second] {
                ++second;
            }});
        REQUIRE(posted);
    }};
    const bool posted = executor.post(std::move(completion));
    REQUIRE(posted);

    REQUIRE(executor.runNext());
    CHECK(first == 1);
    CHECK(second == 0);
    CHECK(executor.pendingWakeCount() == 1);

    REQUIRE(executor.runNext());
    CHECK(second == 1);
}

TEST_CASE("completion invalidation releases accepted and rejected state",
          "[unit][runtime-resources][dispatch][invalidation]")
{
    pci::test::DeterministicCompletionExecutor executor;
    int deliveries = 0;
    auto acceptedState = std::make_shared<int>(1);
    std::weak_ptr<int> acceptedLifetime = acceptedState;

    REQUIRE(executor.post(pci::CompletionExecutor::Completion{
        [state = acceptedState, &deliveries] {
            deliveries += *state;
        }}));
    acceptedState.reset();
    REQUIRE_FALSE(acceptedLifetime.expired());

    executor.invalidate();
    CHECK(acceptedLifetime.expired());
    REQUIRE(executor.runNext());
    CHECK(deliveries == 0);

    auto rejectedState = std::make_shared<int>(2);
    std::weak_ptr<int> rejectedLifetime = rejectedState;
    CHECK_FALSE(executor.post(pci::CompletionExecutor::Completion{
        [state = rejectedState, &deliveries] {
            deliveries += *state;
        }}));
    rejectedState.reset();
    CHECK(rejectedLifetime.expired());
    CHECK(deliveries == 0);
}

TEST_CASE("failed wake scheduling rejects work without retaining it",
          "[unit][runtime-resources][dispatch][shutdown]")
{
    pci::test::DeterministicCompletionExecutor executor;
    executor.rejectWakeRequests();
    auto state = std::make_shared<int>(7);
    std::weak_ptr<int> lifetime = state;

    CHECK_FALSE(executor.post(pci::CompletionExecutor::Completion{[state] {
        static_cast<void>(*state);
    }}));
    state.reset();
    CHECK(lifetime.expired());
    CHECK(executor.pendingWakeCount() == 0);
}
