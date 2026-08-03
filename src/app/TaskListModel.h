#pragma once

#include "import/LoadJobRow.h"

#include <QAbstractListModel>

#include <optional>
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

    void setRows(std::vector<LoadJobRow> rows);
    [[nodiscard]] std::optional<int> rowForKey(LoadJobKey key) const;
    [[nodiscard]] const LoadJobRow *rowAt(int row) const noexcept;

private:
    void reconcile(std::vector<LoadJobRow> next);

    std::vector<LoadJobRow> rows_;
};

} // namespace pci
