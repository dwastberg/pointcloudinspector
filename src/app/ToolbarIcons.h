#pragma once

#include <QIcon>

class QPalette;

namespace pci {

enum class ToolbarIcon {
    Open,
    Vector,
    Raster,
    Fit,
    TopDown,
    Orthographic,
    Settings,
    Navigate,
    Measure,
};

[[nodiscard]] QIcon toolbarIcon(ToolbarIcon icon, const QPalette &palette);

} // namespace pci
