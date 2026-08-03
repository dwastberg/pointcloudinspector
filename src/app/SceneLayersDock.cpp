#include "app/SceneLayersDock.h"

#include "app/SceneLayerListModel.h"

#include <QAbstractItemView>
#include <QAction>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <stdexcept>

namespace pci {
namespace {

class LayerItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        const QString count =
            index.data(SceneLayerListModel::SummaryRole).toString();
        if (count.isEmpty()) {
            return;
        }
        QStyleOptionViewItem styled = option;
        initStyleOption(&styled, index);
        painter->save();
        painter->setPen(
            styled.palette.color(QPalette::Disabled, QPalette::Text));
        painter->drawText(styled.rect.adjusted(0, 0, -8, 0),
                          Qt::AlignRight | Qt::AlignVCenter,
                          count);
        painter->restore();
    }
};

} // namespace

SceneLayersDock::SceneLayersDock(QWidget *parent)
    : QDockWidget(QStringLiteral("Scene"), parent)
{
    setObjectName(QStringLiteral("pointCloudLayerPanel"));
    setFeatures(QDockWidget::DockWidgetClosable |
                QDockWidget::DockWidgetMovable |
                QDockWidget::DockWidgetFloatable);
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setMinimumWidth(220);

    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    auto *actions = new QHBoxLayout();
    actions->setContentsMargins(0, 0, 0, 0);
    auto *add = new QPushButton(QStringLiteral("Add points…"), content);
    add->setObjectName(QStringLiteral("addPointCloudLayerButton"));
    add->setToolTip(
        QStringLiteral("Add point-cloud files to the current scene"));
    auto *showAll = new QPushButton(QStringLiteral("Show all"), content);
    showAll->setObjectName(QStringLiteral("showAllLayersButton"));
    showAll->setToolTip(
        QStringLiteral("Make every layer in the scene visible"));
    actions->addWidget(add);
    actions->addStretch(1);
    actions->addWidget(showAll);
    layout->addLayout(actions);

    list_ = new QListView(content);
    list_->setObjectName(QStringLiteral("pointCloudLayerList"));
    model_ = new SceneLayerListModel(list_);
    list_->setModel(model_);
    list_->setItemDelegate(new LayerItemDelegate(list_));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setAlternatingRowColors(true);
    layout->addWidget(list_, 1);
    auto *empty = new QLabel(QStringLiteral("No layers in this scene.\nOpen or "
                                            "drop point-cloud files to begin."),
                             content);
    empty->setObjectName(QStringLiteral("pointCloudLayerEmptyLabel"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setEnabled(false);
    layout->addWidget(empty);
    setWidget(content);

    statisticsAction_ =
        new QAction(QStringLiteral("Point Cloud &Statistics…"), this);
    statisticsAction_->setObjectName(
        QStringLiteral("pointCloudStatisticsAction"));
    statisticsAction_->setToolTip(QStringLiteral(
        "Show coordinate, density, attribute, and outlier statistics "
        "for the selected layer"));
    statisticsAction_->setEnabled(false);

    connect(
        add, &QPushButton::clicked, this, &SceneLayersDock::addLayerRequested);
    connect(showAll,
            &QPushButton::clicked,
            this,
            &SceneLayersDock::showAllRequested);
    connect(statisticsAction_,
            &QAction::triggered,
            this,
            &SceneLayersDock::showCurrentLayerStatistics);
    connect(model_,
            &SceneLayerListModel::visibilityEditRequested,
            this,
            &SceneLayersDock::visibilityToggled);
    connect(list_->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this,
            [this](const QModelIndex &, const QModelIndex &) {
                publishSelection();
            });
    connect(list_,
            &QListView::customContextMenuRequested,
            this,
            &SceneLayersDock::showContextMenu);
}

void SceneLayersDock::setDocumentSnapshot(SceneDocumentSnapshotPtr snapshot)
{
    if (!snapshot) {
        throw std::invalid_argument("document snapshot must not be null");
    }
    const std::optional<SceneLayerId> selected = currentLayerId();
    model_->setSnapshot(std::move(snapshot));
    if (selected) {
        if (const auto row = model_->rowForId(*selected)) {
            list_->setCurrentIndex(model_->index(*row));
        }
    }
    if (!list_->currentIndex().isValid() && model_->rowCount() > 0) {
        list_->setCurrentIndex(model_->index(0));
    }
    if (QLabel *empty =
            findChild<QLabel *>(QStringLiteral("pointCloudLayerEmptyLabel"))) {
        empty->setVisible(model_->rowCount() == 0);
    }
    publishSelection();
}

std::optional<SceneLayerId> SceneLayersDock::currentLayerId() const
{
    const QModelIndex current = list_->currentIndex();
    if (!current.isValid()) {
        return std::nullopt;
    }
    return current.data(SceneLayerListModel::LayerIdRole).value<SceneLayerId>();
}

QAction *SceneLayersDock::statisticsAction() const noexcept
{
    return statisticsAction_;
}

void SceneLayersDock::selectAllLayers()
{
    list_->selectAll();
}

void SceneLayersDock::fitCurrentLayer()
{
    if (const auto id = currentLayerId()) {
        emit fitRequested(*id);
    }
}

void SceneLayersDock::isolateCurrentLayer()
{
    if (const auto id = currentLayerId()) {
        emit isolateRequested(*id);
    }
}

void SceneLayersDock::removeCurrentLayer()
{
    if (const auto id = currentLayerId()) {
        emit removeRequested(*id);
    }
}

void SceneLayersDock::showCurrentLayerStatistics()
{
    if (const auto id = currentLayerId(); id && currentLayerIsPoint()) {
        emit statisticsRequested(*id);
    }
}

void SceneLayersDock::showAll()
{
    emit showAllRequested();
}

void SceneLayersDock::publishSelection()
{
    const SceneLayerId id = currentLayerId().value_or(SceneLayerId{});
    statisticsAction_->setEnabled(id.value() != 0 && currentLayerIsPoint());
    emit selectionChanged(id);
}

void SceneLayersDock::showContextMenu(const QPoint &position)
{
    const QModelIndex item = list_->indexAt(position);
    if (!item.isValid()) {
        return;
    }
    list_->setCurrentIndex(item);
    const SceneLayerId id =
        item.data(SceneLayerListModel::LayerIdRole).value<SceneLayerId>();
    QMenu menu(this);
    QAction *fit = menu.addAction(QStringLiteral("Fit view to layer"));
    QAction *isolate = menu.addAction(QStringLiteral("Isolate"));
    QAction *showAll = menu.addAction(QStringLiteral("Show all layers"));
    QAction *showAnyway = nullptr;
    if (item.data(SceneLayerListModel::ShowAnywayRole).toBool()) {
        showAnyway = menu.addAction(QStringLiteral("Show anyway"));
    }
    menu.addSeparator();
    menu.addAction(statisticsAction_);
    menu.addSeparator();
    QAction *remove = menu.addAction(QStringLiteral("Remove layer"));
    QAction *chosen = menu.exec(list_->viewport()->mapToGlobal(position));
    if (chosen == fit) {
        emit fitRequested(id);
    } else if (chosen == isolate) {
        emit isolateRequested(id);
    } else if (chosen == showAll) {
        emit showAllRequested();
    } else if (chosen == showAnyway) {
        emit showVectorAnywayRequested(id);
    } else if (chosen == remove) {
        emit removeRequested(id);
    }
}

bool SceneLayersDock::currentLayerIsPoint() const
{
    return list_->currentIndex()
               .data(SceneLayerListModel::LayerKindRole)
               .toInt() == static_cast<int>(SceneLayerKind::PointCloud);
}

} // namespace pci
