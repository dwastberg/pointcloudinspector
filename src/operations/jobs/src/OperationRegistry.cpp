#include <pci/operations/OperationRegistry.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace pci {
void OperationRegistry::assertOwner() const
{
    if (owner_ != std::this_thread::get_id())
        throw std::logic_error(
            "operation registry accessed outside its owner thread");
}
void OperationRegistry::notify(OperationChange change,
                               OperationSnapshot snapshot) const
{
    // Observer failures cannot undo an accepted lifecycle transition.
    try {
        // Copy before delivery: an observer may remove the row or replace
        // itself.
        const auto observer = observer_;
        if (observer)
            observer(change, snapshot);
    } catch (...) {
        // Observers are presentation-only; the authoritative snapshot remains
        // queryable.
    }
}
void OperationRegistry::setObserver(Observer observer)
{
    assertOwner();
    observer_ = std::move(observer);
}
void OperationRegistry::insert(OperationSnapshot snapshot,
                               OperationControls controls)
{
    assertOwner();
    if (!snapshot.token.id.value() || !snapshot.token.attempt.value())
        throw std::invalid_argument("operation identity must be nonzero");
    const auto id = snapshot.token.id;
    if (entries_.contains(id))
        throw std::logic_error("duplicate operation id");
    auto [entry, inserted] =
        entries_.emplace(id, Entry{std::move(snapshot), std::move(controls)});
    static_cast<void>(inserted);
    notify(OperationChange::Inserted, entry->second.snapshot);
}
bool OperationRegistry::beginAttempt(OperationToken token)
{
    assertOwner();
    auto found = entries_.find(token.id);
    if (found == entries_.end() || !terminal(found->second.snapshot.state) ||
        token.attempt != nextGeneration(found->second.snapshot.token.attempt))
        return false;
    found->second.snapshot.token = token;
    found->second.snapshot.state = OperationState::Queued;
    found->second.snapshot.completion = 0;
    found->second.snapshot.capabilities = activeLoadJobCapabilities();
    notify(OperationChange::Updated, found->second.snapshot);
    return true;
}
bool OperationRegistry::update(OperationSnapshot snapshot)
{
    assertOwner();
    auto found = entries_.find(snapshot.token.id);
    if (found == entries_.end() ||
        found->second.snapshot.token != snapshot.token ||
        terminal(found->second.snapshot.state) || terminal(snapshot.state) ||
        found->second.snapshot.kind != snapshot.kind)
        return false;
    found->second.snapshot = std::move(snapshot);
    notify(OperationChange::Updated, found->second.snapshot);
    return true;
}
bool OperationRegistry::complete(OperationSnapshot snapshot)
{
    assertOwner();
    auto found = entries_.find(snapshot.token.id);
    if (found == entries_.end() ||
        found->second.snapshot.token != snapshot.token ||
        terminal(found->second.snapshot.state) || !terminal(snapshot.state) ||
        found->second.snapshot.kind != snapshot.kind)
        return false;
    found->second.snapshot = std::move(snapshot);
    notify(OperationChange::Updated, found->second.snapshot);
    return true;
}
bool OperationRegistry::remove(LoadJobId id)
{
    assertOwner();
    const auto found = entries_.find(id);
    if (found == entries_.end())
        return false;
    auto snapshot = std::move(found->second.snapshot);
    entries_.erase(found);
    notify(OperationChange::Removed, snapshot);
    return true;
}
void OperationRegistry::clear()
{
    assertOwner();
    // Detach before callbacks, so observer reentrancy cannot invalidate
    // traversal.
    auto removed = std::move(entries_);
    entries_.clear();
    for (const auto &[id, entry] : removed) {
        static_cast<void>(id);
        notify(OperationChange::Removed, entry.snapshot);
    }
}
std::optional<OperationSnapshot> OperationRegistry::find(LoadJobId id) const
{
    assertOwner();
    const auto found = entries_.find(id);
    return found == entries_.end() ? std::nullopt
                                   : std::optional(found->second.snapshot);
}
std::vector<OperationSnapshot> OperationRegistry::snapshots() const
{
    assertOwner();
    std::vector<OperationSnapshot> result;
    result.reserve(entries_.size());
    for (const auto &[id, entry] : entries_) {
        static_cast<void>(id);
        result.push_back(entry.snapshot);
    }
    // Preserve the established task groups; global IDs order rows within each.
    const auto order = [](LoadJobKind kind) {
        return kind == LoadJobKind::RasterElevation ? 3
               : kind == LoadJobKind::Colorize      ? 4
                                                    : static_cast<int>(kind);
    };
    std::ranges::sort(result, [&](const auto &a, const auto &b) {
        return order(a.kind) == order(b.kind) ? a.token.id < b.token.id
                                              : order(a.kind) < order(b.kind);
    });
    return result;
}
bool OperationRegistry::invoke(
    LoadJobId id,
    bool LoadJobCapabilities::*capability,
    std::function<void()> OperationControls::*control)
{
    assertOwner();
    const auto found = entries_.find(id);
    if (found == entries_.end() ||
        !(found->second.snapshot.capabilities.*capability))
        return false;
    auto callback = found->second.controls.*control;
    if (!callback)
        return false;
    callback();
    return true;
}
bool OperationRegistry::cancel(LoadJobId id)
{
    return invoke(
        id, &LoadJobCapabilities::canCancel, &OperationControls::cancel);
}
bool OperationRegistry::retry(LoadJobId id)
{
    return invoke(
        id, &LoadJobCapabilities::canRetry, &OperationControls::retry);
}
bool OperationRegistry::prioritize(LoadJobId id)
{
    return invoke(id,
                  &LoadJobCapabilities::canPrioritize,
                  &OperationControls::prioritize);
}
bool OperationRegistry::dismiss(LoadJobId id)
{
    return invoke(
        id, &LoadJobCapabilities::canDismiss, &OperationControls::dismiss);
}
} // namespace pci
