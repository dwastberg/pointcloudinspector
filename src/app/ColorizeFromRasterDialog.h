#pragma once

#include "foundation/SpatialReferenceComparator.h"
#include "scene/SceneDocumentSnapshot.h"

#include <QDialog>

#include <cstdint>
#include <optional>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QPushButton;

namespace pci {

class ColorizeFromRasterDialog final : public QDialog {
public:
    ColorizeFromRasterDialog(
        PointCloudLayer target,
        SceneDocumentSnapshotPtr document,
        std::shared_ptr<const SpatialReferenceComparator> comparator,
        std::uint64_t availablePointMemoryBytes,
        QWidget *parent = nullptr);

    [[nodiscard]] std::optional<SceneLayerId> selectedRasterLayerId() const;

private:
    void updateSelection();

    PointCloudLayer target_;
    SceneDocumentSnapshotPtr document_;
    std::shared_ptr<const SpatialReferenceComparator> comparator_;
    std::uint64_t availablePointMemoryBytes_ = 0;
    QComboBox *rasters_ = nullptr;
    QLabel *summary_ = nullptr;
    QLabel *warning_ = nullptr;
    QLabel *estimate_ = nullptr;
    QDialogButtonBox *buttons_ = nullptr;
    QPushButton *apply_ = nullptr;
};

} // namespace pci
