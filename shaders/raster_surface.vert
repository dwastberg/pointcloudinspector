#version 450

layout(location = 0) out vec2 uv;
layout(location = 1) out float vertexValidity;
layout(location = 2) out vec3 eyePosition;

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

float heightAt(ivec2 position)
{
    return texelFetch(elevation, position, 0).r;
}

float alphaAt(ivec2 position)
{
    return texelFetch(imagery, position, 0).a;
}

void main()
{
    int verticesPerSide = int(tile.shadingParams.z) + 1;
    ivec2 grid = ivec2(gl_VertexIndex % verticesPerSide,
                       gl_VertexIndex / verticesPerSide);
    vec2 unitPosition = vec2(grid) / tile.shadingParams.z;

    // Pixel-edge reconstruction. At a shared edge the two neighbouring tiles
    // fetch the same two real pixels (one through each tile's gutter) and mix
    // them at the same weight, closing same-LOD seams bit-exactly.
    vec2 samplePosition =
        vec2(tile.tileTexels.w) + unitPosition * tile.tileTexels.xy - 0.5;
    ivec2 base = ivec2(floor(samplePosition));
    vec2 weight = fract(samplePosition);
    ivec2 maximumTexel = ivec2(tile.tileTexels.z) - ivec2(1);
    ivec2 p00 = clamp(base, ivec2(0), maximumTexel);
    ivec2 p10 = clamp(base + ivec2(1, 0), ivec2(0), maximumTexel);
    ivec2 p01 = clamp(base + ivec2(0, 1), ivec2(0), maximumTexel);
    ivec2 p11 = clamp(base + ivec2(1, 1), ivec2(0), maximumTexel);
    float top = mix(heightAt(p00), heightAt(p10), weight.x);
    float bottom = mix(heightAt(p01), heightAt(p11), weight.x);
    float residual = mix(top, bottom, weight.y);

    float cutoff = tile.heightParams.z;
    vertexValidity = min(min(alphaAt(p00), alphaAt(p10)),
                         min(alphaAt(p01), alphaAt(p11))) > cutoff
                         ? 1.0
                         : 0.0;
    uv = mix(tile.uvRect.xy, tile.uvRect.zw, unitPosition);
    eyePosition = tile.origin.xyz + tile.edgeU.xyz * unitPosition.x +
                  tile.edgeV.xyz * unitPosition.y;
    eyePosition.z += residual * tile.heightParams.x;
    gl_Position = tile.viewProjection * vec4(eyePosition, 1.0);
}
