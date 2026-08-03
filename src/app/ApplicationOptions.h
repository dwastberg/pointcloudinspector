#pragma once

#include "renderer/GraphicsApi.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pci {

[[nodiscard]] std::optional<GraphicsApi>
parseGraphicsApi(std::string_view text) noexcept;

std::optional<std::uint64_t> parsePointCount(std::string_view text);

struct MemoryBudgetOption {
    bool automatic = false;
    std::uint64_t mebibytes = 0;

    bool operator==(const MemoryBudgetOption &) const = default;
};

// Accepts the literal "auto" or a positive number of mebibytes.
[[nodiscard]] std::optional<MemoryBudgetOption>
parseMemoryBudgetOption(std::string_view text) noexcept;

// Expands command-line file arguments into concrete paths. Arguments that
// contain glob metacharacters (*, ?, [set]) are matched against files in the
// referenced directory (the wildcard applies to the final path component);
// matches are returned sorted. Arguments without wildcards pass through
// unchanged so the loader can report a missing file. Duplicate results are
// removed, preserving first-seen order.
std::vector<std::filesystem::path>
expandPathArguments(const std::vector<std::filesystem::path> &arguments);

} // namespace pci
