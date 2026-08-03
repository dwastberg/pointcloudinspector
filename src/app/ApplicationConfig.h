#pragma once

#include "app/ApplicationOptions.h"
#include "app/MemoryBudgetPolicy.h"
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
    std::uint64_t syntheticPointCount = 10'000'000;
    std::uint64_t maximumLoadPoints = 10'000'000;
    MemoryBudgetOption cpuBudget{.automatic = true};
    std::uint64_t gpuByteBudget = 512ULL * 1024 * 1024;
    GraphicsApi graphicsApi = GraphicsApi::Auto;
    bool smokeTest = false;
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
pointPageCacheDirectory(const QString &standardCacheLocation,
                        const std::filesystem::path &temporaryDirectory);

} // namespace pci
