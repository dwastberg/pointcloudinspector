#pragma once

#include <pci/desktop/operations/LoadJobRow.h>

#include <QAbstractListModel>

#include <optional>
#include <unordered_map>
#include <vector>

namespace pci {

class TaskListModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        JobKeyRole = Qt::UserRole,
        DetailRole,
        CompletionRole,
        TerminalRole,
        CanCancelRole,
        CanRetryRole,
        CanPrioritizeRole,
        CanDismissRole
    };
    Q_ENUM(Role)

    explicit TaskListModel(QObject *parent = nullptr);

    [[nodiscard]] int
    rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void updateRow(LoadJobRow row);
    void removeRow(LoadJobKey key);
    void setRows(std::vector<LoadJobRow> rows);
    [[nodiscard]] std::optional<int> rowForKey(LoadJobKey key) const;
    [[nodiscard]] const LoadJobRow *rowAt(int row) const noexcept;

private:
    friend struct TaskListModelTestAccess;
    mutable std::size_t lookupInspections_ = 0;
    void reconcile(std::vector<LoadJobRow> next);

    struct KeyHash {
        std::size_t operator()(LoadJobKey key) const noexcept
        {
            return std::hash<LoadJobId>{}(key.id) ^
                   (static_cast<std::size_t>(key.kind) << 1);
        }
    };
    struct KeyEqual {
        std::size_t *inspections = nullptr;
        bool operator()(LoadJobKey left, LoadJobKey right) const noexcept
        {
            if (inspections)
                ++*inspections;
            return left == right;
        }
    };
    std::unordered_map<LoadJobKey, int, KeyHash, KeyEqual> rowIndices_{
        0, KeyHash{}, KeyEqual{&lookupInspections_}};
    void reindex(int first = 0);
    std::vector<LoadJobRow> rows_;
};

} // namespace pci
