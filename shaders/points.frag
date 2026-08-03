#version 450

layout(location = 0) in vec4 pointColor;
layout(location = 1) in vec2 pointColorMapUv;
layout(location = 2) in float pointColorMapWeight;

layout(binding = 1) uniform sampler2D pointColorMapAtlas;

layout(location = 0) out vec4 fragmentColor;

void main()
{
    fragmentColor = mix(
        pointColor,
        texture(pointColorMapAtlas, pointColorMapUv),
        pointColorMapWeight);
}
