#pragma once

#include <filesystem>
#include <pci/operations/LoadJobKey.h>
#include <pci/operations/OperationRegistry.h>
#include <string>
#include <vector>

namespace pci {
struct OperationRow {
    LoadJobKey key;
    std::string title;
    std::string detail;
    double completion = 0;
    bool terminal = false;
    LoadJobCapabilities capabilities;
    AttemptGeneration attempt{1};
    OperationState state = OperationState::Queued;
    std::optional<SceneLayerId> target;
};

template <typename Phase>
[[nodiscard]] constexpr OperationState operationState(Phase phase) noexcept
{
    if (phase == Phase::Queued)
        return OperationState::Queued;
    if (phase == Phase::Ready)
        return OperationState::Succeeded;
    if (phase == Phase::Failed)
        return OperationState::Failed;
    if (phase == Phase::Cancelled)
        return OperationState::Cancelled;
    if constexpr (requires { Phase::AwaitingChoice; }) {
        if (phase == Phase::AwaitingChoice)
            return OperationState::AwaitingInput;
    }
    if constexpr (requires { Phase::Committing; }) {
        if (phase == Phase::Committing)
            return OperationState::Committing;
    }
    return OperationState::Running;
}

[[nodiscard]] inline std::string
operationPath(const std::filesystem::path &path)
{
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}
[[nodiscard]] inline std::string
operationPathName(const std::filesystem::path &path)
{
    return operationPath(path.filename().empty() ? path : path.filename());
}
} // namespace pci
