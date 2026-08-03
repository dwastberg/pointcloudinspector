#pragma once

#include <QAbstractEventDispatcher>
#include <QMetaObject>
#include <QObject>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pci {

// QPointer is not safe to inspect concurrently with QObject destruction. This
// token lets workers queue work without touching QObject state. The controller
// pointer is read and invalidated only on its event-loop thread.
template <typename Controller> class QueuedControllerTarget final {
public:
    using Callback = std::function<void(Controller *)>;

    explicit QueuedControllerTarget(Controller *controller)
        : controller_(controller)
        , dispatcher_(QAbstractEventDispatcher::instance(controller->thread()))
    {
        if (dispatcher_.load(std::memory_order_relaxed) == nullptr) {
            throw std::logic_error(
                "an async controller requires an event dispatcher");
        }
    }

    QueuedControllerTarget(const QueuedControllerTarget &) = delete;
    QueuedControllerTarget &operator=(const QueuedControllerTarget &) = delete;

    void invalidate() noexcept
    {
        controller_ = nullptr;
        dispatcher_.store(nullptr, std::memory_order_release);
    }

    [[nodiscard]] QObject *dispatcher() const noexcept
    {
        return dispatcher_.load(std::memory_order_acquire);
    }

    [[nodiscard]] Controller *controller() const noexcept
    {
        return controller_;
    }

    template <typename Completion> void enqueue(Completion completion)
    {
        std::scoped_lock lock(callbackMutex_);
        callbacks_.emplace_back(std::move(completion));
    }

    [[nodiscard]] std::deque<Callback> takeCallbacks()
    {
        std::scoped_lock lock(callbackMutex_);
        return std::exchange(callbacks_, {});
    }

private:
    Controller *controller_;
    std::atomic<QObject *> dispatcher_;
    std::mutex callbackMutex_;
    std::deque<Callback> callbacks_;
};

template <typename Controller> struct QueuedControllerQueue final {
    inline static std::mutex mutex;
    inline static std::vector<
        std::shared_ptr<QueuedControllerTarget<Controller>>>
        targets;
};

template <typename Controller> void drainQueuedControllers()
{
    std::vector<std::shared_ptr<QueuedControllerTarget<Controller>>> targets;
    {
        std::scoped_lock lock(QueuedControllerQueue<Controller>::mutex);
        targets = std::exchange(QueuedControllerQueue<Controller>::targets, {});
    }
    for (const auto &target : targets) {
        auto callbacks = target->takeCallbacks();
        for (auto &callback : callbacks) {
            if (Controller *controller = target->controller()) {
                callback(controller);
            }
        }
    }
}

template <typename Controller, typename Completion>
void postToObject(
    const std::shared_ptr<QueuedControllerTarget<Controller>> &target,
    Completion completion)
{
    QObject *dispatcher = target->dispatcher();
    if (dispatcher == nullptr) {
        return;
    }
    target->enqueue(std::move(completion));
    {
        std::scoped_lock lock(QueuedControllerQueue<Controller>::mutex);
        QueuedControllerQueue<Controller>::targets.push_back(target);
    }
    QMetaObject::invokeMethod(
        dispatcher,
        [] {
            drainQueuedControllers<Controller>();
        },
        Qt::QueuedConnection);
}

} // namespace pci
