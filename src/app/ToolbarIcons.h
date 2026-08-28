#pragma once

#include <QIcon>

class QPalette;

namespace pci {

enum class ToolbarIcon {
    New,
    Open,
    Vector,
    Raster,
    ColorizeRaster,
    RevertColors,
    Fit,
    TopDown,
    Orthographic,
    Settings,
    Navigate,
    Measure,
};

[[nodiscard]] QIcon toolbarIcon(ToolbarIcon icon, const QPalette &palette);

} // namespace pci
