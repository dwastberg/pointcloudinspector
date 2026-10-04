#pragma once

#include <pci/runtime/point/PointDatasetRuntime.h>

#include <cstdint>

namespace pci {

[[nodiscard]] PointDatasetRuntimePtr
buildSyntheticScene(std::uint64_t pointCount);

} // namespace pci
