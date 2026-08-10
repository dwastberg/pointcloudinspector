#version 450

layout(location = 0) out vec2 uv;

layout(std140, binding = 0) uniform RasterTileData
{
    vec4 clipTopLeft;
    vec4 clipTopRight;
    vec4 clipBottomLeft;
    vec4 clipBottomRight;
    // Inner extent of the stored image, excluding the replicated gutter.
    vec4 uvRect;
    float opacity;
}
tile;

void main()
{
    // A four-vertex strip needs no GPU buffer. Deriving the unit coordinates
    // from the vertex index also keeps the vertex slot namespace independent
    // of QRhi's native uniform/texture bindings on every backend.
    const vec2 unitPositions[4] = vec2[4](
        vec2(0.0, 0.0), vec2(1.0, 0.0),
        vec2(0.0, 1.0), vec2(1.0, 1.0));
    vec2 unitPosition = unitPositions[gl_VertexIndex];
    uv = mix(tile.uvRect.xy, tile.uvRect.zw, unitPosition);
    vec4 clipTop = mix(tile.clipTopLeft, tile.clipTopRight, unitPosition.x);
    vec4 clipBottom = mix(
        tile.clipBottomLeft, tile.clipBottomRight, unitPosition.x);
    gl_Position = mix(clipTop, clipBottom, unitPosition.y);
}
