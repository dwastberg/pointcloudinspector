#pragma once

#include <pci/foundation/JobResult.h>
#include <pci/operations/PointCloudLoader.h>

#include <array>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>

namespace pci {

// One admitted producer, one owner-thread consumer. A taken event remains
// charged until releaseEvent(), including while its installation is prepared.
class PointDatasetIngestion final {
public:
    using Outcome = JobResult<PreparedPointDatasetPtr>;
    static constexpr std::size_t eventCapacity = 64;
    PointDatasetIngestion(std::uint64_t byteLimit, std::function<void()> wake);
    bool push(PointDatasetEvent event, std::stop_token stop);
    void progress(PointCloudImportProgress value);
    void finish(Outcome outcome);
    void close() noexcept;
    [[nodiscard]] std::optional<PointDatasetEvent> takeEvent();
    void releaseEvent() noexcept;
    [[nodiscard]] std::optional<PointCloudImportProgress> takeProgress();
    [[nodiscard]] std::optional<Outcome> takeOutcome();
    [[nodiscard]] bool pending() const;
    [[nodiscard]] std::uint64_t retainedBytes() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable_any space_;
    std::array<std::optional<PointDatasetEvent>, eventCapacity> events_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::uint64_t bytes_ = 0;
    std::uint64_t inFlightBytes_ = 0;
    bool inFlight_ = false;
    bool closed_ = false;
    bool finished_ = false;
    std::uint64_t byteLimit_;
    std::function<void()> wake_;
    std::optional<PointCloudImportProgress> progress_;
    std::optional<Outcome> outcome_;
};

} // namespace pci
