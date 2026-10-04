#pragma once

#include <pci/desktop/operations/LoadJobRow.h>
#include <pci/operations/OperationRow.h>

namespace pci {
[[nodiscard]] inline std::vector<LoadJobRow>
desktopOperationRows(const std::vector<OperationRow> &rows)
{
    std::vector<LoadJobRow> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back({row.key,
                          QString::fromStdString(row.title),
                          QString::fromStdString(row.detail),
                          row.completion,
                          row.terminal,
                          row.capabilities});
    }
    return result;
}
} // namespace pci
