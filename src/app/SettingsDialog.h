#pragma once

#include "app/PerformanceSettings.h"
#include "renderer/RenderViewport.h"

#include <QColor>
#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QPushButton;
class QSpinBox;

namespace pci {

class SettingsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const ViewportSettings &settings,
                            const PerformanceSettings &performanceSettings = {},
                            QWidget *parent = nullptr);

    [[nodiscard]] ViewportSettings settings() const noexcept;
    void setSettings(const ViewportSettings &settings);
    [[nodiscard]] PerformanceSettings performanceSettings() const noexcept;
    void setPerformanceSettings(const PerformanceSettings &settings);

signals:
    void settingsChanged(pci::ViewportSettings viewportSettings,
                         pci::PerformanceSettings performanceSettings);

private:
    // Retained so an apply cannot silently discard budgets the dialog does
    // not present.
    RasterPerformanceSettings raster_;
    void chooseBackgroundColor();
    void refreshBackgroundButton();
    void publishSettings();

    QColor backgroundColor_;
    QPushButton *backgroundColorButton_ = nullptr;
    QCheckBox *depthEnhancementEnabled_ = nullptr;
    QDoubleSpinBox *depthEnhancementRadius_ = nullptr;
    QDoubleSpinBox *depthEnhancementStrength_ = nullptr;
    QCheckBox *automaticCpuCache_ = nullptr;
    QSpinBox *cpuCacheMebibytes_ = nullptr;
    QSpinBox *gpuCacheMebibytes_ = nullptr;
    QDoubleSpinBox *maximumLoadPoints_ = nullptr;
};

} // namespace pci
