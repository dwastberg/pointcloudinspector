#include "app/ApplicationBootstrap.h"
#include "app/ApplicationConfig.h"
#include "app/EmbeddedColorMaps.h"
#include "app/MainWindow.h"
#include "app/StrataTheme.h"
#include "pointcloud/PointColorMapCatalog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QIcon>
#include <QTextStream>

#include <exception>
#include <memory>
#include <optional>
#include <string_view>

namespace {

void configureApplicationMetadata()
{
    QCoreApplication::setApplicationName(
        QStringLiteral("Point Cloud Inspector"));
    QCoreApplication::setOrganizationName(
        QStringLiteral("Point Cloud Inspector"));
    QCoreApplication::setOrganizationDomain(
        QStringLiteral("pointcloudinspector.local"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral(PCINSPECTOR_VERSION));
}

[[nodiscard]] bool requestsGdalCapabilities(const int argc, char *argv[])
{
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--gdal-capabilities") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::optional<int>
writeEarlyExit(const pci::ApplicationInvocation &invocation)
{
    const auto *earlyExit = std::get_if<pci::ConfigEarlyExit>(&invocation);
    if (earlyExit == nullptr) {
        return std::nullopt;
    }
    QTextStream stream(earlyExit->writeToStandardError ? stderr : stdout);
    stream << earlyExit->message;
    if (!earlyExit->message.endsWith(QLatin1Char('\n'))) {
        stream << '\n';
    }
    return earlyExit->exitCode;
}

} // namespace

int main(int argc, char *argv[])
{
    // Packaging probes must not initialize a window-system plugin. On macOS,
    // constructing QApplication alone registers a GUI process and can abort
    // on a genuinely headless runner before command-line parsing begins.
    if (requestsGdalCapabilities(argc, argv)) {
        QCoreApplication application(argc, argv);
        configureApplicationMetadata();
        const pci::ApplicationInvocation invocation =
            pci::parseApplicationInvocation(application.arguments());
        if (const std::optional<int> earlyExit = writeEarlyExit(invocation)) {
            return *earlyExit;
        }
        const pci::ApplicationConfig config =
            std::get<pci::ApplicationConfig>(invocation);
        return config.reportGdalCapabilities ? pci::reportGdalCapabilities()
                                             : 1;
    }

    QApplication application(argc, argv);
    configureApplicationMetadata();
    application.setWindowIcon(
        QIcon(QStringLiteral(":/icons/pcinspector-256.png")));
    pci::applyStrataTheme(application);

    const pci::ApplicationInvocation invocation =
        pci::parseApplicationInvocation(application.arguments());
    if (const std::optional<int> earlyExit = writeEarlyExit(invocation)) {
        return *earlyExit;
    }
    const pci::ApplicationConfig config =
        std::get<pci::ApplicationConfig>(invocation);
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
