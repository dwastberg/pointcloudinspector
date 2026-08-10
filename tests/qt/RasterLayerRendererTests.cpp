#include "renderer/rhi/RasterLayerRenderer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

namespace {

[[nodiscard]] pci::RasterLayerMetadata northUpMetadata()
{
    pci::RasterLayerMetadata metadata;
    metadata.width = 64;
    metadata.height = 32;
    // Two-metre pixels, north up, origin at a large projected coordinate.
    metadata.geoTransform = {674000.0, 2.0, 0.0, 6580000.0, 0.0, -2.0};
    metadata.bounds = *pci::rasterPixelEdgeBounds(
        metadata.geoTransform, metadata.width, metadata.height);
    return metadata;
}

TEST_CASE("raster uniform block matches its std140 layout", "[qt][raster]")
{
    // The block is padded identically in C++ and GLSL; the static assertions
    // in the header are the contract and this pins the size a shader change
    // would otherwise break silently.
    CHECK(sizeof(pci::RasterLayerUniform) == 96);
    CHECK(alignof(pci::RasterLayerUniform) == 16);
    CHECK(offsetof(pci::RasterLayerUniform, clipTopLeft) == 0);
    CHECK(offsetof(pci::RasterLayerUniform, clipTopRight) == 16);
    CHECK(offsetof(pci::RasterLayerUniform, clipBottomLeft) == 32);
    CHECK(offsetof(pci::RasterLayerUniform, clipBottomRight) == 48);
    CHECK(offsetof(pci::RasterLayerUniform, uvRect) == 64);
    CHECK(offsetof(pci::RasterLayerUniform, opacity) == 80);
}

TEST_CASE("raster uniform staging honours the device stride", "[qt][raster]")
{
    std::vector<pci::RasterLayerDraw> draws(3);
    draws[0].uniform.opacity = 0.25F;
    draws[1].uniform.opacity = 0.5F;
    draws[2].uniform.opacity = 0.75F;

    constexpr std::size_t stride = 256;
    const std::vector<std::byte> staging =
        pci::stageRasterLayerUniforms(draws, stride);
    REQUIRE(staging.size() == draws.size() * stride);

    for (std::size_t index = 0; index < draws.size(); ++index) {
        pci::RasterLayerUniform copy;
        std::memcpy(&copy,
                    staging.data() + index * stride,
                    sizeof(pci::RasterLayerUniform));
        CHECK(copy.opacity == draws[index].uniform.opacity);
    }

    // A stride smaller than the block would overlap draws in the buffer.
    CHECK_THROWS_AS(pci::stageRasterLayerUniforms(draws, 8),
                    std::invalid_argument);
}

TEST_CASE("raster quad transform maps the unit square onto pixel edges",
          "[qt][raster]")
{
    const pci::RasterLayerMetadata metadata = northUpMetadata();
    pci::RasterLayerStyle style;
    const pci::Vec3d eye{674000.0, 6580000.0, 100.0};

    const pci::RasterQuadTransform quad =
        pci::rasterLayerQuadTransform(metadata, style, eye);

    // Unit (0,0) is the top-left pixel edge, expressed relative to the eye.
    CHECK(quad.origin.x == Catch::Approx(0.0));
    CHECK(quad.origin.y == Catch::Approx(0.0));
    CHECK(quad.origin.z == Catch::Approx(-100.0));

    // 64 columns of two metres and 32 rows of two metres, using edges rather
    // than centers.
    CHECK(quad.edgeU.x == Catch::Approx(128.0));
    CHECK(quad.edgeU.y == Catch::Approx(0.0));
    CHECK(quad.edgeV.x == Catch::Approx(0.0));
    CHECK(quad.edgeV.y == Catch::Approx(-64.0));

    // The quad's far corner coincides with the metadata bounds, so geometry
    // and scene fitting cannot disagree.
    CHECK(quad.origin.x + eye.x + quad.edgeU.x ==
          Catch::Approx(metadata.bounds.maximum[0]));
    CHECK(quad.origin.y + eye.y + quad.edgeV.y ==
          Catch::Approx(metadata.bounds.minimum[1]));
}

TEST_CASE("raster quad transform follows the styled elevation", "[qt][raster]")
{
    const pci::RasterLayerMetadata metadata = northUpMetadata();
    pci::RasterLayerStyle style;
    style.zOffset = 42.0;

    const pci::RasterQuadTransform quad =
        pci::rasterLayerQuadTransform(metadata, style, pci::Vec3d{});
    // The quad draws at exactly the configured Z, even though the layer's
    // scene bounds are inflated around it.
    CHECK(quad.origin.z == Catch::Approx(42.0));
    CHECK(pci::rasterSceneBounds(metadata, style).minimum[2] ==
          Catch::Approx(41.5));
}

TEST_CASE("raster quad transform carries rotation and skew", "[qt][raster]")
{
    pci::RasterLayerMetadata metadata;
    metadata.width = 10;
    metadata.height = 10;
    metadata.geoTransform = {0.0, 1.732, -0.5, 0.0, 1.0, -0.866};

    const pci::RasterQuadTransform quad =
        pci::rasterLayerQuadTransform(metadata, {}, pci::Vec3d{});
    // A rotated transform gives the edges a component on both axes, which is
    // why the quad needs a full matrix rather than a translate and scale.
    CHECK(quad.edgeU.x == Catch::Approx(17.32));
    CHECK(quad.edgeU.y == Catch::Approx(10.0));
    CHECK(quad.edgeV.x == Catch::Approx(-5.0));
    CHECK(quad.edgeV.y == Catch::Approx(-8.66));
}

TEST_CASE("raster culling rejects layers outside the frustum", "[qt][raster]")
{
    const pci::RasterLayerMetadata metadata = northUpMetadata();

    pci::FrameCamera frame;
    frame.eye = {674064.0, 6579968.0, 500.0};
    frame.forward = {0.0, 0.0, -1.0};
    frame.up = {0.0, 1.0, 0.0};
    frame.right = {1.0, 0.0, 0.0};
    frame.outputWidth = 800;
    frame.outputHeight = 600;
    frame.nearPlane = 1.0;
    frame.farPlane = 10000.0;
    frame.orthographic = true;
    frame.orthographicScale = 400.0;
    frame.culler =
        pci::FrustumCuller::fromOrthographic(frame.eye,
                                             frame.forward,
                                             frame.up,
                                             frame.right,
                                             frame.orthographicScale * 0.5,
                                             800.0 / 600.0,
                                             frame.nearPlane,
                                             frame.farPlane);

    CHECK(pci::rasterLayerCullBounds(metadata, {}, frame).has_value());

    // A layer placed far outside the frustum is culled before any GPU work.
    pci::RasterLayerMetadata distant = metadata;
    distant.geoTransform[0] += 1.0e6;
    distant.bounds = *pci::rasterPixelEdgeBounds(
        distant.geoTransform, distant.width, distant.height);
    CHECK_FALSE(pci::rasterLayerCullBounds(distant, {}, frame).has_value());
}

} // namespace
TEST_CASE("raster tile quad lands on the tile's base-pixel rect",
          "[qt][raster]")
{
    pci::RasterLayerMetadata metadata;
    metadata.width = 512;
    metadata.height = 512;
    metadata.geoTransform = {1000.0, 2.0, 0.0, 5000.0, 0.0, -2.0};
    pci::RasterLevel base;
    base.width = 512;
    base.height = 512;
    base.channelCount = 3;
    metadata.levels.push_back(base);

    // Tile (1,0) starts at base pixel 256, which is 512 metres in.
    const pci::RasterQuadTransform quad = pci::rasterTileQuadTransform(
        metadata, {}, pci::RasterTileKey{0, 1, 0}, pci::Vec3d{});
    CHECK(quad.origin.x == Catch::Approx(1000.0 + 512.0));
    CHECK(quad.origin.y == Catch::Approx(5000.0));
    CHECK(quad.edgeU.x == Catch::Approx(512.0));
    CHECK(quad.edgeV.y == Catch::Approx(-512.0));

    // Adjacent tiles share an exact edge, so no seam opens between them.
    const pci::RasterQuadTransform left = pci::rasterTileQuadTransform(
        metadata, {}, pci::RasterTileKey{0, 0, 0}, pci::Vec3d{});
    CHECK(left.origin.x + left.edgeU.x == Catch::Approx(quad.origin.x));

    // A key outside the level yields a degenerate quad rather than garbage.
    const pci::RasterQuadTransform outside = pci::rasterTileQuadTransform(
        metadata, {}, pci::RasterTileKey{9, 0, 0}, pci::Vec3d{});
    CHECK(outside.edgeU.x == 0.0);
}

TEST_CASE("raster tile UVs exclude the replicated gutter", "[qt][raster]")
{
    const std::array<float, 4> full = pci::rasterTileUvRect(256, 256);
    // The interior starts one texel in and ends one texel from the far edge,
    // so the sampler never reaches a neighbouring tile's texels.
    CHECK(full[0] == Catch::Approx(1.0F / 258.0F));
    CHECK(full[1] == Catch::Approx(1.0F / 258.0F));
    CHECK(full[2] == Catch::Approx(257.0F / 258.0F));
    CHECK(full[3] == Catch::Approx(257.0F / 258.0F));

    // An edge tile covers only its own valid extent; assuming a full tile
    // would sample the replicated gutter as if it were image content.
    const std::array<float, 4> edge = pci::rasterTileUvRect(100, 40);
    CHECK(edge[2] == Catch::Approx(101.0F / 258.0F));
    CHECK(edge[3] == Catch::Approx(41.0F / 258.0F));
    CHECK(edge[0] == full[0]);
}
