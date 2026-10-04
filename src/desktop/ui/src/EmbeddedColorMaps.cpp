#include <pci/desktop/ui/EmbeddedColorMaps.h>

#include <pci/color/CptColorMapParser.h>
#include <pci/color/PointColorMapCatalog.h>

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace pci {
namespace {

QString displayNameForResource(const QString &resourcePath)
{
    QString name = QFileInfo(resourcePath).completeBaseName();
    name.replace(u'_', u' ');
    name.replace(u'-', u' ');
    if (!name.isEmpty()) {
        name[0] = name.front().toUpper();
    }
    return name;
}

std::string stableKeyForResource(const QString &resourcePath)
{
    QString relative =
        QDir(QStringLiteral(":/colormaps")).relativeFilePath(resourcePath);
    relative.replace(u'\\', u'/');
    return QStringLiteral("cpt:%1")
        .arg(relative.toCaseFolded())
        .toUtf8()
        .toStdString();
}

} // namespace

EmbeddedColorMapLoadResult loadEmbeddedColorMaps(PointColorMapCatalog &catalog)
{
    QStringList paths;
    QDirIterator iterator(QStringLiteral(":/colormaps"),
                          QStringList{QStringLiteral("*.cpt")},
                          QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        paths.push_back(iterator.next());
    }
    std::ranges::sort(paths, {}, [](const QString &path) {
        return path.toCaseFolded();
    });

    EmbeddedColorMapLoadResult result;
    for (const QString &path : paths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            result.issues.push_back({
                .resourcePath = path,
                .message = file.errorString(),
            });
            continue;
        }
        const QByteArray contents = file.readAll();
        if (file.error() != QFileDevice::NoError) {
            result.issues.push_back({
                .resourcePath = path,
                .message = file.errorString(),
            });
            continue;
        }

        const QByteArray fallbackName = displayNameForResource(path).toUtf8();
        CptColorMapParseResult parsed = parseCptColorMap(
            std::string_view{contents.constData(),
                             static_cast<std::size_t>(contents.size())},
            std::string_view{fallbackName.constData(),
                             static_cast<std::size_t>(fallbackName.size())});
        if (!parsed) {
            const CptColorMapParseError &error = parsed.error();
            result.issues.push_back({
                .resourcePath = path,
                .line = error.line,
                .message = QString::fromStdString(error.message),
            });
            continue;
        }

        ParsedCptColorMap colorMap = std::move(*parsed);
        PointColorMapRegistrationResult registration =
            catalog.registerContinuous(stableKeyForResource(path),
                                       std::move(colorMap.name),
                                       std::move(colorMap.stops),
                                       std::move(colorMap.description));
        if (!registration) {
            result.issues.push_back({
                .resourcePath = path,
                .message = QString::fromStdString(registration.error),
            });
            continue;
        }
        if (registration.added) {
            ++result.loadedMapCount;
        }
    }
    return result;
}

} // namespace pci
