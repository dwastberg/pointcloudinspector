#pragma once

#include <pci/desktop/config/PerformanceSettings.h>
#include <pci/desktop/viewport/RenderViewport.h>

#include <QColor>
#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QPushButton;
class QSpinBox;

namespace pci {

class StorageMaintenanceOperation;

class SettingsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(
        const ViewportSettings &settings,
        const PerformanceSettings &performanceSettings = {},
        QWidget *parent = nullptr,
        StorageMaintenanceOperation *storageMaintenance = nullptr);

    [[nodiscard]] ViewportSettings settings() const noexcept;
    void setSettings(const ViewportSettings &settings);
    [[nodiscard]] PerformanceSettings performanceSettings() const noexcept;
    void setPerformanceSettings(const PerformanceSettings &settings);

signals:
    void settingsChanged(pci::ViewportSettings viewportSettings,
                         pci::PerformanceSettings performanceSettings);

private:
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
    QSpinBox *rasterCpuCacheMebibytes_ = nullptr;
    QSpinBox *rasterGpuCacheMebibytes_ = nullptr;
    QSpinBox *gdalCacheMebibytes_ = nullptr;
    QSpinBox *rasterReadWorkers_ = nullptr;
};

} // namespace pci
