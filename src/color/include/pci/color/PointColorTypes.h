#pragma once

namespace pci {

enum class PointColorSource : int {
    Rgb = 0,
    X = 1,
    Y = 2,
    Z = 3,
    Intensity = 4,
    Classification = 5,
    ReturnNumber = 6,
    NumberOfReturns = 7,
};

enum class PointColorMap : int {
    Rgb = 0,
    // Stable IDs retained for the CPT-backed palettes used as defaults and by
    // persisted selections. Their color data is not built into the program.
    Viridis = 2,
    Turbo = 3,
    LasClassification = 4,
    ReturnNumber = 5,
};

} // namespace pci
