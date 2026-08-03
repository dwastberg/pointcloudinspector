#include "qualification/QualificationDiff.h"

#include <filesystem>
#include <iostream>

int main(const int argc, char **argv)
{
    if (argc != 4) {
        std::cerr
            << "Usage: pci_qualification_diff <baseline.json> <candidate.json> "
               "<tolerances.json>\n"
            << "Exit codes: 0 success, 2 usage, 3 schema error, 4 workload "
               "mismatch, 5 regression\n";
        return static_cast<int>(pci::qualification::DiffExitCode::UsageError);
    }
    const auto result =
        pci::qualification::compareReports(std::filesystem::path(argv[1]),
                                           std::filesystem::path(argv[2]),
                                           std::filesystem::path(argv[3]),
                                           std::cout,
                                           std::cerr);
    return static_cast<int>(result);
}
