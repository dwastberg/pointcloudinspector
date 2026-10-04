#include <pci/desktop/config/SupportedSource.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace pci {
namespace {

using namespace std::literals;

[[nodiscard]] std::string lowercased(std::string value)
{
    std::ranges::transform(value, value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

constexpr std::array pointSuffixes{"ept.json"sv, ".las"sv, ".laz"sv};
constexpr std::array pointPatterns{
    "*.las"sv, "*.laz"sv, "*.copc.laz"sv, "*ept.json"sv};
constexpr std::array pointCombinedPatterns{"*.las"sv, "*.laz"sv, "*ept.json"sv};

constexpr std::array vectorSuffixes{".gpkg"sv,
                                    ".geojson"sv,
                                    ".json"sv,
                                    ".shp"sv,
                                    ".kml"sv,
                                    ".gml"sv,
                                    ".fgb"sv,
                                    ".dxf"sv};
// GeoPackage remains vector-only, including .gti.gpkg, until routing can
// inspect container contents without opening a source twice.
constexpr std::array vectorPatterns{"*.gpkg"sv,
                                    "*.geojson"sv,
                                    "*.json"sv,
                                    "*.shp"sv,
                                    "*.kml"sv,
                                    "*.gml"sv,
                                    "*.fgb"sv,
                                    "*.dxf"sv};

constexpr std::array rasterSuffixes{".gti.fgb"sv,
                                    ".gti.shp"sv,
                                    ".tif"sv,
                                    ".tiff"sv,
                                    ".cog"sv,
                                    ".vrt"sv,
                                    ".gti"sv,
                                    ".img"sv,
                                    ".jp2"sv,
                                    ".png"sv,
                                    ".jpg"sv,
                                    ".jpeg"sv};
constexpr std::array rasterPatterns{"*.tif"sv,
                                    "*.tiff"sv,
                                    "*.cog"sv,
                                    "*.vrt"sv,
                                    "*.gti"sv,
                                    "*.img"sv,
                                    "*.jp2"sv,
                                    "*.png"sv,
                                    "*.jpg"sv,
                                    "*.jpeg"sv};

constexpr std::array sourceDescriptors{
    SupportedSourceDescriptor{
        .kind = SupportedSourceKind::PointCloud,
        .dialogLabel = "Point clouds",
        .routingSuffixes = pointSuffixes,
        .dialogPatterns = pointPatterns,
        .combinedDialogPatterns = pointCombinedPatterns,
    },
    SupportedSourceDescriptor{
        .kind = SupportedSourceKind::Vector,
        .dialogLabel = "Vector files",
        .routingSuffixes = vectorSuffixes,
        .dialogPatterns = vectorPatterns,
        .combinedDialogPatterns = vectorPatterns,
    },
    SupportedSourceDescriptor{
        .kind = SupportedSourceKind::Raster,
        .dialogLabel = "Raster files",
        .routingSuffixes = rasterSuffixes,
        .dialogPatterns = rasterPatterns,
        .combinedDialogPatterns = rasterPatterns,
    },
};

void appendPatterns(std::string &result,
                    const std::span<const std::string_view> patterns)
{
    for (const std::string_view pattern : patterns) {
        if (result.back() != '(') {
            result.push_back(' ');
        }
        result.append(pattern);
    }
}

} // namespace

std::optional<SupportedSourceKind>
supportedSourceKind(const std::filesystem::path &path)
{
    const std::string name = lowercased(path.filename().string());
    if (name.empty()) {
        return std::nullopt;
    }

    std::optional<SupportedSourceKind> result;
    std::size_t matchedLength = 0;
    for (const SupportedSourceDescriptor &descriptor : sourceDescriptors) {
        for (const std::string_view suffix : descriptor.routingSuffixes) {
            // Longest suffix wins, so ept.json and .gti.fgb take precedence
            // over the generic .json and .fgb routes without special cases.
            if (suffix.size() > matchedLength && name.ends_with(suffix)) {
                result = descriptor.kind;
                matchedLength = suffix.size();
            }
        }
    }
    return result;
}

std::span<const SupportedSourceDescriptor> supportedSourceDescriptors() noexcept
{
    return sourceDescriptors;
}

std::string supportedSourceDialogFilter()
{
    std::string result = "Supported files (";
    for (const SupportedSourceDescriptor &descriptor : sourceDescriptors) {
        appendPatterns(result, descriptor.combinedDialogPatterns);
    }
    result += ")";
    for (const SupportedSourceDescriptor &descriptor : sourceDescriptors) {
        result += ";;";
        result += descriptor.dialogLabel;
        result += " (";
        appendPatterns(result, descriptor.dialogPatterns);
        result += ")";
    }
    result += ";;All files (*)";
    return result;
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
