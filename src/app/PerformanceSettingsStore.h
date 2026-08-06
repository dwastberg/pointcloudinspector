#pragma once

#include "app/PerformanceSettings.h"

class QSettings;

namespace pci {

class PerformanceSettingsStore final {
public:
    [[nodiscard]] static PerformanceSettings
    restore(const PerformanceSettings &defaults = {});
    [[nodiscard]] static PerformanceSettings
    restore(const QSettings &settings,
            const PerformanceSettings &defaults = {});
    static void save(const PerformanceSettings &settings);
    static void save(const PerformanceSettings &performanceSettings,
                     QSettings &settings);
};

} // namespace pci
