#pragma once

#include "app/SceneLayerMetaType.h"
#include "scene/SceneDocumentSnapshot.h"

#include <QAbstractListModel>

#include <optional>
#include <vector>

namespace pci {

class SceneLayerListModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        LayerIdRole = Qt::UserRole,
        NameRole,
        VisibilityRole,
        SummaryRole,
        LayerKindRole,
        WarningRole,
        ShowAnywayRole,
        SourceToolTipRole
    };
    Q_ENUM(Role)

    explicit SceneLayerListModel(QObject *parent = nullptr);

    [[nodiscard]] int
    rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool
    setData(const QModelIndex &index, const QVariant &value, int role) override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setSnapshot(SceneDocumentSnapshotPtr snapshot);
    [[nodiscard]] std::optional<int> rowForId(SceneLayerId id) const;
    [[nodiscard]] std::optional<SceneLayerId> idForRow(int row) const;

signals:
    void visibilityEditRequested(pci::SceneLayerId layerId, bool visible);

private:
    struct Row {
        SceneLayerId id;
        SceneLayerKind kind = SceneLayerKind::None;
        QString name;
        QString summary;
        QString toolTip;
        bool visible = true;
        bool warning = false;
        bool allowShowAnyway = false;

        bool operator==(const Row &) const = default;
    };

    [[nodiscard]] static std::vector<Row>
    project(const SceneDocumentSnapshot &snapshot);
    void reconcile(std::vector<Row> next);

    std::vector<Row> rows_;
};

} // namespace pci
