#pragma once

#include "app/ApplicationOptions.h"
#include "app/MemoryBudgetPolicy.h"
#include "app/PerformanceSettings.h"
#include "platform/SystemMemoryInfo.h"
#include "renderer/GraphicsApi.h"

#include <QString>
#include <QStringList>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <variant>
#include <vector>

namespace pci {

struct QualificationOptions {
    std::filesystem::path reportPath;
    bool exitAfterWrite = false;

    bool operator==(const QualificationOptions &) const = default;
};

struct ApplicationConfig {
    std::vector<std::filesystem::path> sources;
    std::optional<std::uint64_t> syntheticPointCount;
    std::uint64_t maximumLoadPoints = 10'000'000;
    MemoryBudgetOption cpuBudget{.automatic = true};
    std::uint64_t gpuByteBudget = std::uint64_t{512} * 1024 * 1024;
    // Separate from the point budgets above, because raster pixels live in
    // three allocators the point path does not touch.
    RasterPerformanceSettings raster;
    GraphicsApi graphicsApi = GraphicsApi::Auto;
    bool smokeTest = false;
    // Prints what the running GDAL build can open and exits. Packaged builds
    // ship a different driver set from a developer machine, and this is how a
    // package proves its own set without a display or a GPU.
    bool reportGdalCapabilities = false;
    bool gpuValidation = false;
    std::optional<QualificationOptions> qualification;

    bool operator==(const ApplicationConfig &) const = default;
};

struct ConfigEarlyExit {
    QString message;
    int exitCode = 0;
    bool writeToStandardError = false;

    bool operator==(const ConfigEarlyExit &) const = default;
};

using ApplicationInvocation = std::variant<ApplicationConfig, ConfigEarlyExit>;

[[nodiscard]] ApplicationInvocation
parseApplicationInvocation(const QStringList &arguments);

struct ResolvedMemoryBudget {
    std::uint64_t pointByteBudget = 0;
    std::optional<AutomaticMemoryBudgetParameters> automaticParameters;
    AutomaticMemoryBudget automaticBudget;
};

[[nodiscard]] ResolvedMemoryBudget
resolveMemoryBudget(const ApplicationConfig &config,
                    const SystemMemoryInfo &memory) noexcept;

[[nodiscard]] std::filesystem::path
pointPageCacheDirectory(const QString &standardCacheLocation);
[[nodiscard]] std::filesystem::path
pointPageCacheConfigurationDirectory(const QString &standardConfigLocation);

} // namespace pci
