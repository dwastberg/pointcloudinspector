#pragma once

#include <pci/foundation/Generation.h>
#include <pci/foundation/LayerIdentity.h>
#include <pci/operations/LoadJobMechanics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pci {

enum class OperationState {
    Queued,
    Running,
    AwaitingInput,
    Committing,
    Succeeded,
    Failed,
    Cancelled
};
[[nodiscard]] constexpr bool terminal(OperationState state) noexcept
{
    return state == OperationState::Succeeded ||
           state == OperationState::Failed ||
           state == OperationState::Cancelled;
}

struct OperationToken {
    LoadJobId id;
    AttemptGeneration attempt{1};
    bool operator==(const OperationToken &) const = default;
};

struct OperationSnapshot {
    OperationToken token;
    LoadJobKind kind = LoadJobKind::PointCloud;
    OperationState state = OperationState::Queued;
    std::optional<SceneLayerId> target;
    std::string title;
    std::string detail;
    double completion = 0;
    LoadJobCapabilities capabilities;
};

struct OperationControls {
    std::function<void()> cancel;
    std::function<void()> retry;
    std::function<void()> prioritize;
    std::function<void()> dismiss;
};

enum class OperationChange {
    Inserted,
    Updated,
    Removed
};

// One owner-thread registry. Concrete workflows retain their transactions and
// retry policies; the registry only routes their registered capabilities.
class OperationRegistry final {
public:
    using Observer =
        std::function<void(OperationChange, const OperationSnapshot &)>;
    void insert(OperationSnapshot snapshot, OperationControls controls);
    [[nodiscard]] bool beginAttempt(OperationToken token);
    [[nodiscard]] bool update(OperationSnapshot snapshot);
    [[nodiscard]] bool complete(OperationSnapshot snapshot);
    [[nodiscard]] bool remove(LoadJobId id);
    void clear();
    [[nodiscard]] std::optional<OperationSnapshot> find(LoadJobId id) const;
    [[nodiscard]] std::vector<OperationSnapshot> snapshots() const;
    void setObserver(Observer observer);
    bool cancel(LoadJobId id);
    bool retry(LoadJobId id);
    bool prioritize(LoadJobId id);
    bool dismiss(LoadJobId id);

private:
    struct Entry {
        OperationSnapshot snapshot;
        OperationControls controls;
    };
    void assertOwner() const;
    void notify(OperationChange change, OperationSnapshot snapshot) const;
    bool invoke(LoadJobId id,
                bool LoadJobCapabilities::*capability,
                std::function<void()> OperationControls::*control);
    const std::thread::id owner_ = std::this_thread::get_id();
    std::unordered_map<LoadJobId, Entry> entries_;
    Observer observer_;
};

} // namespace pci
