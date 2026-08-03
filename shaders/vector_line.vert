#version 450
layout(location = 0) in vec4 segment;
layout(std140, binding = 0) uniform VectorLayerData {
    mat4 mvp; vec4 fillColor; vec4 strokeColor; vec4 markerColor;
    vec2 viewportPixels;
    float strokeHalfWidthPixels;
    float markerHalfSizePixels;
    float opacity;
    int markerShape;
    float featherPixels;
    float nearPlaneW;
} layerData;
layout(location = 0) out vec2 pixelPosition;
layout(location = 1) flat out vec4 pixelSegment;
void main()
{
    vec4 clip0 = layerData.mvp * vec4(segment.xy, 0.0, 1.0);
    vec4 clip1 = layerData.mvp * vec4(segment.zw, 0.0, 1.0);
    if (clip0.w < layerData.nearPlaneW && clip1.w < layerData.nearPlaneW) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0); pixelPosition = vec2(0.0); pixelSegment = vec4(0.0); return;
    }
    if (clip0.w < layerData.nearPlaneW) clip0 = mix(clip0, clip1, (layerData.nearPlaneW - clip0.w) / (clip1.w - clip0.w));
    else if (clip1.w < layerData.nearPlaneW) clip1 = mix(clip1, clip0, (layerData.nearPlaneW - clip1.w) / (clip0.w - clip1.w));
    vec3 ndc0 = clip0.xyz / clip0.w;
    vec3 ndc1 = clip1.xyz / clip1.w;
    vec2 halfViewport = layerData.viewportPixels * 0.5;
    vec2 p0 = ndc0.xy * halfViewport;
    vec2 p1 = ndc1.xy * halfViewport;
    vec2 delta = p1 - p0;
    float span = length(delta);
    vec2 direction = span > 1e-6 ? delta / span : vec2(1.0, 0.0);
    vec2 normal = vec2(-direction.y, direction.x);
    float pad = layerData.strokeHalfWidthPixels + layerData.featherPixels;
    bool atEnd = (gl_VertexIndex & 2) != 0;
    float side = (gl_VertexIndex & 1) != 0 ? 1.0 : -1.0;
    vec2 pixel = (atEnd ? p1 : p0) + direction * (atEnd ? pad : -pad) + normal * side * pad;
    gl_Position = vec4(pixel / halfViewport, atEnd ? ndc1.z : ndc0.z, 1.0);
    pixelPosition = pixel; pixelSegment = vec4(p0, p1);
}
