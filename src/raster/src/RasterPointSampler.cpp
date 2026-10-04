#include <pci/raster/RasterPointSampler.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace pci {

std::optional<RasterInversePlacement> RasterInversePlacement::forMetadata(
    const RasterLayerMetadata &metadata) noexcept
{
    if (metadata.width == 0 || metadata.height == 0 ||
        !rasterAffineInvertible(metadata.geoTransform)) {
        return std::nullopt;
    }

    const auto &g = metadata.geoTransform;
    const double determinant = g[1] * g[5] - g[2] * g[4];

    RasterInversePlacement result;
    result.pixelOffset_ = (-g[0] * g[5] + g[3] * g[2]) / determinant;
    result.pixelFromWorldX_ = g[5] / determinant;
    result.pixelFromWorldY_ = -g[2] / determinant;
    result.lineOffset_ = (g[0] * g[4] - g[3] * g[1]) / determinant;
    result.lineFromWorldX_ = -g[4] / determinant;
    result.lineFromWorldY_ = g[1] / determinant;
    result.width_ = metadata.width;
    result.height_ = metadata.height;
    return result;
}

void RasterInversePlacement::toPixelEdge(const double worldX,
                                         const double worldY,
                                         double &pixel,
                                         double &line) const noexcept
{
    pixel =
        pixelOffset_ + worldX * pixelFromWorldX_ + worldY * pixelFromWorldY_;
    line = lineOffset_ + worldX * lineFromWorldX_ + worldY * lineFromWorldY_;
}

std::optional<RasterPixelAddress>
RasterInversePlacement::addressOfPixel(const double pixel,
                                       const double line) const noexcept
{
    // Positive comparisons reject NaN as well as negative coordinates.
    if (!(pixel >= 0.0 && pixel < static_cast<double>(width_)) ||
        !(line >= 0.0 && line < static_cast<double>(height_))) {
        return std::nullopt;
    }

    const auto baseX = static_cast<std::uint32_t>(std::floor(pixel));
    const auto baseY = static_cast<std::uint32_t>(std::floor(line));
    return RasterPixelAddress{
        .tile =
            RasterTileKey{
                .levelIndex = 0,
                .x = baseX >> 8U,
                .y = baseY >> 8U,
            },
        .innerX = static_cast<std::uint8_t>(baseX & 0xffU),
        .innerY = static_cast<std::uint8_t>(baseY & 0xffU),
    };
}

std::optional<RasterPixelAddress>
RasterInversePlacement::addressOf(const double worldX,
                                  const double worldY) const noexcept
{
    double pixel = std::numeric_limits<double>::quiet_NaN();
    double line = std::numeric_limits<double>::quiet_NaN();
    toPixelEdge(worldX, worldY, pixel, line);
    return addressOfPixel(pixel, line);
}

RasterInversePlacement::BlockPlacement
RasterInversePlacement::forBlock(const double originX,
                                 const double originY,
                                 const double scale) const noexcept
{
    BlockPlacement result;
    toPixelEdge(originX, originY, result.pixelAtOrigin, result.lineAtOrigin);
    result.pixelPerStepX = scale * pixelFromWorldX_;
    result.pixelPerStepY = scale * pixelFromWorldY_;
    result.linePerStepX = scale * lineFromWorldX_;
    result.linePerStepY = scale * lineFromWorldY_;
    return result;
}

std::optional<RasterPixelAddress>
RasterInversePlacement::addressOf(const BlockPlacement &block,
                                  const std::uint16_t quantizedX,
                                  const std::uint16_t quantizedY) const noexcept
{
    const double pixel = block.pixelAtOrigin +
                         static_cast<double>(quantizedX) * block.pixelPerStepX +
                         static_cast<double>(quantizedY) * block.pixelPerStepY;
    const double line = block.lineAtOrigin +
                        static_cast<double>(quantizedX) * block.linePerStepX +
                        static_cast<double>(quantizedY) * block.linePerStepY;
    return addressOfPixel(pixel, line);
}

RasterPointColor
rasterTexelToPointColor(const RasterTileData &tile,
                        const std::uint32_t byteOffset) noexcept
{
    if (byteOffset > tile.rgba.size() || tile.rgba.size() - byteOffset < 4) {
        return std::nullopt;
    }

    const auto channel = [&tile, byteOffset](const std::uint32_t index) {
        return std::to_integer<std::uint32_t>(tile.rgba[byteOffset + index]);
    };
    const std::uint32_t alpha = channel(3);
    if (alpha == 0) {
        return std::nullopt;
    }

    const auto unpremultiply = [alpha](const std::uint32_t value) {
        if (alpha == 255) {
            return value;
        }
        return std::min((value * 255U + alpha / 2U) / alpha, 255U);
    };
    const std::uint32_t red = unpremultiply(channel(0));
    const std::uint32_t green = unpremultiply(channel(1));
    const std::uint32_t blue = unpremultiply(channel(2));
    return red | (green << 8U) | (blue << 16U) | 0xff000000U;
}

} // namespace pci
