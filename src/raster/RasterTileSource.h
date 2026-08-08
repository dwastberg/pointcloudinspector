#pragma once

#include "raster/RasterLayer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace pci {

// Every decoded tile is one guttered, premultiplied RGBA8 buffer of exactly
// this size, so cache accounting and upload sizing never depend on the source.
inline constexpr std::size_t rasterStoredTileBytes =
    static_cast<std::size_t>(rasterStoredTilePixels) * rasterStoredTilePixels *
    4U;

// A tile read that could not produce valid pixels. The worker wrapper converts
// this into a negative-cache entry.
class RasterReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A tile read abandoned because its stop token was requested. Cancellation is
// not a failure: it must never enter the negative cache, or a cancelled pan
// would poison the tiles the next frame needs.
class RasterReadCancelled : public std::exception {
public:
    [[nodiscard]] const char *what() const noexcept override
    {
        return "Raster tile read was cancelled";
    }
};

struct RasterTileRequest {
    RasterTileKey key;
    std::uint64_t renderGeneration = 0;
    std::shared_ptr<const RasterDecodeParameters> decode;
};

struct RasterTileData {
    RasterTileKey key;
    std::uint64_t renderGeneration = 0;
    std::uint16_t validWidth = 0; // inner pixels, excluding the gutter
    std::uint16_t validHeight = 0;
    std::vector<std::byte> rgba; // always rasterStoredTileBytes

    // Accounted rather than logical size: the cache reserves what the
    // allocation actually holds, not what the pixels logically occupy.
    [[nodiscard]] std::uint64_t byteSize() const noexcept
    {
        return sizeof(RasterTileData) +
               static_cast<std::uint64_t>(rgba.capacity());
    }
};

// One bounded, already-decimated image for phase 1's single static texture.
// Phase 2's tiled path supersedes it; runtime tile reads are always 1:1.
struct RasterStaticImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levelIndex = 0;
    // True when the image came from a decimating base-band read rather than
    // from an explicitly backed overview.
    bool decimatedBase = false;
    std::vector<std::byte> rgba; // premultiplied, width * height * 4
};

// The only contract through which non-GDAL code reads pixels. It names no Qt,
// GDAL, or QRhi type.
class RasterTileSource {
public:
    RasterTileSource() = default;
    virtual ~RasterTileSource() = default;
    RasterTileSource(const RasterTileSource &) = delete;
    RasterTileSource &operator=(const RasterTileSource &) = delete;
    RasterTileSource(RasterTileSource &&) = delete;
    RasterTileSource &operator=(RasterTileSource &&) = delete;

    [[nodiscard]] virtual const RasterLayerMetadata &
    metadata() const noexcept = 0;

    // Throws RasterReadError on failure and RasterReadCancelled when the stop
    // token is requested. It never returns a partially valid tile, so no
    // caller can mistake a zeroed buffer for transparent imagery.
    [[nodiscard]] virtual RasterTileData readTile(const RasterTileRequest &,
                                                  std::stop_token) const = 0;

    // Reads the finest backed level that fits maximumTexturePixels, or one
    // decimating base-band read when the base is below
    // rasterBoundedBaseReadPixels. Throws RasterReadError when neither
    // applies, so a source too large for either path reports "tiled rendering
    // required" instead of scanning its base image.
    [[nodiscard]] virtual RasterStaticImage
    readStaticImage(std::uint32_t /*maximumTexturePixels*/,
                    const RasterDecodeParameters &,
                    std::stop_token) const
    {
        throw RasterReadError("This source cannot produce a static image");
    }
};

using RasterTileSourcePtr = std::shared_ptr<const RasterTileSource>;

struct RasterLayerData {
    RasterSourceId sourceId;
    RasterTileSourcePtr source;

    [[nodiscard]] const RasterLayerMetadata &metadata() const noexcept
    {
        return source->metadata();
    }
};

using RasterLayerDataPtr = std::shared_ptr<const RasterLayerData>;

// Distinguishes cache ownership without making the portable raster target
// depend on SceneLayerId, which would introduce a scene/raster target cycle.
[[nodiscard]] inline RasterSourceId nextRasterSourceId()
{
    static std::atomic<std::uint64_t> next{1};
    const std::uint64_t value = next.fetch_add(1, std::memory_order_relaxed);
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Raster source identifiers are exhausted");
    }
    return RasterSourceId{value};
}

} // namespace pci
