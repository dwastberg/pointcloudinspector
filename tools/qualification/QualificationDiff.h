#pragma once

#include <filesystem>
#include <iosfwd>

namespace pci::qualification {

enum class DiffExitCode : int {
    Success = 0,
    UsageError = 2,
    SchemaError = 3,
    WorkloadMismatch = 4,
    Regression = 5,
};

[[nodiscard]] DiffExitCode
compareReports(const std::filesystem::path &baselinePath,
               const std::filesystem::path &candidatePath,
               const std::filesystem::path &tolerancePath,
               std::ostream &output,
               std::ostream &errors);

} // namespace pci::qualification
