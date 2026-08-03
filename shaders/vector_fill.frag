#version 450
layout(location = 0) out vec4 fragmentColor;
layout(std140, binding = 0) uniform VectorLayerData {
    mat4 mvp; vec4 fillColor; vec4 strokeColor; vec4 markerColor;
    vec2 viewportPixels; float strokeHalfWidthPixels; float markerHalfSizePixels;
    float opacity; int markerShape; float featherPixels; float nearPlaneW;
} layerData;
void main() { float alpha = layerData.fillColor.a * layerData.opacity; fragmentColor = vec4(layerData.fillColor.rgb * alpha, alpha); }
