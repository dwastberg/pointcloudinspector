#include <pci/operations/StorageMaintenanceOperation.h>

namespace pci {
void StorageMaintenanceSubscription::reset() noexcept
{
    auto state = std::move(state_);
    if (!state)
        return;
    state->observer = {};
    if (state->cancel) {
        try {
            state->cancel();
        } catch (...) {
        }
    }
}
StorageMaintenanceSubscription &StorageMaintenanceSubscription::operator=(
    StorageMaintenanceSubscription &&other) noexcept
{
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
    }
    return *this;
}

StorageMaintenanceOperation::StorageMaintenanceOperation(
    std::shared_ptr<const StorageMaintenance> provider,
    TaskScheduler &scheduler,
    std::shared_ptr<CompletionExecutor> executor)
    : provider_(std::move(provider))
    , scheduler_(scheduler)
    , delivery_(std::make_shared<OperationTarget<StorageMaintenanceOperation>>(
          this, std::move(executor)))
{
    if (!provider_)
        throw std::invalid_argument("Storage maintenance requires a provider");
}
StorageMaintenanceOperation::~StorageMaintenanceOperation()
{
    delivery_->invalidate();
    observer_.reset();
    cancel();
}
StorageMaintenanceSubscription StorageMaintenanceOperation::start(
    StorageMaintenanceAction action,
    StorageMaintenanceSubscription::Observer observer)
{
    if (busy_)
        throw std::runtime_error("Previous storage request is still stopping. "
                                 "Refresh to try again.");
    StorageMaintenanceSubscription subscription;
    subscription.state_ =
        std::make_shared<StorageMaintenanceSubscription::State>();
    subscription.state_->observer = std::move(observer);
    const auto weak = std::weak_ptr(delivery_);
    const auto generation = nextGeneration(lifecycle_.generation);
    subscription.state_->cancel = [weak, generation] {
        if (const auto target = weak.lock())
            if (auto *owner = target->controller();
                owner && owner->lifecycle_.generation == generation)
                owner->cancel();
    };
    const auto reserved = delivery_->reserveCompletion();
    lifecycle_.restart();
    busy_ = true;
    observer_ = subscription.state_;
    const auto stop = lifecycle_.stop.get_token();
    auto complete = [reserved](StorageMaintenanceResult result) {
        static_cast<void>(
            postToOperation(reserved,
                            [result = std::move(result)](
                                StorageMaintenanceOperation *owner) mutable {
                                owner->finish(std::move(result));
                            }));
    };
    try {
        lifecycle_.task = scheduler_.submit(
            TaskPriority::Background,
            64 * 1024,
            [provider = provider_,
             delivery = delivery_,
             stop,
             action,
             complete,
             generation] {
                StorageMaintenanceResult result;
                try {
                    result = provider->run(
                        action,
                        stop,
                        [delivery, generation](std::uint64_t visited) {
                            static_cast<void>(postOperationProgress(
                                delivery,
                                {LoadJobId{1}, generation},
                                [visited](StorageMaintenanceOperation *owner) {
                                    owner->progress(visited);
                                }));
                        });
                } catch (const std::exception &error) {
                    result.errorCount = 1;
                    result.errors.push_back(error.what());
                } catch (...) {
                    result.errorCount = 1;
                    result.errors.push_back(
                        "Unknown storage maintenance failure");
                }
                result.cancelled = stop.stop_requested();
                complete(std::move(result));
            },
            [complete] {
                StorageMaintenanceResult result;
                result.cancelled = true;
                complete(std::move(result));
            });
    } catch (const std::exception &error) {
        StorageMaintenanceResult result;
        result.errorCount = 1;
        result.errors.push_back(error.what());
        complete(std::move(result));
    }
    return subscription;
}
void StorageMaintenanceOperation::cancel()
{
    if (busy_)
        lifecycle_.cancel(scheduler_);
}
void StorageMaintenanceOperation::recoverDelivery()
{
    delivery_->recover();
}
void StorageMaintenanceOperation::progress(std::uint64_t visited)
{
    if (busy_)
        if (const auto state = observer_.lock(); state && state->observer)
            state->observer({visited, {}});
}
void StorageMaintenanceOperation::finish(StorageMaintenanceResult result)
{
    if (!lifecycle_.finish())
        return;
    busy_ = false;
    const auto state = observer_.lock();
    observer_.reset();
    if (state && state->observer)
        state->observer(
            {0, std::make_shared<StorageMaintenanceResult>(std::move(result))});
}
} // namespace pci
