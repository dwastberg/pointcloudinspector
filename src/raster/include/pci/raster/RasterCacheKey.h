#pragma once

#include <pci/raster/RasterTileSource.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace pci {

struct RasterCacheKey {
    RasterSourceId sourceId;
    // Bumped when a style change alters decoded pixels, so tiles decoded for
    // an obsolete range or ramp can never be mistaken for current ones.
    std::uint64_t renderGeneration = 0;
    RasterTileKey tile;
    RasterTilePayloadProfile profile = RasterTilePayloadProfile::ColorOnly;
    bool operator==(const RasterCacheKey &) const = default;
};

} // namespace pci

template <> struct std::hash<pci::RasterCacheKey> {
    [[nodiscard]] std::size_t
    operator()(const pci::RasterCacheKey &key) const noexcept;
};
