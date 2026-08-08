#version 450

// The quad is the unit square in level pixel-edge space. Its world placement,
// including any rotation or skew from the source geotransform, is folded into
// mvp on the CPU in double precision before narrowing.
layout(location = 0) in vec2 unitPosition;

layout(location = 0) out vec2 uv;

layout(std140, binding = 0) uniform RasterTileData
{
    mat4 mvp;
    // Inner extent of the stored image, excluding the replicated gutter.
    vec4 uvRect;
    float opacity;
}
tile;

void main()
{
    uv = mix(tile.uvRect.xy, tile.uvRect.zw, unitPosition);
    gl_Position = tile.mvp * vec4(unitPosition, 0.0, 1.0);
}
