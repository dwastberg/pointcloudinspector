#pragma once

#include <QByteArray>
#include <QString>
#include <QStringView>

#include <filesystem>
#include <string>

namespace pci {

[[nodiscard]] inline QString pathToQString(const std::filesystem::path &path)
{
#if defined(_WIN32)
    return QString::fromStdWString(path.native());
#else
    const auto &native = path.native();
    return QString::fromUtf8(native.data(),
                             static_cast<qsizetype>(native.size()));
#endif
}

[[nodiscard]] inline std::filesystem::path qStringToPath(const QStringView text)
{
#if defined(_WIN32)
    return std::filesystem::path(text.toString().toStdWString());
#else
    const QByteArray utf8 = text.toUtf8();
    return std::filesystem::path(
        std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
#endif
}

[[nodiscard]] inline QString displayPathName(const std::filesystem::path &path)
{
    const std::filesystem::path filename = path.filename();
    return pathToQString(filename.empty() ? path : filename);
}

} // namespace pci
