#version 450

layout(std140, binding = 0) uniform EyeDomeLightingData {
    vec2 inverseViewport;
    float radius;
    float strength;
    float nearPlane;
    float farPlane;
} edl;

layout(binding = 1) uniform sampler2D sceneColor;
layout(binding = 2) uniform sampler2D sceneDepth;

layout(location = 0) out vec4 fragmentColor;

const vec2 neighbours[8] = vec2[](
    vec2(1.0, 0.0),
    vec2(0.70710678, 0.70710678),
    vec2(0.0, 1.0),
    vec2(-0.70710678, 0.70710678),
    vec2(-1.0, 0.0),
    vec2(-0.70710678, -0.70710678),
    vec2(0.0, -1.0),
    vec2(0.70710678, -0.70710678));

float viewDepth(float normalizedDepth)
{
    return (edl.nearPlane * edl.farPlane)
        / (edl.farPlane
           - normalizedDepth * (edl.farPlane - edl.nearPlane));
}

void main()
{
    vec2 uv = gl_FragCoord.xy * edl.inverseViewport;
    vec4 color = texture(sceneColor, uv);
    float centerDepth = texture(sceneDepth, uv).r;
    // Preserve the point pass depth for every composite fragment. This must
    // happen before the background early return as well.
    gl_FragDepth = centerDepth;

    if (centerDepth >= 1.0) {
        fragmentColor = color;
        return;
    }

    float centerLogDepth = log2(viewDepth(centerDepth));
    float response = 0.0;
    for (int index = 0; index < 8; ++index) {
        vec2 neighbourUv = uv
            + neighbours[index] * edl.radius * edl.inverseViewport;
        float neighbourDepth = texture(sceneDepth, neighbourUv).r;
        if (neighbourDepth < 1.0) {
            response += max(
                0.0,
                centerLogDepth
                    - log2(viewDepth(neighbourDepth)));
        }
    }

    response /= 8.0;
    float shade = exp(-edl.strength * response);
    fragmentColor = vec4(color.rgb * shade, color.a);
}
