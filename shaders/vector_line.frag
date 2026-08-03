#version 450
layout(std140, binding = 0) uniform VectorLayerData {
    mat4 mvp; vec4 fillColor; vec4 strokeColor; vec4 markerColor; vec2 viewportPixels;
    float strokeHalfWidthPixels; float markerHalfSizePixels; float opacity;
    int markerShape; float featherPixels; float nearPlaneW;
} layerData;
layout(location = 0) in vec2 pixelPosition;
layout(location = 1) flat in vec4 pixelSegment;
layout(location = 0) out vec4 fragmentColor;
void main()
{
    vec2 span = pixelSegment.zw - pixelSegment.xy;
    float lengthSquared = dot(span, span);
    float t = lengthSquared > 1e-12 ? clamp(dot(pixelPosition - pixelSegment.xy, span) / lengthSquared, 0.0, 1.0) : 0.0;
    float distance = length(pixelPosition - (pixelSegment.xy + span * t));
    float coverage = clamp((layerData.strokeHalfWidthPixels - distance) / max(layerData.featherPixels, 1e-3) + 0.5, 0.0, 1.0);
    if (coverage <= 0.0) discard;
    float alpha = layerData.strokeColor.a * layerData.opacity * coverage;
    fragmentColor = vec4(layerData.strokeColor.rgb * alpha, alpha);
}
