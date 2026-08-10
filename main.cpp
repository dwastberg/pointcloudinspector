#include "app/ApplicationBootstrap.h"
#include "app/ApplicationConfig.h"
#include "app/EmbeddedColorMaps.h"
#include "app/MainWindow.h"
#include "app/StrataTheme.h"
#include "pointcloud/PointColorMapCatalog.h"

#include <QApplication>
#include <QDebug>
#include <QIcon>
#include <QTextStream>

#include <exception>
#include <memory>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("Point Cloud Inspector"));
    QCoreApplication::setOrganizationName(
        QStringLiteral("Point Cloud Inspector"));
    QCoreApplication::setOrganizationDomain(
        QStringLiteral("pointcloudinspector.local"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral(PCINSPECTOR_VERSION));
    application.setWindowIcon(
        QIcon(QStringLiteral(":/icons/pcinspector-256.png")));
    pci::applyStrataTheme(application);

    const pci::ApplicationInvocation invocation =
        pci::parseApplicationInvocation(application.arguments());
    if (const auto *earlyExit =
            std::get_if<pci::ConfigEarlyExit>(&invocation)) {
        QTextStream stream(earlyExit->writeToStandardError ? stderr : stdout);
        stream << earlyExit->message;
        if (!earlyExit->message.endsWith(QLatin1Char('\n'))) {
            stream << '\n';
        }
        return earlyExit->exitCode;
    }
    const pci::ApplicationConfig config =
        std::get<pci::ApplicationConfig>(invocation);
    if (config.reportGdalCapabilities) {
        // Answered before any window or renderer exists, so a packaged build
        // can be checked on a headless machine.
        return pci::reportGdalCapabilities();
    }

    pci::PointColorMapCatalog colorMapCatalog;
    const pci::EmbeddedColorMapLoadResult colorMapLoad =
        pci::loadEmbeddedColorMaps(colorMapCatalog);
    for (const pci::EmbeddedColorMapIssue &issue : colorMapLoad.issues) {
        const QString location = issue.line > 0 ? QStringLiteral("%1:%2")
                                                      .arg(issue.resourcePath)
                                                      .arg(issue.line)
                                                : issue.resourcePath;
        qWarning().noquote()
            << QStringLiteral("Could not load color map %1: %2")
                   .arg(location, issue.message);
    }

    try {
        std::unique_ptr<pci::MainWindow> window =
            pci::bootstrapApplication(config, colorMapCatalog.freeze());
        return application.exec();
    } catch (const std::exception &error) {
        qCritical().noquote() << QStringLiteral("Could not start renderer: %1")
                                     .arg(QString::fromUtf8(error.what()));
        return 1;
    }
}
