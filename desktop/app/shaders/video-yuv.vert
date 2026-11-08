#version 440
layout(location = 0) in vec4 vertex;
layout(location = 1) in vec2 texCoord;
layout(location = 0) out vec2 uv;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 yuvOffset;
    mat4 yuvMatrix;
};
void main() {
    uv = texCoord;
    gl_Position = qt_Matrix * vertex;
}
