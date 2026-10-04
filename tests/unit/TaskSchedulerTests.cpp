#include <pci/tasking/TaskScheduler.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool waitUntil(const auto &predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return predicate();
}

TEST_CASE("task scheduler bounds workers and active byte permits",
          "[unit][scheduler][concurrency][residency]")
{
    pci::TaskScheduler scheduler(3, 100);
    std::atomic<int> active = 0;
    std::atomic<int> peakActive = 0;
    std::atomic<bool> release = false;
    const auto task = [&] {
        const int now = ++active;
        int peak = peakActive.load();
        while (now > peak && !peakActive.compare_exchange_weak(peak, now)) {
        }
        while (!release.load()) {
            std::this_thread::sleep_for(1ms);
        }
        --active;
    };

    for (int index = 0; index < 8; ++index) {
        static_cast<void>(
            scheduler.submit(pci::TaskPriority::Import, 60, task));
    }
    REQUIRE(waitUntil([&] {
        return active.load() != 0;
    }));
    CHECK(active.load() == 1);
    CHECK(peakActive.load() == 1);
    CHECK(scheduler.metrics().activeEstimatedBytes == 60);
    release = true;
    scheduler.waitForIdle();

    const pci::TaskSchedulerMetrics metrics = scheduler.metrics();
    CHECK(metrics.started == 8);
    CHECK(metrics.completed == 8);
    CHECK(metrics.peakActive == 1);
    CHECK(metrics.peakActiveEstimatedBytes == 60);
}

TEST_CASE("task scheduler rotates equal-priority source groups",
          "[unit][scheduler][fairness][residency]")
{
    pci::TaskScheduler scheduler(1, 100);
    std::atomic<bool> blockerActive = false;
    std::atomic<bool> releaseBlocker = false;
    static_cast<void>(scheduler.submit(
        pci::TaskPriority::Inspection,
        1,
        [&] {
            blockerActive = true;
            while (!releaseBlocker.load()) {
                std::this_thread::sleep_for(1ms);
            }
        },
        {},
        99));
    REQUIRE(waitUntil([&] {
        return blockerActive.load();
    }));

    std::mutex orderMutex;
    std::vector<std::uint64_t> order;
    const auto record = [&](const std::uint64_t group) {
        const std::scoped_lock lock(orderMutex);
        order.push_back(group);
    };
    for (int request = 0; request < 3; ++request) {
        static_cast<void>(scheduler.submit(
            pci::TaskPriority::VisibleCoverage,
            1,
            [&, group = std::uint64_t{1}] {
                record(group);
            },
            {},
            1));
    }
    for (int request = 0; request < 3; ++request) {
        static_cast<void>(scheduler.submit(
            pci::TaskPriority::VisibleCoverage,
            1,
            [&, group = std::uint64_t{2}] {
                record(group);
            },
            {},
            2));
    }
    releaseBlocker = true;
    scheduler.waitForIdle();

    REQUIRE(order.size() == 6);
    CHECK(order == std::vector<std::uint64_t>{1, 2, 1, 2, 1, 2});
    CHECK(scheduler.metrics().fairnessGroups == 0);
}

TEST_CASE("queued tasks can be promoted without replacement",
          "[unit][scheduler][priority][recovery]")
{
    pci::TaskScheduler scheduler(1, 10);
    std::atomic_bool release = false;
    std::atomic_bool active = false;
    std::mutex orderMutex;
    std::vector<int> order;
    static_cast<void>(
        scheduler.submit(pci::TaskPriority::VisibleCoverage, 1, [&] {
            active = true;
            while (!release.load()) {
                std::this_thread::yield();
            }
        }));
    REQUIRE(waitUntil([&] {
        return active.load();
    }));
    static_cast<void>(scheduler.submit(pci::TaskPriority::Background, 1, [&] {
        const std::scoped_lock lock(orderMutex);
        order.push_back(1);
    }));
    const auto promoted =
        scheduler.submit(pci::TaskPriority::Background, 1, [&] {
            const std::scoped_lock lock(orderMutex);
            order.push_back(2);
        });
    REQUIRE(
        scheduler.reprioritize(promoted, pci::TaskPriority::VisibleCoverage));
    release = true;
    scheduler.waitForIdle();
    CHECK(order == std::vector{2, 1});
    CHECK_FALSE(
        scheduler.reprioritize(promoted, pci::TaskPriority::VisibleCoverage));
}

