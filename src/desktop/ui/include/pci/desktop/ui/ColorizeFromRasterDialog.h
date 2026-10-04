#pragma once

#include <pci/document/SceneDocumentSnapshot.h>
#include <pci/foundation/SpatialReferenceComparator.h>
#include <pci/operations/RasterPointColorize.h>

#include <QDialog>

#include <cstdint>
#include <functional>
#include <optional>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QPushButton;

namespace pci {

using RasterColorizeEstimateFunction =
    std::function<RasterColorizeResourceEstimate(SceneLayerId)>;

class ColorizeFromRasterDialog final : public QDialog {
public:
    ColorizeFromRasterDialog(
        PointCloudLayerSnapshot target,
        SceneDocumentSnapshotPtr document,
        RasterColorizeEstimateFunction estimate,
        std::shared_ptr<const SpatialReferenceComparator> comparator,
        std::uint64_t availablePointMemoryBytes,
        QWidget *parent = nullptr);

    [[nodiscard]] std::optional<SceneLayerId> selectedRasterLayerId() const;

private:
    void updateSelection();

    PointCloudLayerSnapshot target_;
    SceneDocumentSnapshotPtr document_;
    RasterColorizeEstimateFunction estimateFunction_;
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
