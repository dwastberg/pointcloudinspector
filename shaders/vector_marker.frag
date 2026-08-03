#version 450
layout(std140, binding = 0) uniform VectorLayerData {
    mat4 mvp; vec4 fillColor; vec4 strokeColor; vec4 markerColor; vec2 viewportPixels;
    float strokeHalfWidthPixels; float markerHalfSizePixels; float opacity;
    int markerShape; float featherPixels; float nearPlaneW;
} layerData;
layout(location = 0) in vec2 localPixel;
layout(location = 0) out vec4 fragmentColor;
void main()
{
    float distance = layerData.markerShape == 0
        ? length(localPixel) : max(abs(localPixel.x), abs(localPixel.y));
    float feather = max(layerData.featherPixels, 1e-3);
    float outer = clamp((layerData.markerHalfSizePixels + layerData.strokeHalfWidthPixels - distance) / feather + 0.5, 0.0, 1.0);
    if (outer <= 0.0) discard;
    float inner = clamp((layerData.markerHalfSizePixels - distance) / feather + 0.5, 0.0, 1.0);
    vec4 color = mix(layerData.strokeColor, layerData.markerColor, inner);
    float alpha = color.a * layerData.opacity * outer;
    fragmentColor = vec4(color.rgb * alpha, alpha);
}
