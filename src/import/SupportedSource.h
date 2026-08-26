#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace pci {

enum class SupportedSourceKind : std::uint8_t {
    PointCloud,
    Vector,
    Raster,
};

struct SupportedSource {
    std::filesystem::path path;
    SupportedSourceKind kind = SupportedSourceKind::PointCloud;
};

struct SourceClassification {
    std::vector<SupportedSource> supported;
    std::vector<std::filesystem::path> unsupported;
};

[[nodiscard]] std::optional<SupportedSourceKind>
supportedSourceKind(const std::filesystem::path &path);

[[nodiscard]] SourceClassification
classifySupportedSources(std::vector<std::filesystem::path> paths);

} // namespace pci
