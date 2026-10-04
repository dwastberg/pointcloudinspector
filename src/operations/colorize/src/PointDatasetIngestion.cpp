#include <pci/operations/PointDatasetIngestion.h>

#include <stdexcept>
#include <utility>

namespace pci {

PointDatasetIngestion::PointDatasetIngestion(const std::uint64_t byteLimit,
                                             std::function<void()> wake)
    : byteLimit_(byteLimit)
    , wake_(std::move(wake))
{
    if (!byteLimit_ || !wake_) {
        throw std::invalid_argument(
            "ingestion requires a byte allowance and wake");
    }
}

bool PointDatasetIngestion::push(PointDatasetEvent event, std::stop_token stop)
{
    const auto bytes = event.retainedBytes();
    if (bytes > byteLimit_) {
        throw PointCloudImportError(
            "point event exceeds its ingestion allowance");
    }
    {
        std::unique_lock lock(mutex_);
        if (!space_.wait(lock,
                         stop,
                         [&] {
                             return closed_ || finished_ ||
                                    (count_ + (inFlight_ ? 1U : 0U) <
                                         eventCapacity &&
                                     bytes <= byteLimit_ - bytes_);
                         }) ||
            closed_ || finished_ || stop.stop_requested()) {
            return false;
        }
        events_[(head_ + count_) % eventCapacity] = std::move(event);
        ++count_;
        bytes_ += bytes;
    }
    wake_();
    return true;
}

void PointDatasetIngestion::progress(PointCloudImportProgress value)
{
    {
        std::scoped_lock lock(mutex_);
        if (closed_ || finished_) {
            return;
        }
        progress_ = value;
    }
    wake_();
}

void PointDatasetIngestion::finish(Outcome outcome)
{
    {
        std::scoped_lock lock(mutex_);
        if (closed_ || finished_) {
            return;
        }
        finished_ = true;
        if (!outcome) {
            for (auto &event : events_) {
                event.reset();
            }
            count_ = 0;
            bytes_ = inFlightBytes_;
            progress_.reset();
        }
        outcome_ = std::move(outcome);
    }
    space_.notify_all();
    wake_();
}

void PointDatasetIngestion::close() noexcept
{
    {
        std::scoped_lock lock(mutex_);
        closed_ = true;
        for (auto &event : events_) {
            event.reset();
        }
        count_ = 0;
        bytes_ = inFlightBytes_;
        outcome_.reset();
        progress_.reset();
    }
    space_.notify_all();
}

std::optional<PointDatasetEvent> PointDatasetIngestion::takeEvent()
{
    std::scoped_lock lock(mutex_);
    if (closed_ || !count_ || inFlight_) {
        return {};
    }
    auto event = std::exchange(events_[head_], {});
    head_ = (head_ + 1) % eventCapacity;
    --count_;
    inFlight_ = true;
    inFlightBytes_ = event->retainedBytes();
    return event;
}

void PointDatasetIngestion::releaseEvent() noexcept
{
    {
        std::scoped_lock lock(mutex_);
        bytes_ -= inFlightBytes_;
        inFlightBytes_ = 0;
        inFlight_ = false;
    }
    space_.notify_all();
}

std::optional<PointCloudImportProgress> PointDatasetIngestion::takeProgress()
{
    std::scoped_lock lock(mutex_);
    return std::exchange(progress_, {});
}

std::optional<PointDatasetIngestion::Outcome>
PointDatasetIngestion::takeOutcome()
{
    std::scoped_lock lock(mutex_);
    if (count_ || inFlight_) {
        return {};
    }
    return std::exchange(outcome_, {});
}

bool PointDatasetIngestion::pending() const
{
    std::scoped_lock lock(mutex_);
    return count_ || progress_ || outcome_;
}

std::uint64_t PointDatasetIngestion::retainedBytes() const
{
    std::scoped_lock lock(mutex_);
    return bytes_;
}

} // namespace pci
