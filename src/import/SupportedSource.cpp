#include "import/SupportedSource.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace pci {
namespace {

[[nodiscard]] std::string lowercased(std::string value)
{
    std::ranges::transform(value, value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

[[nodiscard]] bool endsWith(const std::string_view value,
                            const std::string_view suffix) noexcept
{
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

} // namespace

std::optional<SupportedSourceKind>
supportedSourceKind(const std::filesystem::path &path)
{
    const std::string name = lowercased(path.filename().string());
    if (name.empty()) {
        return std::nullopt;
    }

    // Compound source names take precedence over their storage extension.
    if (endsWith(name, "ept.json")) {
        return SupportedSourceKind::PointCloud;
    }
    if (endsWith(name, ".gti.fgb") || endsWith(name, ".gti.shp")) {
        return SupportedSourceKind::Raster;
    }

    // GeoPackage is deliberately vector-only in the initial unified-open
    // workflow. This includes .gti.gpkg; raster GeoPackages remain out of
    // scope until the source router can distinguish their contents safely.
    if (endsWith(name, ".gpkg")) {
        return SupportedSourceKind::Vector;
    }

    if (endsWith(name, ".las") || endsWith(name, ".laz")) {
        return SupportedSourceKind::PointCloud;
    }
    if (endsWith(name, ".geojson") || endsWith(name, ".json") ||
        endsWith(name, ".shp") || endsWith(name, ".kml") ||
        endsWith(name, ".gml") || endsWith(name, ".fgb") ||
        endsWith(name, ".dxf")) {
        return SupportedSourceKind::Vector;
    }
    if (endsWith(name, ".tif") || endsWith(name, ".tiff") ||
        endsWith(name, ".cog") || endsWith(name, ".vrt") ||
        endsWith(name, ".gti") || endsWith(name, ".img") ||
        endsWith(name, ".jp2") || endsWith(name, ".png") ||
        endsWith(name, ".jpg") || endsWith(name, ".jpeg")) {
        return SupportedSourceKind::Raster;
    }
    return std::nullopt;
}

SourceClassification
classifySupportedSources(std::vector<std::filesystem::path> paths)
{
    SourceClassification result;
    result.supported.reserve(paths.size());
    for (std::filesystem::path &path : paths) {
        const std::optional<SupportedSourceKind> kind =
            supportedSourceKind(path);
        if (!kind) {
            result.unsupported.push_back(std::move(path));
            continue;
        }
        result.supported.push_back({.path = std::move(path), .kind = *kind});
    }
    return result;
}

} // namespace pci
