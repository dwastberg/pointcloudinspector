#pragma once

#include <cstdint>

namespace pci {

struct RuntimeBudgetSnapshot {
    std::uint64_t decodedPointBytes = 0;

    [[nodiscard]] bool valid() const noexcept
    {
        return decodedPointBytes > 0;
    }

    bool operator==(const RuntimeBudgetSnapshot &) const = default;
};

} // namespace pci
