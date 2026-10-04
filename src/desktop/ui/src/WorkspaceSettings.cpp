#include <pci/desktop/ui/WorkspaceSettings.h>

#include <QCoreApplication>
#include <QMainWindow>
#include <QSettings>

namespace pci {
namespace {

constexpr int workspaceStateVersion = 1;

} // namespace

void WorkspaceSettings::restore(QMainWindow &window)
{
    if (QCoreApplication::organizationName().isEmpty()) {
        return;
    }
    const QSettings settings;
    restore(window, settings);
}

void WorkspaceSettings::save(const QMainWindow &window)
{
    if (QCoreApplication::organizationName().isEmpty()) {
        return;
    }
    QSettings settings;
    save(window, settings);
}

void WorkspaceSettings::restore(QMainWindow &window, const QSettings &settings)
{
    const QByteArray geometry =
        settings.value(QStringLiteral("workspace/geometry")).toByteArray();
    const QByteArray state =
        settings.value(QStringLiteral("workspace/state")).toByteArray();
    if (!geometry.isEmpty()) {
        window.restoreGeometry(geometry);
    }
    if (!state.isEmpty()) {
        window.restoreState(state, workspaceStateVersion);
    }
}

void WorkspaceSettings::save(const QMainWindow &window, QSettings &settings)
{
    settings.setValue(QStringLiteral("workspace/geometry"),
                      window.saveGeometry());
    settings.setValue(QStringLiteral("workspace/state"),
                      window.saveState(workspaceStateVersion));
}

} // namespace pci
