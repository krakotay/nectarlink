#version 440
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 1) uniform sampler2D luma;
layout(binding = 2) uniform sampler2D chroma;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 yuvOffset;
    mat4 yuvMatrix;
};
void main() {
    vec2 c = texture(chroma, uv).rg;
    vec4 rgb = yuvMatrix * (vec4(texture(luma, uv).r, c, 1.0) + yuvOffset);
    color = vec4(clamp(rgb.rgb, 0.0, 1.0), 1.0) * qt_Opacity;
}
