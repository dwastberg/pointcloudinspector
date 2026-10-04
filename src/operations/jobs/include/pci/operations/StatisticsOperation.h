#pragma once

#include <pci/foundation/JobResult.h>
#include <pci/operations/OperationLifecycle.h>
#include <pci/operations/OperationRegistration.h>
#include <pci/operations/OperationTarget.h>
#include <pci/operations/PointCloudStatistics.h>
#include <pci/pointcloud/PointCloudMetadata.h>
#include <pci/pointcloud/PointIdentity.h>

namespace pci {

struct StatisticsBindingToken {
    SessionGeneration session;
    PointCloudLayerId layer;
    PointCloudSourceId source;
    BindingGeneration binding;
    bool operator==(const StatisticsBindingToken &) const = default;
};

struct StatisticsUpdate {
    std::uint64_t processed = 0;
    std::uint64_t total = 0;
    std::shared_ptr<const JobResult<PointCloudStatistics>> result;
};

// Owned by the observer, not by a widget-aware operation. Closing detaches
// first, then requests cancellation through a weak owner-thread endpoint.
class StatisticsSubscription final {
public:
    using Observer = std::function<void(const StatisticsUpdate &)>;
    StatisticsSubscription() = default;
    ~StatisticsSubscription()
    {
        reset();
    }
    StatisticsSubscription(const StatisticsSubscription &) = delete;
    StatisticsSubscription &operator=(const StatisticsSubscription &) = delete;
    StatisticsSubscription(StatisticsSubscription &&other) noexcept = default;
    StatisticsSubscription &operator=(StatisticsSubscription &&other) noexcept
    {
        if (this != &other) {
            reset();
            state_ = std::move(other.state_);
            id_ = other.id_;
        }
        return *this;
    }
    void reset() noexcept;
    [[nodiscard]] LoadJobId id() const noexcept
    {
        return id_;
    }

private:
    friend class StatisticsOperation;
    struct State {
        Observer observer;
        std::function<void()> cancel;
    };
    std::shared_ptr<State> state_;
    LoadJobId id_;
};

class StatisticsOperation final {
public:
    using Validator = std::function<bool(const StatisticsBindingToken &)>;
    StatisticsOperation(
        std::shared_ptr<const PointCloudStatisticsProvider> provider,
        TaskScheduler &scheduler,
        std::shared_ptr<CompletionExecutor> executor,
        std::shared_ptr<LoadJobIdSequence> ids,
        OperationRegistry &registry,
        Validator valid);
    ~StatisticsOperation();
    [[nodiscard]] StatisticsSubscription
    start(PointCloudMetadata metadata,
          StatisticsBindingToken binding,
          StatisticsSubscription::Observer observer);
    void cancel(LoadJobId id);
    void cancelLayer(PointCloudLayerId id);
    void cancelAll();
    void cancelInvalidTargets();
    bool dismiss(LoadJobId id);
    void recoverDelivery();

private:
    struct Job {
        OperationLifecycle lifecycle;
        StatisticsBindingToken binding;
        OperationRow row;
        StatisticsUpdate update;
        std::weak_ptr<StatisticsSubscription::State> observer;
    };
    void progress(OperationToken token,
                  std::uint64_t processed,
                  std::uint64_t total);
    void finish(OperationToken token, JobResult<PointCloudStatistics> result);
    void publish(LoadJobId id);
    std::shared_ptr<const PointCloudStatisticsProvider> provider_;
    TaskScheduler &scheduler_;
    std::shared_ptr<LoadJobIdSequence> ids_;
    std::shared_ptr<OperationTarget<StatisticsOperation>> delivery_;
    OperationRegistration registration_;
    Validator valid_;
    std::unordered_map<LoadJobId, Job> jobs_;
};
} // namespace pci
