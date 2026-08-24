#pragma once

#include "foundation/StrongId.h"

#include <cstdint>

namespace pci {

using LoadJobId = StrongId<struct LoadJobIdTag>;

enum class LoadJobKind : std::uint8_t {
    PointCloud,
    Vector,
    Raster,
    Colorize
};

struct LoadJobKey {
    LoadJobKind kind = LoadJobKind::PointCloud;
    LoadJobId id;
    bool operator==(const LoadJobKey &) const = default;
};

struct LoadJobCapabilities {
    bool canCancel = false;
    bool canRetry = false;
    bool canPrioritize = false;
    bool canDismiss = false;
};

} // namespace pci
