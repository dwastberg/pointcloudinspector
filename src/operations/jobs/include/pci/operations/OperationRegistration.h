#pragma once

#include <pci/operations/OperationRow.h>
#include <unordered_set>

namespace pci {

class OperationRegistration final {
public:
    using Controls = std::function<OperationControls(LoadJobId)>;
    void bind(OperationRegistry &registry, Controls controls)
    {
        registry_ = &registry;
        controls_ = std::move(controls);
    }
    void publish(const OperationRow &row)
    {
        if (!registry_)
            return;
        OperationSnapshot snapshot{
            .token = {row.key.id, row.attempt},
            .kind = row.key.kind,
            .state = row.state,
            .target = row.target,
            .title = row.title,
            .detail = row.detail,
            .completion = row.completion,
            .capabilities = row.capabilities,
        };
        const auto current = registry_->find(row.key.id);
        if (!current) {
            ids_.insert(row.key.id);
            registry_->insert(std::move(snapshot), controls_(row.key.id));
            return;
        }
        if (current->token.attempt != row.attempt &&
            !registry_->beginAttempt(snapshot.token))
            return;
        if (terminal(snapshot.state))
            static_cast<void>(registry_->complete(std::move(snapshot)));
        else
            static_cast<void>(registry_->update(std::move(snapshot)));
    }
    void remove(LoadJobId id)
    {
        ids_.erase(id);
        if (registry_)
            static_cast<void>(registry_->remove(id));
    }
    void detach()
    {
        auto *registry = std::exchange(registry_, nullptr);
        controls_ = {};
        auto ids = std::move(ids_);
        if (registry)
            for (auto id : ids)
                static_cast<void>(registry->remove(id));
    }

private:
    OperationRegistry *registry_ = nullptr;
    Controls controls_;
    std::unordered_set<LoadJobId> ids_;
};

} // namespace pci
