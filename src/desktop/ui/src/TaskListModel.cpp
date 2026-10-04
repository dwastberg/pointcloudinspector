#include <pci/desktop/ui/TaskListModel.h>

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
    const auto found = rowIndices_.find(key);
    return found == rowIndices_.end() ? std::nullopt
                                      : std::optional<int>{found->second};
}

const LoadJobRow *TaskListModel::rowAt(const int row) const noexcept
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return nullptr;
    }
    return &rows_[static_cast<std::size_t>(row)];
}

void TaskListModel::removeRow(LoadJobKey key)
{
    const auto row = rowForKey(key);
    if (!row)
        return;
    beginRemoveRows({}, *row, *row);
    rowIndices_.erase(key);
    rows_.erase(rows_.begin() + *row);
    reindex(*row);
    endRemoveRows();
}

void TaskListModel::updateRow(LoadJobRow row)
{
#ifndef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    if (row.key.kind == LoadJobKind::PointCloud && row.terminal &&
        !row.capabilities.canRetry) {
        removeRow(row.key);
        return;
    }
#endif
    if (const auto index = rowForKey(row.key)) {
        auto &current = rows_[static_cast<std::size_t>(*index)];
        if (!sameRow(current, row)) {
            current = std::move(row);
            emit dataChanged(this->index(*index), this->index(*index));
        }
        return;
    }
    const auto position = std::ranges::lower_bound(
        rows_, row, [](const auto &left, const auto &right) {
            return std::pair{left.key.kind, left.key.id} <
                   std::pair{right.key.kind, right.key.id};
        });
    const int index = static_cast<int>(position - rows_.begin());
    beginInsertRows({}, index, index);
    rows_.insert(position, std::move(row));
    reindex(index);
    endInsertRows();
}

void TaskListModel::reindex(int first)
{
    for (int row = first; row < static_cast<int>(rows_.size()); ++row)
        rowIndices_[rows_[static_cast<std::size_t>(row)].key] = row;
}

void TaskListModel::reconcile(std::vector<LoadJobRow> next)
{
    decltype(rowIndices_) desiredIndices;
    desiredIndices.reserve(next.size());
    for (std::size_t i = 0; i < next.size(); ++i)
        desiredIndices.emplace(next[i].key, static_cast<int>(i));
    rowIndices_.reserve(next.size());
    for (int row = static_cast<int>(rows_.size()) - 1; row >= 0; --row) {
        if (!desiredIndices.contains(
                rows_[static_cast<std::size_t>(row)].key)) {
            beginRemoveRows({}, row, row);
            rowIndices_.erase(rows_[static_cast<std::size_t>(row)].key);
            rows_.erase(rows_.begin() + row);
            reindex(row);
            endRemoveRows();
        }
    }
    for (int target = 0; target < static_cast<int>(next.size()); ++target) {
        const LoadJobRow &desired = next[static_cast<std::size_t>(target)];
        if (target >= static_cast<int>(rows_.size())) {
            beginInsertRows({}, target, target);
            rows_.insert(rows_.begin() + target, desired);
            reindex(target);
            endInsertRows();
        } else if (rows_[static_cast<std::size_t>(target)].key != desired.key) {
            const auto found = rowIndices_.find(desired.key);
            if (found == rowIndices_.end()) {
                beginInsertRows({}, target, target);
                rows_.insert(rows_.begin() + target, desired);
                reindex(target);
                endInsertRows();
            } else {
                const int source = found->second;
                beginMoveRows({}, source, source, {}, target);
                LoadJobRow moved =
                    std::move(rows_[static_cast<std::size_t>(source)]);
                rows_.erase(rows_.begin() + source);
                rows_.insert(rows_.begin() + target, std::move(moved));
                reindex(target);
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
