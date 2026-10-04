#pragma once

#include <pci/operations/OperationLifecycle.h>
#include <pci/operations/OperationTarget.h>
#include <pci/operations/StorageMaintenance.h>

namespace pci {
struct StorageMaintenanceUpdate {
    std::uint64_t visitedEntries = 0;
    std::shared_ptr<const StorageMaintenanceResult> result;
};
class StorageMaintenanceSubscription final {
public:
    using Observer = std::function<void(const StorageMaintenanceUpdate &)>;
    StorageMaintenanceSubscription() = default;
    ~StorageMaintenanceSubscription()
    {
        reset();
    }
    StorageMaintenanceSubscription(StorageMaintenanceSubscription &&) noexcept =
        default;
    StorageMaintenanceSubscription &
    operator=(StorageMaintenanceSubscription &&other) noexcept;
    StorageMaintenanceSubscription(const StorageMaintenanceSubscription &) =
        delete;
    StorageMaintenanceSubscription &
    operator=(const StorageMaintenanceSubscription &) = delete;
    void reset() noexcept;

private:
    friend class StorageMaintenanceOperation;
    struct State {
        Observer observer;
        std::function<void()> cancel;
    };
    std::shared_ptr<State> state_;
};

// Session-owned. One request at a time, on the shared background scheduler;
// observers may detach without joining workers or retaining widgets.
class StorageMaintenanceOperation final {
public:
    StorageMaintenanceOperation(
        std::shared_ptr<const StorageMaintenance> provider,
        TaskScheduler &scheduler,
        std::shared_ptr<CompletionExecutor> executor);
    ~StorageMaintenanceOperation();
    [[nodiscard]] StorageMaintenanceSubscription
    start(StorageMaintenanceAction action,
          StorageMaintenanceSubscription::Observer observer);
    void cancel();
    void recoverDelivery();

private:
    void finish(StorageMaintenanceResult result);
    void progress(std::uint64_t visited);
    std::shared_ptr<const StorageMaintenance> provider_;
    TaskScheduler &scheduler_;
    std::shared_ptr<OperationTarget<StorageMaintenanceOperation>> delivery_;
    OperationLifecycle lifecycle_;
    std::weak_ptr<StorageMaintenanceSubscription::State> observer_;
    bool busy_ = false;
};
} // namespace pci
