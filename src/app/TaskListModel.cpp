#include "app/TaskListModel.h"

#include <QSize>
#include <QVariant>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <utility>

namespace pci {
namespace {

bool sameRow(const LoadJobRow &left, const LoadJobRow &right)
{
    return left.key == right.key && left.title == right.title &&
           left.detail == right.detail && left.completion == right.completion &&
           left.terminal == right.terminal &&
           left.capabilities.canCancel == right.capabilities.canCancel &&
           left.capabilities.canRetry == right.capabilities.canRetry &&
           left.capabilities.canPrioritize ==
               right.capabilities.canPrioritize &&
           left.capabilities.canDismiss == right.capabilities.canDismiss;
}

std::vector<LoadJobRow> visibleRows(std::vector<LoadJobRow> rows)
{
#ifndef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    std::erase_if(rows, [](const LoadJobRow &row) {
        return row.key.kind == LoadJobKind::PointCloud && row.terminal &&
               !row.capabilities.canRetry;
    });
#endif
    return rows;
}

} // namespace

TaskListModel::TaskListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int TaskListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant TaskListModel::data(const QModelIndex &index, const int role) const
{
    const LoadJobRow *row = rowAt(index.row());
    if (!index.isValid() || !row) {
        return {};
    }
    switch (role) {
    case Qt::DisplayRole:
        return row->title;
    case Qt::ToolTipRole:
    case DetailRole:
        return row->detail;
    case Qt::SizeHintRole:
        return QSize(0, 72);
    case JobKeyRole:
        return QVariant::fromValue(row->key);
    case CompletionRole:
        return row->completion;
    case TerminalRole:
        return row->terminal;
    case CanCancelRole:
        return row->capabilities.canCancel;
    case CanRetryRole:
        return row->capabilities.canRetry;
    case CanPrioritizeRole:
        return row->capabilities.canPrioritize;
    case CanDismissRole:
        return row->capabilities.canDismiss;
    default:
        return {};
    }
}

QHash<int, QByteArray> TaskListModel::roleNames() const
{
    auto roles = QAbstractListModel::roleNames();
    roles.insert(JobKeyRole, "jobKey");
    roles.insert(DetailRole, "detail");
    roles.insert(CompletionRole, "completion");
    roles.insert(TerminalRole, "terminal");
    roles.insert(CanCancelRole, "canCancel");
    roles.insert(CanRetryRole, "canRetry");
    roles.insert(CanPrioritizeRole, "canPrioritize");
    roles.insert(CanDismissRole, "canDismiss");
    return roles;
}

void TaskListModel::setRows(std::vector<LoadJobRow> rows)
{
    reconcile(visibleRows(std::move(rows)));
}

std::optional<int> TaskListModel::rowForKey(const LoadJobKey key) const
{
    const auto found = std::ranges::find(rows_, key, &LoadJobRow::key);
    if (found == rows_.end()) {
        return std::nullopt;
    }
    return static_cast<int>(std::distance(rows_.begin(), found));
}

const LoadJobRow *TaskListModel::rowAt(const int row) const noexcept
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return nullptr;
    }
    return &rows_[static_cast<std::size_t>(row)];
}

void TaskListModel::reconcile(std::vector<LoadJobRow> next)
{
    for (int row = static_cast<int>(rows_.size()) - 1; row >= 0; --row) {
        if (std::ranges::none_of(next,
                                 [key = rows_[row].key](const auto &entry) {
                                     return entry.key == key;
                                 })) {
            beginRemoveRows({}, row, row);
            rows_.erase(rows_.begin() + row);
            endRemoveRows();
        }
    }
    for (int target = 0; target < static_cast<int>(next.size()); ++target) {
        const LoadJobRow &desired = next[static_cast<std::size_t>(target)];
        if (target >= static_cast<int>(rows_.size())) {
            beginInsertRows({}, target, target);
            rows_.insert(rows_.begin() + target, desired);
            endInsertRows();
        } else if (rows_[static_cast<std::size_t>(target)].key != desired.key) {
            const auto found = std::ranges::find(rows_.begin() + target + 1,
                                                 rows_.end(),
                                                 desired.key,
                                                 &LoadJobRow::key);
            if (found == rows_.end()) {
                beginInsertRows({}, target, target);
                rows_.insert(rows_.begin() + target, desired);
                endInsertRows();
            } else {
                const int source =
                    static_cast<int>(std::distance(rows_.begin(), found));
                beginMoveRows({}, source, source, {}, target);
                LoadJobRow moved =
                    std::move(rows_[static_cast<std::size_t>(source)]);
                rows_.erase(rows_.begin() + source);
                rows_.insert(rows_.begin() + target, std::move(moved));
                endMoveRows();
            }
        }
        LoadJobRow &current = rows_[static_cast<std::size_t>(target)];
        if (!sameRow(current, desired)) {
            current = desired;
            emit dataChanged(index(target), index(target));
        }
    }
}

} // namespace pci
