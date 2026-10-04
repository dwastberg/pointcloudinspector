#pragma once

#include <cstdint>

namespace pci {

struct PointCloudStorageMetrics {
    std::uint64_t persistentBytes = 0;
    bool localPersistent = false;
    bool committed = false;
    bool reused = false;

    bool operator==(const PointCloudStorageMetrics &) const = default;
};

} // namespace pci
