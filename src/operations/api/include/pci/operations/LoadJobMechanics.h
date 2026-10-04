#pragma once

#include <pci/operations/LoadJobKey.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pci {

class LoadJobIdSequence final {
public:
    explicit constexpr LoadJobIdSequence(
        const std::uint64_t lastIssued = 0) noexcept
        : lastIssued_(lastIssued)
    {
    }

    [[nodiscard]] LoadJobId next(const std::string_view exhaustedMessage)
    {
        if (lastIssued_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error(std::string(exhaustedMessage));
        }
        return LoadJobId{++lastIssued_};
    }

private:
    std::uint64_t lastIssued_ = 0;
};

[[nodiscard]] constexpr LoadJobCapabilities
activeLoadJobCapabilities(const bool canPrioritize = false) noexcept
{
    return {
        .canCancel = true,
        .canRetry = false,
        .canPrioritize = canPrioritize,
        .canDismiss = false,
    };
}

[[nodiscard]] constexpr LoadJobCapabilities
terminalLoadJobCapabilities(const bool canRetry) noexcept
{
    return {
        .canCancel = false,
        .canRetry = canRetry,
        .canPrioritize = false,
        .canDismiss = true,
    };
}

template <typename State, typename Records>
[[nodiscard]] std::vector<State> sortedJobStates(const Records &records)
{
    std::vector<State> result;
    result.reserve(records.size());
    for (const auto &[id, state] : records) {
        static_cast<void>(id);
        result.push_back(state);
    }
    std::ranges::sort(result, {}, &State::jobId);
    return result;
}

} // namespace pci
