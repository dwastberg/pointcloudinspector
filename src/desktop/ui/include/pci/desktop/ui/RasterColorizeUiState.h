#pragma once

#include <pci/document/SceneSnapshotLayer.h>

#include <QString>

#include <cstddef>

namespace pci {

struct RasterColorizeUiState {
    bool startEnabled = false;
    bool revertVisible = false;
    bool revertEnabled = false;
    QString startText;
    QString startToolTip;
    QString revertToolTip;
};

[[nodiscard]] RasterColorizeUiState
rasterColorizeUiState(const PointCloudLayerSnapshot *point,
                      std::size_t rasterLayerCount,
                      bool active,
                      bool committing);

} // namespace pci
