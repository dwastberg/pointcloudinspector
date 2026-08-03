#version 450
layout(location = 0) in vec2 position;
layout(std140, binding = 0) uniform VectorLayerData {
    mat4 mvp; vec4 fillColor; vec4 strokeColor; vec4 markerColor;
    vec2 viewportPixels; float strokeHalfWidthPixels; float markerHalfSizePixels;
    float opacity; int markerShape; float featherPixels; float nearPlaneW;
} layerData;
void main() { gl_Position = layerData.mvp * vec4(position, 0.0, 1.0); }
