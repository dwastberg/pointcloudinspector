#version 450
layout(location = 0) in vec2 position;
layout(std140, binding = 0) uniform VectorLayerData {
    mat4 mvp; vec4 fillColor; vec4 strokeColor; vec4 markerColor; vec2 viewportPixels;
    float strokeHalfWidthPixels; float markerHalfSizePixels; float opacity;
    int markerShape; float featherPixels; float nearPlaneW;
} layerData;
layout(location = 0) out vec2 localPixel;
void main()
{
    vec4 clip = layerData.mvp * vec4(position, 0.0, 1.0);
    if (clip.w < layerData.nearPlaneW) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0); localPixel = vec2(0.0); return;
    }
    vec3 ndc = clip.xyz / clip.w;
    vec2 halfViewport = layerData.viewportPixels * 0.5;
    float radius = layerData.markerHalfSizePixels + layerData.strokeHalfWidthPixels + layerData.featherPixels;
    vec2 corner = vec2((gl_VertexIndex & 1) != 0 ? 1.0 : -1.0,
                       (gl_VertexIndex & 2) != 0 ? 1.0 : -1.0);
    localPixel = corner * radius;
    gl_Position = vec4(ndc.xy + localPixel / halfViewport, ndc.z, 1.0);
}
