#include <pci/raster/RasterCacheKey.h>

#include <pci/foundation/Hash.h>

std::size_t std::hash<pci::RasterCacheKey>::operator()(
    const pci::RasterCacheKey &key) const noexcept
{
    std::size_t seed = std::hash<pci::RasterSourceId>{}(key.sourceId);
    seed =
        pci::hashCombine(seed, static_cast<std::size_t>(key.renderGeneration));
    seed = pci::hashCombine(seed, static_cast<std::size_t>(key.profile));
    return pci::hashCombine(seed, std::hash<pci::RasterTileKey>{}(key.tile));
}
