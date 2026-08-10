#pragma once

#include "raster/RasterTileSource.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace pci {

// Band assignment resolved once during inspection, so no runtime read has to
// re-derive it or let GDAL choose an overview implicitly.
struct RasterBandSelection {
    RasterSampleKind sampleKind = RasterSampleKind::ContinuousColor;
    // One band for palette, grayscale, and scalar sources; three for RGB.
    std::vector<int> colorBands;
    int alphaBand = 0; // 0 when the source has no explicit alpha band
    bool usesDatasetMask = false;
    // Set when three or four same-type bands with Undefined interpretations
    // were assigned positionally. Reported to the user as a warning.
    bool positionalFallback = false;
    // Per color band; not-a-number when the band declares no nodata value.
    // Compared against the raw sample, before scale and offset are applied.
    std::vector<double> nodata;
    // Explicit per-band scale and offset, applied before the shared display
    // range is derived so that range and pixels agree on one domain.
    std::vector<double> scale;
    std::vector<double> offset;
    // Byte reads halve memory traffic and quarter scratch size against a
    // widened double read, which matters for the one bounded static read.
    bool byteColorBands = false;
    bool byteAlphaBand = true;
    // Full-scale value of the alpha band's type, so a 16-bit alpha is not
    // read as if every sample above 255 were fully opaque.
    double alphaMaximum = 255.0;
    // Expanded once at inspection time. A palette never reaches portable code:
    // neither channelCount nor rgbaBands can express "one index band plus a
    // lookup table", and the table belongs to the source rather than to the
    // user-adjustable display transform.
    std::vector<std::array<float, 4>> paletteTable;
};

// Defined in the implementation so no GDAL type reaches this header.
struct GdalRasterHandlePool;

// Owns the read-only handle pool and performs windowed 1:1 tile reads. It is
// the only place that turns GDAL bands into premultiplied RGBA.
class GdalRasterSource final : public RasterTileSource {
public:
    GdalRasterSource(std::filesystem::path sourcePath,
                     RasterLayerMetadata metadata,
                     RasterBandSelection selection,
                     std::uint32_t maximumHandles = 2);
    ~GdalRasterSource() override;

    [[nodiscard]] const RasterLayerMetadata &metadata() const noexcept override;

    [[nodiscard]] std::uint64_t
    readReservationBytes(const RasterTileRequest &request) const override;

    [[nodiscard]] RasterTileData readTile(const RasterTileRequest &request,
                                          std::stop_token stop) const override;

    // Number of GDAL RasterIO calls this source has issued. The bounded-read
    // acceptance tests assert against it, so a regression that starts scanning
    // the base image fails loudly instead of merely running slowly.
    [[nodiscard]] std::uint64_t readCount() const noexcept;

private:
    std::filesystem::path sourcePath_;
    RasterLayerMetadata metadata_;
    RasterBandSelection selection_;
    std::unique_ptr<GdalRasterHandlePool> handles_;
    mutable std::atomic<std::uint64_t> readCount_{0};
};

} // namespace pci
