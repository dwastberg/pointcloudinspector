#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

struct SupportedSourceDescriptor {
    SupportedSourceKind kind = SupportedSourceKind::PointCloud;
    std::string_view dialogLabel;
    std::span<const std::string_view> routingSuffixes;
    std::span<const std::string_view> dialogPatterns;
    std::span<const std::string_view> combinedDialogPatterns;
};

struct SourceClassification {
    std::vector<SupportedSource> supported;
    std::vector<std::filesystem::path> unsupported;
};

[[nodiscard]] std::optional<SupportedSourceKind>
supportedSourceKind(const std::filesystem::path &path);

[[nodiscard]] std::span<const SupportedSourceDescriptor>
supportedSourceDescriptors() noexcept;

// Qt-free source of the native file dialog filter. Keeping this beside the
// classifier prevents a newly routed suffix from drifting away from the UI.
[[nodiscard]] std::string supportedSourceDialogFilter();

[[nodiscard]] SourceClassification
classifySupportedSources(std::vector<std::filesystem::path> paths);

} // namespace pci
