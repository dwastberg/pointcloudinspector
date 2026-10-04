#pragma once

#include <pci/desktop/ui/SceneLayerMetaType.h>
#include <pci/document/SceneDocumentSnapshot.h>

#include <QAbstractListModel>

#include <optional>
#include <unordered_map>
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
        SourceToolTipRole,
        RasterColorsRole,
        RasterColorSourceRole
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

signals:
    void visibilityEditRequested(pci::SceneLayerId layerId, bool visible);

private:
    struct Row {
        SceneLayerId id;
        SceneLayerKind kind = SceneLayerKind::None;
        QString name;
        QString summary;
        QString toolTip;
        QString rasterColorSource;
        bool visible = true;
        bool rasterColors = false;
        bool warning = false;
        bool allowShowAnyway = false;

        bool operator==(const Row &) const = default;
    };

    [[nodiscard]] static std::vector<Row>
    project(const SceneDocumentSnapshot &snapshot);
    void reconcile(std::vector<Row> next);

    std::unordered_map<SceneLayerId, int> rowIndices_;
    void reindex(int first = 0);
    std::vector<Row> rows_;
};

} // namespace pci
