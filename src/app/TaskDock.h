#pragma once

#include "import/LoadJobRow.h"

#include <QDockWidget>

#include <cstdint>
#include <vector>

class QListView;
class QPoint;
class QWidget;

namespace pci {

class TaskListModel;

enum class LoadJobAction : std::uint8_t {
    Cancel,
    Retry,
    Prioritize,
    Dismiss
};

class TaskDock final : public QDockWidget {
    Q_OBJECT

public:
    explicit TaskDock(QWidget *parent = nullptr);

    void setRows(std::vector<LoadJobRow> rows);

signals:
    void jobActionRequested(pci::LoadJobKey key, pci::LoadJobAction action);

private:
    [[nodiscard]] QWidget *createRowWidget(const LoadJobRow &row);
    void updateRowWidget(QWidget &widget, const LoadJobRow &row);
    void synchronizeRowWidgets();
    void showContextMenu(const QPoint &position);

    QListView *list_ = nullptr;
    TaskListModel *model_ = nullptr;
};

} // namespace pci

Q_DECLARE_METATYPE(pci::LoadJobAction)
