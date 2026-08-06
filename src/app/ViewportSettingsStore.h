#pragma once

#include "renderer/RenderViewport.h"

class QSettings;

namespace pci {

class ViewportSettingsStore final {
public:
    [[nodiscard]] static ViewportSettings
    restore(const ViewportSettings &defaults = {});
    [[nodiscard]] static ViewportSettings
    restore(const QSettings &settings, const ViewportSettings &defaults = {});
    static void save(const ViewportSettings &settings);
    static void save(const ViewportSettings &viewportSettings,
                     QSettings &settings);
};

} // namespace pci
