#version 450

// ─── ВХОДНЫЕ ДАННЫЕ (из vertex buffer) ─────────────────────────
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUV;

// ─── ВЫХОДНЫЕ ДАННЫЕ (во фрагментный шейдер) ───────────────────
layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragPos;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec2 fragUV;

// ─── UNIFORM-БУФЕР ─────────────────────────────────────────────
// Должен ТОЧНО совпадать по структуре с C++-структурой UBO.
layout(binding = 0) uniform UBO {
    mat4 mvp;
    mat4 model;

    vec4 camera_pos;
    vec4 light_direction;
    vec4 light_color;
    vec4 ambient_light;

    vec4 albedo;
    vec4 specular;
    vec4 material;        // x = shininess, y = use_texture

    mat4 light_view_proj; // ЛР4 (не используется в вершинном шейдере,
                          //      но нужен для совпадения layout)
    vec4 shadow_params;   // ЛР4
} ubo;

void main() {
    vec4 worldPos = ubo.model * vec4(inPosition, 1.0);
    fragPos = worldPos.xyz;

    fragNormal = mat3(ubo.model) * inNormal;

    gl_Position = ubo.mvp * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragUV = inUV;
}