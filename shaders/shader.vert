#version 450

// ─── ВХОДНЫЕ ДАННЫЕ (из vertex buffer) ─────────────────────────
// location 0 — позиция вершины (локальные координаты модели)
// location 1 — цвет вершины (в Блинн-Фонге почти не используется,
//              но оставим для совместимости)
// location 2 — НОРМАЛЬ поверхности (новое в лабе №2)
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;

// ─── ВЫХОДНЫЕ ДАННЫЕ (во фрагментный шейдер) ───────────────────
// Передаём в фрагментный шейдер мировые координаты фрагмента
// и мировую нормаль — они нужны для расчёта Блинн-Фонга.
layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragPos;
layout(location = 2) out vec3 fragNormal;

// ─── UNIFORM-БУФЕР ─────────────────────────────────────────────
// Должен ТОЧНО совпадать по структуре с C++-структурой UBO.
// В std140 матрицы mat4 занимают 64 байта, vec4 — 16 байт.
layout(binding = 0) uniform UBO {
    mat4 mvp;
    mat4 model;

    vec4 camera_pos;
    vec4 light_direction;
    vec4 light_color;
    vec4 ambient_light;

    vec4 albedo;
    vec4 specular;
    vec4 material;   // material.x = shininess
} ubo;

void main() {
    // Локальная позиция → мировая.
    // Из лекции №4 (слайд 4): model преобразует локальные координаты
    // в мировые. Нам нужна МИРОВАЯ позиция фрагмента, чтобы вычислить
    // направления к камере и свету.
    vec4 worldPos = ubo.model * vec4(inPosition, 1.0);
    fragPos = worldPos.xyz;

    // Нормаль тоже переводим в мир. Для этого берём ВЕРХНИЙ ЛЕВЫЙ
    // блок 3×3 матрицы model (без переноса). Если масштаб равномерный,
    // этого достаточно. Если бы масштаб был неравномерным, понадобилась
    // бы обратная транспонированная матрица (normal matrix).
    //
    // Из лекции №4: нормали нельзя трансформировать как обычные точки —
    // они должны преобразовываться только поворотом/масштабом, без переноса.
    fragNormal = mat3(ubo.model) * inNormal;

    // Итоговая позиция вершины в NDC.
    gl_Position = ubo.mvp * vec4(inPosition, 1.0);
    fragColor = inColor;
}