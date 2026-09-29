#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform UBO {
    mat4 mvp;
    vec4 color;
} ubo;

void main() {
    outColor = vec4(ubo.color.rgb * fragColor, 1.0);
}