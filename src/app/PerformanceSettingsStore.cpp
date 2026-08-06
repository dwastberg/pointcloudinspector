#include "app/PerformanceSettingsStore.h"

#include <QCoreApplication>
#include <QSettings>
#include <QVariant>

namespace pci {
namespace {

constexpr auto automaticCpuCacheKey = "performance/cpuCacheAutomatic";
constexpr auto cpuCacheMebibytesKey = "performance/cpuCacheMiB";
constexpr auto gpuCacheMebibytesKey = "performance/gpuCacheMiB";
constexpr auto maximumLoadPointsKey = "performance/maximumLoadPoints";

std::uint64_t positiveInteger(const QVariant &value,
                              const std::uint64_t fallback)
{
    bool valid = false;
    const qulonglong parsed = value.toULongLong(&valid);
    return valid && parsed > 0 ? static_cast<std::uint64_t>(parsed) : fallback;
}

} // namespace

PerformanceSettings
PerformanceSettingsStore::restore(const PerformanceSettings &defaults)
{
    if (QCoreApplication::organizationName().isEmpty()) {
        return defaults;
    }
    const QSettings settings;
    return restore(settings, defaults);
}

PerformanceSettings
PerformanceSettingsStore::restore(const QSettings &settings,
                                  const PerformanceSettings &defaults)
{
    return {
        .automaticCpuCache =
            settings
                .value(QString::fromLatin1(automaticCpuCacheKey),
                       defaults.automaticCpuCache)
                .toBool(),
        .cpuCacheMebibytes = positiveInteger(
            settings.value(QString::fromLatin1(cpuCacheMebibytesKey)),
            defaults.cpuCacheMebibytes),
        .gpuCacheMebibytes = positiveInteger(
            settings.value(QString::fromLatin1(gpuCacheMebibytesKey)),
            defaults.gpuCacheMebibytes),
        .maximumLoadPoints = positiveInteger(
            settings.value(QString::fromLatin1(maximumLoadPointsKey)),
            defaults.maximumLoadPoints),
    };
}

void PerformanceSettingsStore::save(const PerformanceSettings &settings)
{
    if (QCoreApplication::organizationName().isEmpty()) {
        return;
    }
    QSettings storage;
    save(settings, storage);
}

void PerformanceSettingsStore::save(
    const PerformanceSettings &performanceSettings, QSettings &settings)
{
    settings.setValue(QString::fromLatin1(automaticCpuCacheKey),
                      performanceSettings.automaticCpuCache);
    settings.setValue(
        QString::fromLatin1(cpuCacheMebibytesKey),
        QVariant::fromValue<qulonglong>(performanceSettings.cpuCacheMebibytes));
    settings.setValue(
        QString::fromLatin1(gpuCacheMebibytesKey),
        QVariant::fromValue<qulonglong>(performanceSettings.gpuCacheMebibytes));
    settings.setValue(
        QString::fromLatin1(maximumLoadPointsKey),
        QVariant::fromValue<qulonglong>(performanceSettings.maximumLoadPoints));
}

} // namespace pci
