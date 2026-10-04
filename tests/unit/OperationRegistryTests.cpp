#include "support/DeterministicCompletionExecutor.h"
#include <pci/operations/OperationLifecycle.h>
#include <pci/operations/OperationTarget.h>

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
pci::OperationSnapshot
row(pci::LoadJobId id,
    pci::OperationState state = pci::OperationState::Running,
    pci::AttemptGeneration attempt = pci::AttemptGeneration{1})
{
    return {.token = {id, attempt},
            .kind = pci::LoadJobKind::Raster,
            .state = state,
            .target = std::nullopt,
            .title = "Raster",
            .detail = {},
            .completion = 0,
            .capabilities = pci::terminal(state)
                                ? pci::terminalLoadJobCapabilities(true)
                                : pci::activeLoadJobCapabilities()};
}
} // namespace

TEST_CASE(
    "operation registry completes each attempt once and rejects stale progress",
    "[operations][registry][unit]")
{
    pci::OperationRegistry registry;
    const pci::LoadJobId id{1};
    int terminalEvents = 0;
    registry.setObserver([&](auto, const auto &value) {
        terminalEvents += pci::terminal(value.state) ? 1 : 0;
    });
    registry.insert(row(id), {});
    CHECK(registry.complete(row(id, pci::OperationState::Failed)));
    CHECK_FALSE(registry.complete(row(id, pci::OperationState::Succeeded)));
    CHECK_FALSE(registry.update(row(id)));
    CHECK(terminalEvents == 1);
    REQUIRE(registry.beginAttempt({id, pci::AttemptGeneration{2}}));
    CHECK_FALSE(registry.complete(row(id, pci::OperationState::Succeeded)));
    CHECK(registry.complete(
        row(id, pci::OperationState::Succeeded, pci::AttemptGeneration{2})));
    CHECK(terminalEvents == 2);
}

TEST_CASE(
    "operation controls survive reentrant row removal and obey capabilities",
    "[operations][registry][unit]")
{
    pci::OperationRegistry registry;
    const pci::LoadJobId id{1};
    int cancelled = 0;
    registry.insert(row(id),
                    {.cancel =
                         [&] {
                             ++cancelled;
                             CHECK(registry.remove(id));
                         },
                     .retry = {},
                     .prioritize = {},
                     .dismiss = {}});
    CHECK_FALSE(registry.retry(id));
    CHECK(registry.cancel(id));
    CHECK(cancelled == 1);
    CHECK_FALSE(registry.cancel(id));
    CHECK(registry.snapshots().empty());
    registry.setObserver([&](auto change, const auto &value) {
        if (change == pci::OperationChange::Inserted)
            CHECK(registry.remove(value.token.id));
    });
    registry.insert(row(id), {});
    CHECK(registry.snapshots().empty());
}

TEST_CASE(
    "operation lifecycle retries invalidate old stop state and reject overflow",
    "[operations][lifecycle][unit]")
{
    pci::OperationLifecycle lifecycle;
    const auto oldStop = lifecycle.stop.get_token();
    lifecycle.stop.request_stop();
    CHECK(lifecycle.finish());
    CHECK_FALSE(lifecycle.finish());
    lifecycle.restart();
    CHECK(oldStop.stop_requested());
    CHECK_FALSE(lifecycle.stop.stop_requested());
    CHECK(lifecycle.generation == pci::AttemptGeneration{2});
    CHECK_FALSE(lifecycle.finished());
    lifecycle.generation =
        pci::AttemptGeneration{std::numeric_limits<std::uint64_t>::max()};
    CHECK_THROWS_AS(lifecycle.restart(), std::overflow_error);
    CHECK(lifecycle.generation.value() ==
          std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE(
    "operation observer failures cannot reverse committed lifecycle state",
    "[operations][registry][unit]")
{
    pci::OperationRegistry registry;
    registry.setObserver([](auto, const auto &) {
        throw std::runtime_error("presentation failed");
    });
    const pci::LoadJobId id{1};
    CHECK_NOTHROW(registry.insert(row(id), {}));
    CHECK(registry.complete(row(id, pci::OperationState::Succeeded)));
    CHECK(registry.find(id)->state == pci::OperationState::Succeeded);
    CHECK_FALSE(registry.complete(row(id, pci::OperationState::Failed)));
    CHECK_NOTHROW(registry.clear());
}

TEST_CASE(
    "reserved operation terminals survive progress bursts and rejected wakes",
    "[operations][delivery][unit]")
{
    struct Owner {
        int progress = 0;
        int terminal = 0;
    } owner;
    auto executor =
        std::make_shared<pci::test::DeterministicCompletionExecutor>();
    auto target =
        std::make_shared<pci::OperationTarget<Owner>>(&owner, executor);
    auto completion = target->reserveCompletion();
    executor->rejectWakeRequests();
    for (int i = 0; i < 10000; ++i) {
        CHECK(pci::postOperationProgress(
            target,
            {pci::LoadJobId{1}, pci::AttemptGeneration{1}},
            [i](Owner *value) {
                value->progress = i;
            }));
    }
    CHECK(pci::postToOperation(completion, [](Owner *value) {
        ++value->terminal;
    }));
    CHECK_FALSE(pci::postToOperation(completion, [](Owner *value) {
        ++value->terminal;
    }));
    target->drain();
    CHECK(owner.progress == 9999);
    CHECK(owner.terminal == 1);
    CHECK_FALSE(pci::postToOperation(completion, [](Owner *value) {
        ++value->terminal;
    }));
}
TEST_CASE(
    "reserved operation results release after owner shutdown without pumping",
    "[operations][delivery][shutdown][unit]")
{
    int owner = 0;
    auto executor =
        std::make_shared<pci::test::DeterministicCompletionExecutor>();
    auto target = std::make_shared<pci::OperationTarget<int>>(&owner, executor);
    auto completion = target->reserveCompletion();
    auto payload = std::make_shared<int>(42);
    std::weak_ptr<int> weakPayload = payload;
    CHECK(pci::postToOperation(completion,
                               [payload = std::move(payload)](int *value) {
                                   *value = *payload;
                               }));
    target->invalidate();
    CHECK(weakPayload.expired());
    CHECK_FALSE(pci::postToOperation(completion, [](int *value) {
        ++*value;
    }));
    CHECK(owner == 0);
}
