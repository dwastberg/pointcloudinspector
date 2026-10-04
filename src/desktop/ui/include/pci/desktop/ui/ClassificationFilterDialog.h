#pragma once

#include <pci/color/PointColorMapCatalog.h>
#include <pci/pointcloud/PointClassificationFilter.h>

#include <QDialog>

class QLabel;
class QListWidget;

namespace pci {

class ClassificationFilterDialog final : public QDialog {
public:
    explicit ClassificationFilterDialog(
        PointColorMapCatalogSnapshotPtr colorMaps,
        PointClassificationFilter filter,
        PointClassificationFilter availableClassifications,
        const QString &layerName,
        QWidget *parent = nullptr);
    explicit ClassificationFilterDialog(
        PointClassificationFilter filter,
        PointClassificationFilter availableClassifications,
        const QString &layerName,
        QWidget *parent = nullptr);

    [[nodiscard]] PointClassificationFilter filter() const;
    [[nodiscard]] bool applyToAllLayers() const noexcept;

private:
    void setAllChecked(bool checked);
    void updateSummary();

    QListWidget *classificationList_ = nullptr;
    QLabel *summaryLabel_ = nullptr;
    PointClassificationFilter baseFilter_;
    PointColorMapCatalogSnapshotPtr colorMaps_;
    bool applyToAllLayers_ = false;
};

} // namespace pci
