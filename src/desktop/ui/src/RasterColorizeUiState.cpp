#include <pci/desktop/ui/RasterColorizeUiState.h>

namespace pci {

RasterColorizeUiState
rasterColorizeUiState(const PointCloudLayerSnapshot *point,
                      const std::size_t rasterLayerCount,
                      const bool active,
                      const bool committing)
{
    RasterColorizeUiState result;
    const bool baked = point != nullptr && point->rasterColors.has_value();
    result.revertVisible = baked;
    result.revertEnabled = baked && !committing;

    if (committing) {
        result.startText = QStringLiteral("Applying raster colors…");
    } else if (active) {
        result.startText = QStringLiteral("Colorizing…");
    } else if (baked) {
        result.startText = QStringLiteral("Recolor from raster…");
    } else {
        result.startText = QStringLiteral("Colorize from raster…");
    }

    if (!point) {
        result.startToolTip = QStringLiteral("Select one point-cloud layer");
    } else if (rasterLayerCount == 0) {
        result.startToolTip = QStringLiteral("Import a raster layer first");
    } else if (active) {
        result.startToolTip =
            committing ? QStringLiteral("Raster colors are being applied")
                       : QStringLiteral("Colorization is already running for "
                                        "this layer");
    } else {
        switch (point->colorizeAvailability) {
        case PointColorizeAvailability::Ready:
            result.startEnabled = true;
            result.startToolTip =
                baked ? QStringLiteral("Replace the current baked colors "
                                       "from a raster layer")
                      : QStringLiteral("Transfer displayed raster colors "
                                       "to this point cloud");
            break;
        case PointColorizeAvailability::Loading:
            result.startToolTip =
                QStringLiteral("Wait for this point-cloud import to finish");
            break;
        case PointColorizeAvailability::Unsupported:
            result.startToolTip = QStringLiteral(
                "This source is read directly and has no local page cache to "
                "colorize");
            break;
        }
    }

    if (committing) {
        result.revertToolTip =
            QStringLiteral("Wait for the active colorization to commit");
    } else if (active) {
        result.revertToolTip = QStringLiteral(
            "Cancel the active colorization and restore exact source colors");
    } else {
        result.revertToolTip =
            QStringLiteral("Restore the point cloud's exact source colors");
    }
    return result;
}

} // namespace pci
