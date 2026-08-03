#version 450

layout(location = 0) in uvec4 positionAttributes;
layout(location = 1) in vec4 color;
layout(location = 2) in uint packedProperties;

layout(std140, binding = 0) uniform BlockData {
    mat4 mvp;
    vec4 reserved;
    float pointSize;
    int colorSource;
    int colorMap;
    float scalarOffset;
    float scalarStep;
    int idBase;
    uvec4 classificationMaskLow;
    uvec4 classificationMaskHigh;
} camera;

layout(location = 0) out vec4 pointColor;
layout(location = 1) out vec2 pointColorMapUv;
layout(location = 2) out float pointColorMapWeight;

const int ColorRgb = 0;
const int ColorX = 1;
const int ColorY = 2;
const int ColorZ = 3;
const int ColorIntensity = 4;
const int ColorClassification = 5;
const int ColorReturnNumber = 6;
const int ColorNumberOfReturns = 7;

float normalizedScalar(float localValue)
{
    return clamp(
        camera.scalarOffset + localValue * camera.scalarStep,
        0.0,
        1.0);
}

float continuousMapU(float normalizedValue)
{
    return 0.5 * camera.reserved.y
        + normalizedValue * camera.reserved.z;
}

float categoricalMapU(uint value)
{
    return (float(value) + 0.5) * camera.reserved.y;
}

void useContinuousMap(float localValue)
{
    pointColorMapUv = vec2(
        continuousMapU(normalizedScalar(localValue)),
        camera.reserved.x);
    pointColorMapWeight = 1.0;
}

void useCategoricalMap(uint value)
{
    pointColorMapUv = vec2(
        categoricalMapU(value), camera.reserved.x);
    pointColorMapWeight = 1.0;
}

bool classificationVisible(uint classification)
{
    uint wordIndex = classification >> 5u;
    uint word = wordIndex < 4u
        ? camera.classificationMaskLow[wordIndex]
        : camera.classificationMaskHigh[wordIndex - 4u];
    return (word & (1u << (classification & 31u))) != 0u;
}

void main()
{
    gl_Position = camera.mvp * vec4(vec3(positionAttributes.xyz), 1.0);
    gl_PointSize = camera.pointSize;
    pointColor = color;
    pointColorMapUv = vec2(0.0, camera.reserved.x);
    pointColorMapWeight = 0.0;
    uint classification = positionAttributes.w & 0xffu;
    if (!classificationVisible(classification)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        gl_PointSize = 0.0;
        return;
    }
    if (camera.colorSource == ColorX) {
        useContinuousMap(float(positionAttributes.x));
    } else if (camera.colorSource == ColorY) {
        useContinuousMap(float(positionAttributes.y));
    } else if (camera.colorSource == ColorZ) {
        useContinuousMap(float(positionAttributes.z));
    } else if (camera.colorSource == ColorIntensity) {
        useContinuousMap(float(packedProperties & 0xffffu));
    } else if (camera.colorSource == ColorClassification) {
        useCategoricalMap(classification);
    } else if (camera.colorSource == ColorReturnNumber) {
        useCategoricalMap((packedProperties >> 16u) & 0xffu);
    } else if (camera.colorSource == ColorNumberOfReturns) {
        useCategoricalMap((packedProperties >> 24u) & 0xffu);
    }
}
