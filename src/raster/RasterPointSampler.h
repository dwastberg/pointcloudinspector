#pragma once

#include "raster/RasterTileSource.h"

#include <cstdint>
#include <optional>

namespace pci {

struct RasterPixelAddress {
    RasterTileKey tile;
    std::uint8_t innerX = 0;
    std::uint8_t innerY = 0;

    bool operator==(const RasterPixelAddress &) const = default;
};

class RasterInversePlacement {
public:
    struct BlockPlacement {
        double pixelAtOrigin = 0.0;
        double lineAtOrigin = 0.0;
        double pixelPerStepX = 0.0;
        double pixelPerStepY = 0.0;
        double linePerStepX = 0.0;
        double linePerStepY = 0.0;
    };

    [[nodiscard]] static std::optional<RasterInversePlacement>
    forMetadata(const RasterLayerMetadata &metadata) noexcept;

    void toPixelEdge(double worldX,
                     double worldY,
                     double &pixel,
                     double &line) const noexcept;

    [[nodiscard]] std::optional<RasterPixelAddress>
    addressOf(double worldX, double worldY) const noexcept;

    [[nodiscard]] BlockPlacement
    forBlock(double originX, double originY, double scale) const noexcept;

    [[nodiscard]] std::optional<RasterPixelAddress>
    addressOf(const BlockPlacement &block,
              std::uint16_t quantizedX,
              std::uint16_t quantizedY) const noexcept;

private:
    [[nodiscard]] std::optional<RasterPixelAddress>
    addressOfPixel(double pixel, double line) const noexcept;

    double pixelOffset_ = 0.0;
    double pixelFromWorldX_ = 0.0;
    double pixelFromWorldY_ = 0.0;
    double lineOffset_ = 0.0;
    double lineFromWorldX_ = 0.0;
    double lineFromWorldY_ = 0.0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

using RasterPointColor = std::optional<std::uint32_t>;

[[nodiscard]] RasterPointColor
rasterTexelToPointColor(const RasterTileData &tile,
                        std::uint32_t byteOffset) noexcept;

} // namespace pci
