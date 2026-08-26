#version 450

layout(location = 0) in vec2 uv;
layout(location = 1) in float vertexValidity;
layout(location = 2) in vec3 eyePosition;

layout(location = 0) out vec4 fragmentColor;

layout(std140, binding = 0) uniform RasterSurfaceData
{
    mat4 viewProjection;
    vec4 origin;
    vec4 edgeU;
    vec4 edgeV;
    vec4 uvRect;
    vec4 tileTexels;
    vec4 heightParams;
    vec4 shadingParams;
}
tile;

layout(binding = 1) uniform sampler2D imagery;
layout(binding = 2) uniform sampler2D elevation;

void main()
{
    vec4 premultiplied = texture(imagery, uv);
    if (vertexValidity < 0.999 ||
        premultiplied.a <= tile.heightParams.z) {
        discard;
    }

    int storedPixels = int(tile.tileTexels.z);
    ivec2 center = clamp(ivec2(floor(uv * tile.tileTexels.z)),
                         ivec2(1), ivec2(storedPixels - 2));
    float left = texelFetch(elevation, center - ivec2(1, 0), 0).r;
    float right = texelFetch(elevation, center + ivec2(1, 0), 0).r;
    float top = texelFetch(elevation, center - ivec2(0, 1), 0).r;
    float bottom = texelFetch(elevation, center + ivec2(0, 1), 0).r;

    vec3 tangentU = 2.0 * tile.edgeU.xyz / tile.tileTexels.x;
    vec3 tangentV = 2.0 * tile.edgeV.xyz / tile.tileTexels.y;
    tangentU.z += (right - left) * tile.heightParams.x;
    tangentV.z += (bottom - top) * tile.heightParams.x;
    vec3 normal = normalize(cross(tangentU, tangentV));
    if (normal.z < 0.0) {
        normal = -normal;
    }
    vec3 toEye = normalize(-eyePosition);
    float lighting = tile.shadingParams.x +
                     tile.shadingParams.y * max(dot(normal, toEye), 0.0);
    float shade = mix(1.0, lighting, tile.heightParams.w);
    premultiplied.rgb *= shade;
    fragmentColor = premultiplied * tile.heightParams.y;
}
