#include "app/ViewportSettingsStore.h"

#include <QColor>
#include <QCoreApplication>
#include <QSettings>

namespace pci {
namespace {

constexpr auto backgroundColorKey = "viewport/backgroundColor";
constexpr auto depthEnabledKey = "viewport/depthEnhancement/enabled";
constexpr auto depthRadiusKey = "viewport/depthEnhancement/radius";
constexpr auto depthStrengthKey = "viewport/depthEnhancement/strength";

QColor toQColor(const ViewportColor &color)
{
    return QColor::fromRgbF(color.red, color.green, color.blue);
}

} // namespace

ViewportSettings
ViewportSettingsStore::restore(const ViewportSettings &defaults)
{
    if (QCoreApplication::organizationName().isEmpty()) {
        return defaults;
    }
    const QSettings settings;
    return restore(settings, defaults);
}

ViewportSettings
ViewportSettingsStore::restore(const QSettings &settings,
                               const ViewportSettings &defaults)
{
    ViewportSettings restored = defaults;
    const QColor background =
        settings
            .value(QString::fromLatin1(backgroundColorKey),
                   toQColor(defaults.backgroundColor))
            .value<QColor>();
    if (background.isValid()) {
        restored.backgroundColor = {
            .red = background.redF(),
            .green = background.greenF(),
            .blue = background.blueF(),
        };
    }
    restored.depthEnhancement.enabled =
        settings
            .value(QString::fromLatin1(depthEnabledKey),
                   defaults.depthEnhancement.enabled)
            .toBool();
    restored.depthEnhancement.radius =
        settings
            .value(QString::fromLatin1(depthRadiusKey),
                   defaults.depthEnhancement.radius)
            .toFloat();
    restored.depthEnhancement.strength =
        settings
            .value(QString::fromLatin1(depthStrengthKey),
                   defaults.depthEnhancement.strength)
            .toFloat();
    return restored;
}

void ViewportSettingsStore::save(const ViewportSettings &settings)
{
    if (QCoreApplication::organizationName().isEmpty()) {
        return;
    }
    QSettings storage;
    save(settings, storage);
}

void ViewportSettingsStore::save(const ViewportSettings &viewportSettings,
                                 QSettings &settings)
{
    settings.setValue(QString::fromLatin1(backgroundColorKey),
                      toQColor(viewportSettings.backgroundColor));
    settings.setValue(QString::fromLatin1(depthEnabledKey),
                      viewportSettings.depthEnhancement.enabled);
    settings.setValue(QString::fromLatin1(depthRadiusKey),
                      viewportSettings.depthEnhancement.radius);
    settings.setValue(QString::fromLatin1(depthStrengthKey),
                      viewportSettings.depthEnhancement.strength);
}

} // namespace pci
