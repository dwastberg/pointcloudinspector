#include "app/TaskDock.h"

#include "app/TaskListModel.h"

#include <QAction>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace pci {
TaskDock::TaskDock(QWidget *parent)
    : QDockWidget(QStringLiteral("Tasks"), parent)
{
    setObjectName(QStringLiteral("pointCloudTasksPanel"));
    setFeatures(QDockWidget::DockWidgetClosable |
                QDockWidget::DockWidgetMovable |
                QDockWidget::DockWidgetFloatable);
    setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    auto *content = new QWidget(this);
    content->setObjectName(QStringLiteral("pointCloudLoadActivity"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(8, 8, 8, 8);
    list_ = new QListView(content);
    list_->setObjectName(QStringLiteral("pointCloudLoadStateList"));
    model_ = new TaskListModel(list_);
    list_->setModel(model_);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(list_);
    setWidget(content);
    content->hide();
    hide();
    connect(list_,
            &QListView::customContextMenuRequested,
            this,
            &TaskDock::showContextMenu);
}

void TaskDock::setRows(std::vector<LoadJobRow> rows)
{
    model_->setRows(std::move(rows));
    synchronizeRowWidgets();
    const bool hasTasks = model_->rowCount() > 0;
    widget()->setVisible(hasTasks);
    setVisible(hasTasks);
}

QWidget *TaskDock::createRowWidget(const LoadJobRow &row)
{
    auto *task = new QWidget(list_->viewport());
    task->setObjectName(QStringLiteral("loadTaskRow"));
    auto *taskLayout = new QHBoxLayout(task);
    taskLayout->setContentsMargins(8, 5, 6, 5);
    taskLayout->setSpacing(10);
    auto *textColumn = new QWidget(task);
    auto *textLayout = new QVBoxLayout(textColumn);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(3);
    auto *title = new QLabel(textColumn);
    title->setObjectName(QStringLiteral("loadTaskTitle"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    auto *detail = new QLabel(textColumn);
    detail->setObjectName(QStringLiteral("loadTaskDetail"));
    detail->setWordWrap(true);
    auto *progress = new QProgressBar(textColumn);
    progress->setObjectName(QStringLiteral("loadTaskProgressBar"));
    progress->setRange(0, 1000);
    progress->setTextVisible(false);
    progress->setAccessibleName(QStringLiteral("Task progress"));
    textLayout->addWidget(title);
    textLayout->addWidget(detail);
    textLayout->addWidget(progress);
    taskLayout->addWidget(textColumn, 1);

    const auto addButton = [task, taskLayout](const QString &text,
                                              const QString &objectName,
                                              const QString &toolTip) {
        auto *button = new QPushButton(text, task);
        button->setObjectName(objectName);
        button->setToolTip(toolTip);
        button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        taskLayout->addWidget(button);
        return button;
    };
    const auto connectAction =
        [this, key = row.key](QPushButton *button, const LoadJobAction action) {
            connect(button, &QPushButton::clicked, this, [this, key, action] {
                emit jobActionRequested(key, action);
            });
        };
    QPushButton *prioritize =
        addButton(QStringLiteral("Prioritize"),
                  QStringLiteral("prioritizeLoadTaskButton"),
                  QStringLiteral("Move this load ahead of queued work"));
    connectAction(prioritize, LoadJobAction::Prioritize);
    QPushButton *cancel = addButton(QStringLiteral("Cancel"),
                                    QStringLiteral("cancelLoadTaskButton"),
                                    QStringLiteral("Cancel this load"));
    connectAction(cancel, LoadJobAction::Cancel);
    QPushButton *retry = addButton(
        QStringLiteral("Retry"),
        QStringLiteral("retryLoadTaskButton"),
        row.key.kind == LoadJobKind::Vector
            ? QStringLiteral("Retry only failed or not-yet-loaded sublayers")
            : QStringLiteral("Retry this load"));
    retry->setProperty("primary", true);
    connectAction(retry, LoadJobAction::Retry);
    QPushButton *dismiss =
        addButton(QStringLiteral("Dismiss"),
                  QStringLiteral("dismissLoadTaskButton"),
                  QStringLiteral("Remove this completed task from the list"));
    connectAction(dismiss, LoadJobAction::Dismiss);
    updateRowWidget(*task, row);
    return task;
}

void TaskDock::updateRowWidget(QWidget &widget, const LoadJobRow &row)
{
    widget.setAccessibleName(
        QStringLiteral("%1, %2").arg(row.title, row.detail));
    widget.findChild<QLabel *>(QStringLiteral("loadTaskTitle"))
        ->setText(row.title);
    widget.findChild<QLabel *>(QStringLiteral("loadTaskDetail"))
        ->setText(row.detail);
    auto *progress =
        widget.findChild<QProgressBar *>(QStringLiteral("loadTaskProgressBar"));
    progress->setValue(qRound(std::clamp(row.completion, 0.0, 1.0) * 1000.0));
    progress->setAccessibleDescription(
        QStringLiteral("%1 percent")
            .arg(qRound(std::clamp(row.completion, 0.0, 1.0) * 100.0)));
    progress->setProperty("state",
                          row.capabilities.canRetry ? "attention" : QVariant{});
    widget.findChild<QPushButton *>(QStringLiteral("prioritizeLoadTaskButton"))
        ->setVisible(row.capabilities.canPrioritize);
    widget.findChild<QPushButton *>(QStringLiteral("cancelLoadTaskButton"))
        ->setVisible(row.capabilities.canCancel);
    widget.findChild<QPushButton *>(QStringLiteral("retryLoadTaskButton"))
        ->setVisible(row.capabilities.canRetry);
    widget.findChild<QPushButton *>(QStringLiteral("dismissLoadTaskButton"))
        ->setVisible(row.capabilities.canDismiss);
}

void TaskDock::synchronizeRowWidgets()
{
    for (int rowIndex = 0; rowIndex < model_->rowCount(); ++rowIndex) {
        const QModelIndex index = model_->index(rowIndex);
        QWidget *widget = list_->indexWidget(index);
        const LoadJobRow *row = model_->rowAt(rowIndex);
        if (!row) {
            continue;
        }
        if (!widget) {
            widget = createRowWidget(*row);
            list_->setIndexWidget(index, widget);
        } else {
            updateRowWidget(*widget, *row);
        }
    }
}

void TaskDock::showContextMenu(const QPoint &position)
{
    const QModelIndex item = list_->indexAt(position);
    if (!item.isValid()) {
        return;
    }
    list_->setCurrentIndex(item);
    const LoadJobRow *row = model_->rowAt(item.row());
    if (!row) {
        return;
    }
    const LoadJobKey key = row->key;
    QMenu menu(this);
    const auto addAction = [&menu](const bool enabled, const QString &label) {
        return enabled ? menu.addAction(label) : nullptr;
    };
    QAction *prioritize = addAction(row->capabilities.canPrioritize,
                                    QStringLiteral("Prioritize"));
    QAction *cancel =
        addAction(row->capabilities.canCancel, QStringLiteral("Cancel"));
    QAction *retry =
        addAction(row->capabilities.canRetry, QStringLiteral("Retry"));
    QAction *dismiss =
        addAction(row->capabilities.canDismiss, QStringLiteral("Dismiss"));
    if (menu.isEmpty()) {
        return;
    }
    QAction *chosen = menu.exec(list_->viewport()->mapToGlobal(position));
    if (chosen == prioritize) {
        emit jobActionRequested(key, LoadJobAction::Prioritize);
    } else if (chosen == cancel) {
        emit jobActionRequested(key, LoadJobAction::Cancel);
    } else if (chosen == retry) {
        emit jobActionRequested(key, LoadJobAction::Retry);
    } else if (chosen == dismiss) {
        emit jobActionRequested(key, LoadJobAction::Dismiss);
    }
}

} // namespace pci
