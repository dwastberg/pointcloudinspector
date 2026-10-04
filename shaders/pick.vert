#version 450

layout(location = 0) in uvec4 positionAttributes;

layout(std140, binding = 0) uniform BlockData {
    mat4 mvp;
    vec4 reserved;
    float pointSize;
    int colorSource;
    int reservedColorMap;
    float scalarOffset;
    float scalarStep;
    int idBase;
    uvec4 classificationMaskLow;
    uvec4 classificationMaskHigh;
} camera;

layout(location = 0) flat out uint pointId;

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
    pointId = uint(gl_VertexIndex + camera.idBase) + 1u;
    uint classification = positionAttributes.w & 0xffu;
    if (!classificationVisible(classification)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        gl_PointSize = 0.0;
    }
}
