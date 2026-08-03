#include "platform/QtPath.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

TEST_CASE("Qt path conversion handles empty and non-ASCII native paths",
          "[qt][path]")
{
    const std::filesystem::path empty;
    CHECK(pci::pathToQString(empty).isEmpty());
    CHECK(pci::qStringToPath(QStringView{}) == empty);

    const QString nonAscii = QStringLiteral("mätdata/測量.laz");
    const std::filesystem::path path = pci::qStringToPath(nonAscii);
    CHECK(pci::pathToQString(path) == nonAscii);
    CHECK(pci::displayPathName(path) == QStringLiteral("測量.laz"));
}

TEST_CASE("display path names fall back when no filename exists", "[qt][path]")
{
    const std::filesystem::path root =
        std::filesystem::path::preferred_separator == '\\'
            ? std::filesystem::path("C:\\")
            : std::filesystem::path("/");
    CHECK(pci::displayPathName(root) == pci::pathToQString(root));
}
