#include <pci/desktop/ui/SceneLayersDock.h>

#include "ToolbarIcons.h"
#include <pci/desktop/ui/RasterColorizeUiState.h>
#include <pci/desktop/ui/SceneLayerListModel.h>

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
        const QString count =
            index.data(SceneLayerListModel::SummaryRole).toString();
        QStyleOptionViewItem styled = option;
        initStyleOption(&styled, index);
        const bool rasterColors =
            index.data(SceneLayerListModel::RasterColorsRole).toBool();
        QFontMetrics metrics(styled.font);
        const int countWidth =
            count.isEmpty() ? 0 : metrics.horizontalAdvance(count) + 12;
        constexpr int badgeWidth = 48;
        constexpr int itemGap = 5;
        const int badgeSpace = rasterColors ? badgeWidth + itemGap : 0;
        styled.rect.adjust(0, 0, -(countWidth + badgeSpace), 0);
        QStyledItemDelegate::paint(painter, styled, index);

        painter->save();
        if (rasterColors) {
            QRect badgeRect(option.rect.right() - countWidth - badgeWidth,
                            option.rect.center().y() - 8,
                            badgeWidth,
                            16);
            QColor badge = styled.palette.color(QPalette::Highlight);
            badge.setAlpha(210);
            painter->setPen(Qt::NoPen);
            painter->setBrush(badge);
            painter->drawRoundedRect(badgeRect, 4.0, 4.0);
            QFont badgeFont = styled.font;
            badgeFont.setPixelSize(9);
            badgeFont.setBold(true);
            painter->setFont(badgeFont);
            painter->setPen(styled.palette.color(QPalette::HighlightedText));
            painter->drawText(
                badgeRect, Qt::AlignCenter, QStringLiteral("Raster"));
        }
        if (!count.isEmpty()) {
            painter->setFont(styled.font);
            painter->setPen(
                styled.palette.color(QPalette::Disabled, QPalette::Text));
            const QRect countRect(option.rect.right() - countWidth,
                                  option.rect.top(),
                                  countWidth - 6,
                                  option.rect.height());
            painter->drawText(
                countRect, Qt::AlignRight | Qt::AlignVCenter, count);
        }
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
    colorizeAction_ =
        new QAction(QStringLiteral("Colorize from &Raster…"), this);
    colorizeAction_->setObjectName(QStringLiteral("colorizeFromRasterAction"));
    colorizeAction_->setIcon(
        toolbarIcon(ToolbarIcon::ColorizeRaster, palette()));
    revertColorsAction_ =
        new QAction(QStringLiteral("Revert to Source &Colors"), this);
    revertColorsAction_->setObjectName(
        QStringLiteral("revertRasterColorsAction"));
    revertColorsAction_->setIcon(
        toolbarIcon(ToolbarIcon::RevertColors, palette()));
    colorizeAction_->setEnabled(false);
    revertColorsAction_->setEnabled(false);

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
    connect(colorizeAction_, &QAction::triggered, this, [this] {
        if (const auto id = currentLayerId()) {
            emit colorizeRequested(*id);
        }
    });
    connect(revertColorsAction_, &QAction::triggered, this, [this] {
        if (const auto id = currentLayerId()) {
            emit revertColorsRequested(*id);
        }
    });
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
    snapshot_ = snapshot;
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

QAction *SceneLayersDock::colorizeAction() const noexcept
{
    return colorizeAction_;
}

QAction *SceneLayersDock::revertColorsAction() const noexcept
{
    return revertColorsAction_;
}

void SceneLayersDock::setColorizeJobState(const bool active,
                                          const bool committing)
{
    if (colorizeJobActive_ == active && colorizeJobCommitting_ == committing) {
        return;
    }
    colorizeJobActive_ = active;
    colorizeJobCommitting_ = committing;
    publishSelection();
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
    const auto point = snapshot_ && id.value() != 0
                           ? snapshot_->layer(id)
                           : std::optional<PointCloudLayerSnapshot>{};
    const RasterColorizeUiState state =
        rasterColorizeUiState(point ? &*point : nullptr,
                              snapshot_ ? snapshot_->rasterLayerCount() : 0,
                              colorizeJobActive_,
                              colorizeJobCommitting_);
    colorizeAction_->setText(state.startText);
    colorizeAction_->setToolTip(state.startToolTip);
    colorizeAction_->setEnabled(state.startEnabled);
    revertColorsAction_->setVisible(state.revertVisible);
    revertColorsAction_->setEnabled(state.revertEnabled);
    revertColorsAction_->setToolTip(state.revertToolTip);
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
    menu.addAction(colorizeAction_);
    menu.addAction(revertColorsAction_);
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