TEST_CASE("task scheduler prioritizes queued work and cancels by id",
          "[unit][scheduler][priority][cancellation]")
{
    pci::TaskScheduler scheduler(1, 10);
    std::atomic_bool blockerActive = false;
    std::atomic_bool releaseBlocker = false;
    std::atomic_int cancellationCallbacks = 0;
    std::mutex orderMutex;
    std::vector<int> order;

    static_cast<void>(scheduler.submit(pci::TaskPriority::Inspection, 1, [&] {
        blockerActive = true;
        while (!releaseBlocker.load()) {
            std::this_thread::yield();
        }
    }));
    REQUIRE(waitUntil([&] {
        return blockerActive.load();
    }));

    static_cast<void>(scheduler.submit(pci::TaskPriority::Background, 1, [&] {
        const std::scoped_lock lock(orderMutex);
        order.push_back(1);
    }));
    static_cast<void>(
        scheduler.submit(pci::TaskPriority::VisibleCoverage, 1, [&] {
            const std::scoped_lock lock(orderMutex);
            order.push_back(2);
        }));
    const auto cancelled = scheduler.submit(
        pci::TaskPriority::VisibleDetail,
        1,
        [&] {
            const std::scoped_lock lock(orderMutex);
            order.push_back(3);
        },
        [&] {
            ++cancellationCallbacks;
        });

    REQUIRE(scheduler.cancel(cancelled));
    CHECK_FALSE(scheduler.cancel(cancelled));
    CHECK(cancellationCallbacks.load() == 1);

    releaseBlocker = true;
    scheduler.waitForIdle();

    CHECK(order == std::vector{2, 1});
    const pci::TaskSchedulerMetrics metrics = scheduler.metrics();
    CHECK(metrics.submitted == 4);
    CHECK(metrics.started == 3);
    CHECK(metrics.completed == 3);
    CHECK(metrics.cancelled == 1);
    CHECK(metrics.pending == 0);
}

TEST_CASE("task scheduler contains task exceptions",
          "[unit][scheduler][exceptions]")
{
    pci::TaskScheduler scheduler(1, 10);
    std::atomic_int completedAfterFailure = 0;

    static_cast<void>(scheduler.submit(pci::TaskPriority::Import, 1, [] {
        throw std::runtime_error("expected scheduler test failure");
    }));
    static_cast<void>(scheduler.submit(pci::TaskPriority::Import, 1, [&] {
        ++completedAfterFailure;
    }));
    scheduler.waitForIdle();

    CHECK(completedAfterFailure.load() == 1);
    const pci::TaskSchedulerMetrics metrics = scheduler.metrics();
    CHECK(metrics.started == 2);
    CHECK(metrics.completed == 2);
}

TEST_CASE("task scheduler shutdown cancels queued work and joins workers",
          "[unit][scheduler][shutdown][cancellation]")
{
    auto scheduler = std::make_unique<pci::TaskScheduler>(1, 10);
    std::atomic_bool blockerActive = false;
    std::atomic_bool releaseBlocker = false;
    std::atomic_bool shutdownComplete = false;
    std::atomic_int cancellationCallbacks = 0;

    static_cast<void>(scheduler->submit(pci::TaskPriority::Inspection, 1, [&] {
        blockerActive = true;
        while (!releaseBlocker.load()) {
            std::this_thread::yield();
        }
    }));
    REQUIRE(waitUntil([&] {
        return blockerActive.load();
    }));

    for (int index = 0; index < 2; ++index) {
        static_cast<void>(scheduler->submit(
            pci::TaskPriority::Background,
            1,
            [] {},
            [&] {
                ++cancellationCallbacks;
            }));
    }

    std::jthread shutdown([&] {
        scheduler.reset();
        shutdownComplete = true;
    });
    REQUIRE(waitUntil([&] {
        return cancellationCallbacks.load() == 2;
    }));
    CHECK_FALSE(shutdownComplete.load());

    releaseBlocker = true;
    shutdown.join();
    CHECK(shutdownComplete.load());
}

} // namespace
