#include "StorageSettingsWidget.h"
#include <QScrollArea>
#include <pci/desktop/ui/SettingsDialog.h>

#include <QCheckBox>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace pci {
namespace {

QColor toQColor(const ViewportColor &color)
{
    return QColor::fromRgbF(color.red, color.green, color.blue);
}

ViewportColor toViewportColor(const QColor &color)
{
    return {
        .red = color.redF(),
        .green = color.greenF(),
        .blue = color.blueF(),
    };
}

QIcon colorSwatch(const QColor &color)
{
    QPixmap pixmap(28, 18);
    pixmap.fill(color);
    QPainter painter(&pixmap);
    painter.setPen(QColor(QStringLiteral("#59616b")));
    painter.drawRect(pixmap.rect().adjusted(0, 0, -1, -1));
    return QIcon(pixmap);
}

} // namespace

SettingsDialog::SettingsDialog(const ViewportSettings &settings,
                               const PerformanceSettings &performanceSettings,
                               QWidget *parent,
                               StorageMaintenanceOperation *storageMaintenance)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(tr("Settings"));
    setModal(false);
    // Tool windows remain above their owning main window while preserving
    // modeless interaction and without floating above unrelated applications.
    setWindowFlag(Qt::Tool, true);

    auto *outerLayout = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    auto *layout = new QVBoxLayout(content);
    scroll->setWidget(content);
    outerLayout->addWidget(scroll, 1);

    auto *appearance = new QGroupBox(tr("Appearance"), this);
    auto *appearanceLayout = new QFormLayout(appearance);
    backgroundColorButton_ = new QPushButton(appearance);
    backgroundColorButton_->setObjectName(
        QStringLiteral("backgroundColorButton"));
    backgroundColorButton_->setIconSize(QSize(28, 18));
    backgroundColorButton_->setAccessibleName(tr("Background color"));
    appearanceLayout->addRow(tr("Background color"), backgroundColorButton_);
    connect(backgroundColorButton_,
            &QPushButton::clicked,
            this,
            &SettingsDialog::chooseBackgroundColor);
    layout->addWidget(appearance);

    auto *depthEnhancement = new QGroupBox(tr("Depth Enhancement"), this);
    auto *depthLayout = new QFormLayout(depthEnhancement);
    depthEnhancementEnabled_ =
        new QCheckBox(tr("Enable depth enhancement"), depthEnhancement);
    depthEnhancementEnabled_->setObjectName(
        QStringLiteral("depthEnhancementEnabledCheckBox"));
    depthLayout->addRow(depthEnhancementEnabled_);

    depthEnhancementRadius_ = new QDoubleSpinBox(depthEnhancement);
    depthEnhancementRadius_->setObjectName(
        QStringLiteral("depthEnhancementRadiusSpinBox"));
    depthEnhancementRadius_->setRange(minimumDepthEnhancementRadius,
                                      maximumDepthEnhancementRadius);
    depthEnhancementRadius_->setSingleStep(0.25);
    depthEnhancementRadius_->setDecimals(2);
    depthEnhancementRadius_->setSuffix(tr(" px"));
    depthEnhancementRadius_->setToolTip(
        tr("Distance from each point at which neighboring depth is sampled"));
    depthLayout->addRow(tr("Radius"), depthEnhancementRadius_);

    depthEnhancementStrength_ = new QDoubleSpinBox(depthEnhancement);
    depthEnhancementStrength_->setObjectName(
        QStringLiteral("depthEnhancementStrengthSpinBox"));
    depthEnhancementStrength_->setRange(minimumDepthEnhancementStrength,
                                        maximumDepthEnhancementStrength);
    depthEnhancementStrength_->setSingleStep(1.0);
    depthEnhancementStrength_->setDecimals(1);
    depthEnhancementStrength_->setToolTip(
        tr("Amount of darkening applied at depth discontinuities"));
    depthLayout->addRow(tr("Strength"), depthEnhancementStrength_);
    layout->addWidget(depthEnhancement);

    connect(depthEnhancementEnabled_,
            &QCheckBox::toggled,
            depthEnhancementRadius_,
            &QWidget::setEnabled);
    connect(depthEnhancementEnabled_,
            &QCheckBox::toggled,
            depthEnhancementStrength_,
            &QWidget::setEnabled);

    auto *performance = new QGroupBox(tr("Performance"), this);
    auto *performanceLayout = new QFormLayout(performance);
    automaticCpuCache_ = new QCheckBox(tr("Adjust automatically"), performance);
    automaticCpuCache_->setObjectName(
        QStringLiteral("automaticCpuCacheCheckBox"));
    performanceLayout->addRow(tr("CPU point cache"), automaticCpuCache_);

    cpuCacheMebibytes_ = new QSpinBox(performance);
    cpuCacheMebibytes_->setObjectName(QStringLiteral("cpuCacheSpinBox"));
    cpuCacheMebibytes_->setRange(1, maximumCacheMebibytes);
    cpuCacheMebibytes_->setSuffix(tr(" MiB"));
    cpuCacheMebibytes_->setGroupSeparatorShown(true);
    cpuCacheMebibytes_->setToolTip(
        tr("Memory available for retained and decoded point data"));
    performanceLayout->addRow(tr("CPU cache limit"), cpuCacheMebibytes_);

    gpuCacheMebibytes_ = new QSpinBox(performance);
    gpuCacheMebibytes_->setObjectName(QStringLiteral("gpuCacheSpinBox"));
    gpuCacheMebibytes_->setRange(1, maximumCacheMebibytes);
    gpuCacheMebibytes_->setSuffix(tr(" MiB"));
    gpuCacheMebibytes_->setGroupSeparatorShown(true);
    gpuCacheMebibytes_->setToolTip(
        tr("Memory available for point buffers on the graphics device"));
    performanceLayout->addRow(tr("GPU cache limit"), gpuCacheMebibytes_);

    maximumLoadPoints_ = new QDoubleSpinBox(performance);
    maximumLoadPoints_->setObjectName(
        QStringLiteral("maximumLoadPointsSpinBox"));
    maximumLoadPoints_->setRange(1.0, maximumLoadPointsSetting);
    maximumLoadPoints_->setDecimals(0);
    maximumLoadPoints_->setSingleStep(1'000'000.0);
    maximumLoadPoints_->setGroupSeparatorShown(true);
    maximumLoadPoints_->setToolTip(
        tr("Maximum points retained for non-paged sources; paged and "
           "hierarchical sources retain all points"));
    performanceLayout->addRow(tr("Maximum non-paged points"),
                              maximumLoadPoints_);
    layout->addWidget(performance);

    // Separate from the point caches on purpose: three different allocators
    // hold raster pixels, and the user needs to see all three.
    auto *rasterPerformance = new QGroupBox(tr("Raster Layers"), this);
    rasterPerformance->setObjectName(QStringLiteral("rasterPerformanceGroup"));
    auto *rasterLayout = new QFormLayout(rasterPerformance);

    rasterCpuCacheMebibytes_ = new QSpinBox(rasterPerformance);
    rasterCpuCacheMebibytes_->setObjectName(
        QStringLiteral("rasterCpuCacheSpinBox"));
    rasterCpuCacheMebibytes_->setRange(
        static_cast<int>(minimumRasterCpuCacheMebibytes),
        maximumCacheMebibytes);
    rasterCpuCacheMebibytes_->setSuffix(tr(" MiB"));
    rasterCpuCacheMebibytes_->setGroupSeparatorShown(true);
    rasterCpuCacheMebibytes_->setToolTip(
        tr("Memory available for decoded raster tiles, separate from the "
           "point caches"));
    rasterLayout->addRow(tr("Raster CPU cache"), rasterCpuCacheMebibytes_);

    rasterGpuCacheMebibytes_ = new QSpinBox(rasterPerformance);
    rasterGpuCacheMebibytes_->setObjectName(
        QStringLiteral("rasterGpuCacheSpinBox"));
    rasterGpuCacheMebibytes_->setRange(
        static_cast<int>(minimumRasterGpuCacheMebibytes),
        maximumCacheMebibytes);
    rasterGpuCacheMebibytes_->setSuffix(tr(" MiB"));
    rasterGpuCacheMebibytes_->setGroupSeparatorShown(true);
    rasterGpuCacheMebibytes_->setToolTip(
        tr("Memory available for raster tile textures on the graphics "
           "device"));
    rasterLayout->addRow(tr("Raster GPU cache"), rasterGpuCacheMebibytes_);

    gdalCacheMebibytes_ = new QSpinBox(rasterPerformance);
    gdalCacheMebibytes_->setObjectName(QStringLiteral("gdalCacheSpinBox"));
    gdalCacheMebibytes_->setRange(static_cast<int>(minimumGdalCacheMebibytes),
                                  maximumCacheMebibytes);
    gdalCacheMebibytes_->setSuffix(tr(" MiB"));
    gdalCacheMebibytes_->setGroupSeparatorShown(true);
    gdalCacheMebibytes_->setToolTip(
        tr("Block cache GDAL uses while reading raster sources"));
    rasterLayout->addRow(tr("GDAL block cache"), gdalCacheMebibytes_);

    rasterReadWorkers_ = new QSpinBox(rasterPerformance);
    rasterReadWorkers_->setObjectName(QStringLiteral("rasterWorkersSpinBox"));
    rasterReadWorkers_->setRange(static_cast<int>(minimumRasterReadWorkers),
                                 static_cast<int>(maximumRasterReadWorkers));
    // The read pool is created once, so a change here cannot take effect
    // until the next launch. Saying so beats silently ignoring the edit.
    rasterReadWorkers_->setToolTip(
        tr("Threads reading raster tiles. Applies after restart."));
    rasterLayout->addRow(tr("Raster read workers"), rasterReadWorkers_);
    layout->addWidget(rasterPerformance);

    connect(automaticCpuCache_,
            &QCheckBox::toggled,
            cpuCacheMebibytes_,
            [this](const bool automatic) {
                cpuCacheMebibytes_->setEnabled(!automatic);
            });

    auto *storage = new StorageSettingsWidget(storageMaintenance, content);
    layout->addWidget(storage);
    connect(this, &QDialog::finished, storage, [storage] {
        storage->cancel();
    });

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("settingsButtonBox"));
    QPushButton *restoreDefaults =
        buttons->addButton(tr("Restore Defaults"), QDialogButtonBox::ResetRole);
    restoreDefaults->setObjectName(QStringLiteral("restoreDefaultsButton"));
    connect(restoreDefaults, &QPushButton::clicked, this, [this] {
        setSettings(ViewportSettings{});
        setPerformanceSettings(PerformanceSettings{});
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outerLayout->addWidget(buttons);
    resize(760, 680);

    setSettings(settings);
    setPerformanceSettings(performanceSettings);

    const auto publish = [this] {
        publishSettings();
    };
    connect(depthEnhancementEnabled_, &QCheckBox::toggled, this, publish);
    connect(
        depthEnhancementRadius_, &QDoubleSpinBox::valueChanged, this, publish);
    connect(depthEnhancementStrength_,
            &QDoubleSpinBox::valueChanged,
            this,
            publish);
    connect(automaticCpuCache_, &QCheckBox::toggled, this, publish);
    connect(cpuCacheMebibytes_, &QSpinBox::valueChanged, this, publish);
    connect(gpuCacheMebibytes_, &QSpinBox::valueChanged, this, publish);
    connect(maximumLoadPoints_, &QDoubleSpinBox::valueChanged, this, publish);
    connect(rasterCpuCacheMebibytes_, &QSpinBox::valueChanged, this, publish);
    connect(rasterGpuCacheMebibytes_, &QSpinBox::valueChanged, this, publish);
    connect(gdalCacheMebibytes_, &QSpinBox::valueChanged, this, publish);
    connect(rasterReadWorkers_, &QSpinBox::valueChanged, this, publish);
}

ViewportSettings SettingsDialog::settings() const noexcept
{
    return {
        .backgroundColor = toViewportColor(backgroundColor_),
        .depthEnhancement =
            {
                .enabled = depthEnhancementEnabled_->isChecked(),
                .radius = static_cast<float>(depthEnhancementRadius_->value()),
                .strength =
                    static_cast<float>(depthEnhancementStrength_->value()),
            },
    };
}

void SettingsDialog::setSettings(const ViewportSettings &settings)
{
    const QSignalBlocker enabledBlocker(depthEnhancementEnabled_);
    const QSignalBlocker radiusBlocker(depthEnhancementRadius_);
    const QSignalBlocker strengthBlocker(depthEnhancementStrength_);
    backgroundColor_ = toQColor(settings.backgroundColor);
    depthEnhancementEnabled_->setChecked(settings.depthEnhancement.enabled);
    depthEnhancementRadius_->setValue(settings.depthEnhancement.radius);
    depthEnhancementStrength_->setValue(settings.depthEnhancement.strength);
    depthEnhancementRadius_->setEnabled(settings.depthEnhancement.enabled);
    depthEnhancementStrength_->setEnabled(settings.depthEnhancement.enabled);
    refreshBackgroundButton();
    publishSettings();
}

PerformanceSettings SettingsDialog::performanceSettings() const noexcept
{
    return {
        .automaticCpuCache = automaticCpuCache_->isChecked(),
        .cpuCacheMebibytes =
            static_cast<std::uint64_t>(cpuCacheMebibytes_->value()),
        .gpuCacheMebibytes =
            static_cast<std::uint64_t>(gpuCacheMebibytes_->value()),
        .maximumLoadPoints =
            static_cast<std::uint64_t>(maximumLoadPoints_->value()),
        .raster = clampRasterPerformanceSettings({
            .cpuCacheMebibytes =
                static_cast<std::uint64_t>(rasterCpuCacheMebibytes_->value()),
            .gpuCacheMebibytes =
                static_cast<std::uint64_t>(rasterGpuCacheMebibytes_->value()),
            .gdalCacheMebibytes =
                static_cast<std::uint64_t>(gdalCacheMebibytes_->value()),
            .readWorkers =
                static_cast<std::uint32_t>(rasterReadWorkers_->value()),
        }),
    };
}

void SettingsDialog::setPerformanceSettings(const PerformanceSettings &settings)
{
    const RasterPerformanceSettings raster =
        clampRasterPerformanceSettings(settings.raster);
    const QSignalBlocker rasterCpuBlocker(rasterCpuCacheMebibytes_);
    const QSignalBlocker rasterGpuBlocker(rasterGpuCacheMebibytes_);
    const QSignalBlocker gdalBlocker(gdalCacheMebibytes_);
    const QSignalBlocker workersBlocker(rasterReadWorkers_);
    rasterCpuCacheMebibytes_->setValue(static_cast<int>(std::min<std::uint64_t>(
        raster.cpuCacheMebibytes,
        static_cast<std::uint64_t>(maximumCacheMebibytes))));
    rasterGpuCacheMebibytes_->setValue(static_cast<int>(std::min<std::uint64_t>(
        raster.gpuCacheMebibytes,
        static_cast<std::uint64_t>(maximumCacheMebibytes))));
    gdalCacheMebibytes_->setValue(static_cast<int>(std::min<std::uint64_t>(
        raster.gdalCacheMebibytes,
        static_cast<std::uint64_t>(maximumCacheMebibytes))));
    rasterReadWorkers_->setValue(static_cast<int>(raster.readWorkers));

    const QSignalBlocker automaticBlocker(automaticCpuCache_);
    const QSignalBlocker cpuBlocker(cpuCacheMebibytes_);
    const QSignalBlocker gpuBlocker(gpuCacheMebibytes_);
    const QSignalBlocker pointsBlocker(maximumLoadPoints_);
    automaticCpuCache_->setChecked(settings.automaticCpuCache);
    cpuCacheMebibytes_->setValue(static_cast<int>(std::min<std::uint64_t>(
        settings.cpuCacheMebibytes, maximumCacheMebibytes)));
    gpuCacheMebibytes_->setValue(static_cast<int>(std::min<std::uint64_t>(
        settings.gpuCacheMebibytes, maximumCacheMebibytes)));
    maximumLoadPoints_->setValue(
        std::min(static_cast<double>(settings.maximumLoadPoints),
                 maximumLoadPointsSetting));
    cpuCacheMebibytes_->setEnabled(!settings.automaticCpuCache);
    publishSettings();
}

void SettingsDialog::chooseBackgroundColor()
{
    if (QColorDialog *existing = findChild<QColorDialog *>(
            QStringLiteral("backgroundColorDialog"))) {
        existing->raise();
        existing->activateWindow();
        return;
    }

    const QColor original = backgroundColor_;
    auto *dialog = new QColorDialog(backgroundColor_, this);
    dialog->setObjectName(QStringLiteral("backgroundColorDialog"));
    dialog->setWindowTitle(tr("Background color"));
    dialog->setOption(QColorDialog::ShowAlphaChannel, false);
    dialog->setModal(false);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog,
            &QColorDialog::currentColorChanged,
            this,
            [this](const QColor &color) {
                if (!color.isValid()) {
                    return;
                }
                backgroundColor_ = color;
                refreshBackgroundButton();
                publishSettings();
            });
    connect(dialog, &QDialog::rejected, this, [this, original] {
        backgroundColor_ = original;
        refreshBackgroundButton();
        publishSettings();
    });
    dialog->show();
}

void SettingsDialog::refreshBackgroundButton()
{
    const QString name = backgroundColor_.name(QColor::HexRgb).toUpper();
    backgroundColorButton_->setIcon(colorSwatch(backgroundColor_));
    backgroundColorButton_->setText(name);
    backgroundColorButton_->setToolTip(tr("Choose viewport background color"));
    backgroundColorButton_->setAccessibleDescription(
        tr("Viewport background color %1").arg(name));
}

void SettingsDialog::publishSettings()
{
    if (!backgroundColorButton_ || !automaticCpuCache_) {
        return;
    }
    emit settingsChanged(settings(), performanceSettings());
}

} // namespace pci
