#version 450

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 fragmentColor;

layout(std140, binding = 0) uniform RasterTileData
{
    vec4 clipTopLeft;
    vec4 clipTopRight;
    vec4 clipBottomLeft;
    vec4 clipBottomRight;
    vec4 uvRect;
    float opacity;
}
tile;

layout(binding = 1) uniform sampler2D imagery;

void main()
{
    // The texture already holds premultiplied alpha, composed on the CPU
    // before upload so the sampler never blends an invalid source color into a
    // valid neighbour. Layer opacity scales the premultiplied result, which is
    // why it is a uniform rather than part of the decode.
    vec4 premultiplied = texture(imagery, uv);
    fragmentColor = premultiplied * tile.opacity;
}
