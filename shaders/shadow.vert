#version 450

// ─── ВХОДНЫЕ ДАННЫЕ ────────────────────────────────────────────
// Теневому шейдеру нужна ТОЛЬКО позиция вершины.
layout(location = 0) in vec3 inPosition;

// ─── UNIFORM-БУФЕР ─────────────────────────────────────────────
// Должен ТОЧНО совпадать с C++-структурой ShadowUBO:
//   struct ShadowUBO {
//       glm::mat4 light_view_proj; // 64 байта
//       glm::mat4 model;           // 64 байта
//   };
layout(binding = 0) uniform ShadowUBO {
    mat4 light_view_proj;
    mat4 model;
} shadow_ubo;

void main() {
    // Просто проецируем вершину в clip-space света.
    // Никаких нормалей/UV/цветов — только позиция.
    gl_Position = shadow_ubo.light_view_proj * shadow_ubo.model * vec4(inPosition, 1.0);
}