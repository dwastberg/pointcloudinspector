#include "support/DeterministicCompletionExecutor.h"
#include <pci/desktop/dispatch/QtCompletionExecutor.h>
#include <pci/operations/OperationTarget.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QEvent>
#include <QObject>

#include <memory>
#include <thread>

namespace {

struct Controller final {
    int deliveries = 0;
};

class QtController final : public QObject {
public:
    int deliveries = 0;
};

} // namespace

TEST_CASE(
    "queued controller target delegates deferred delivery and invalidation",
    "[qt][async][dispatch][invalidation]")
{
    auto executor =
        std::make_shared<pci::test::DeterministicCompletionExecutor>();
    Controller controller;
    auto target = std::make_shared<pci::OperationTarget<Controller>>(
        &controller, executor);

    REQUIRE(pci::postToOperation(target, [](Controller *owner) {
        ++owner->deliveries;
    }));
    CHECK(controller.deliveries == 0);
    REQUIRE(executor->runNext());
    CHECK(controller.deliveries == 1);

    auto state = std::make_shared<int>(7);
    std::weak_ptr<int> lifetime = state;
    REQUIRE(pci::postToOperation(target, [state](Controller *owner) {
        owner->deliveries += *state;
    }));
    state.reset();
    target->invalidate();
    CHECK(lifetime.expired());
    REQUIRE(executor->runNext());
    CHECK(controller.deliveries == 1);
    CHECK_FALSE(pci::postToOperation(target, [](Controller *owner) {
        ++owner->deliveries;
    }));
}

TEST_CASE("Qt controller completion remains deferred on its owner thread",
          "[qt][async][dispatch]")
{
    QtController controller;
    auto target = std::make_shared<pci::OperationTarget<QtController>>(
        &controller, pci::makeQtCompletionExecutor(&controller));

    REQUIRE(pci::postToOperation(target, [](QtController *owner) {
        ++owner->deliveries;
    }));
    CHECK(controller.deliveries == 0);

    QCoreApplication::processEvents();
    CHECK(controller.deliveries == 1);
    target->invalidate();
}

TEST_CASE("controller shutdown releases queued state before event-loop drain",
          "[qt][async][dispatch][shutdown]")
{
    QtController controller;
    auto target = std::make_shared<pci::OperationTarget<QtController>>(
        &controller, pci::makeQtCompletionExecutor(&controller));
    auto state = std::make_shared<int>(11);
    std::weak_ptr<int> lifetime = state;

    REQUIRE(pci::postToOperation(target, [state](QtController *owner) {
        owner->deliveries += *state;
    }));
    state.reset();
    REQUIRE_FALSE(lifetime.expired());

    target->invalidate();
    CHECK(lifetime.expired());
    QCoreApplication::processEvents();
    CHECK(controller.deliveries == 0);
}

TEST_CASE("last worker releases Qt completions without an owner queue drain",
          "[qt][async][dispatch][shutdown]")
{
    QtController controller;
    auto executor = pci::makeQtCompletionExecutor(&controller);
    auto state = std::make_shared<int>(11);
    std::weak_ptr<int> lifetime = state;
    bool accepted = false;
    {
        std::jthread worker([executor = std::move(executor),
                             state = std::move(state),
                             &accepted,
                             &controller]() mutable {
            accepted = executor->post(pci::CompletionExecutor::Completion{
                [state = std::move(state), &controller] {
                    controller.deliveries += *state;
                }});
            executor.reset();
        });
    }
    REQUIRE(accepted);
    CHECK(lifetime.expired());
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(controller.deliveries == 0);
}
