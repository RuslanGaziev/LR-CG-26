#include "application.hpp"

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <fstream>
#include <vector>
#include <cstring>
#include <iostream>
#include <cmath>

namespace application {

    namespace {

        // ============================================================
        // ВЕРШИНА
        // ============================================================
        // Из лекции №4: для освещения по Блинн-Фонгу нам нужна
        // НОРМАЛЬ поверхности в каждой вершине. Без неё невозможно
        // вычислить cos(θ) — угол между нормалью и направлением света.
        // Поэтому в структуру добавляется третье поле: normal.
        struct Vertex {
            glm::vec3 pos;
            glm::vec3 color;
            glm::vec3 normal;   // ← НОВОЕ: нормаль поверхности
        };

        // ============================================================
        // UNIFORM-БУФЕР
        // ============================================================
        // Из лекции №4 (слайд 29, формула Блинн-Фонга):
        //   C = I·(cosθ·K_d + K_s·(cosφ)^α) + I_a·K_a
        //
        // Чтобы шейдер мог посчитать эту формулу, ему нужно:
        //   • MVP для позиции вершины
        //   • model — чтобы перевести нормаль в мировые координаты
        //   • camera_pos — вектор v (от фрагмента к камере)
        //   • light_direction — вектор ω (от фрагмента к свету)
        //   • light_color — интенсивность I источника
        //   • ambient_light — I_a (рассеянный свет)
        //   • albedo — K_d (базовый цвет материала)
        //   • specular — K_s (цвет блика)
        //   • shininess — α (коэффициент блеска)
        //
        // ВАЖНО: в uniform-буфере с layout std140 поля vec3 выравниваются
        // по границе 16 байт (как vec4). Поэтому используем vec4 для
        // всего, что связано с цветом/направлением — так проще и не будет
        // «съезда» данных.
        struct UBO {
            glm::mat4 mvp;             // 64 байта
            glm::mat4 model;           // 64 байта

            glm::vec4 camera_pos;      // xyz = позиция, w = не используется
            glm::vec4 light_direction; // xyz = направление (мировое), w = 0
            glm::vec4 light_color;     // xyz = цвет × интенсивность, w = 1
            glm::vec4 ambient_light;   // xyz = цвет ambient, w = 1

            glm::vec4 albedo;          // K_d (цвет материала)
            glm::vec4 specular;        // K_s (цвет блика)
            glm::vec4 material;        // x = shininess (α), остальное — паддинг
        };

        // ============================================================
        // СОСТОЯНИЕ UI — ГЕОМЕТРИЯ МОДЕЛИ (из лабы №1)
        // ============================================================
        glm::vec3 g_position(0.0f);
        glm::vec3 g_rotation(30.0f, 45.0f, 0.0f);
        glm::vec3 g_scale(1.0f);
        int       g_projectionMode = 1;

        bool  g_animPlaying = true;
        float g_animSpeed = 0.1f;    // медленно, чтобы не «трясло»
        float g_animRadius = 2.0f;
        float g_animTime = 0.0f;

        // ============================================================
        // СОСТОЯНИЕ UI — КАМЕРА (НОВОЕ в лабе №2)
        // ============================================================
        // Из лекции №4 (слайды 5–9): камера — это точка + ориентация.
        // Матрица вида V — это ОБРАТНАЯ матрица камеры.
        // Мы параметризуем камеру через:
        //   • position — где стоит глаз
        //   • yaw/pitch — куда смотрит (углы Эйлера)
        //   • fov — угол обзора (для перспективной проекции)
        glm::vec3 g_cameraPos(0.0f, 2.0f, 6.0f);
        float     g_cameraYaw   = -90.0f;  // -90° = смотрим вдоль -Z (в центр)
        float     g_cameraPitch = -15.0f;  // слегка вниз
        float     g_cameraFov   = 45.0f;

        // ============================================================
        // СОСТОЯНИЕ UI — ИСТОЧНИК СВЕТА (НОВОЕ)
        // ============================================================
        // Из лекции №4 (слайд 14): направленный источник (directional)
        // моделируется как бесконечно удалённый — все лучи параллельны.
        // Поэтому у него нет позиции, только НАПРАВЛЕНИЕ и цвет+интенсивность.
        glm::vec3 g_lightDirection(-0.5f, -1.0f, -0.3f);
        glm::vec3 g_lightColor(1.0f, 1.0f, 1.0f);
        float     g_lightIntensity = 1.5f;

        // ============================================================
        // СОСТОЯНИЕ UI — AMBIENT (рассеянный свет)
        // ============================================================
        // Из лекции №4 (слайд 25): ambient аппроксимирует свет,
        // который приходит отовсюду из окружения. У него нет направления,
        // он равномерно освещает все поверхности. Грубо, но без него
        // теневые стороны становятся полностью чёрными.
        glm::vec3 g_ambientColor(0.15f, 0.15f, 0.18f);

        // ============================================================
        // СОСТОЯНИЕ UI — МАТЕРИАЛ (НОВОЕ)
        // ============================================================
        // Из лекции №4 (слайды 21–24):
        //   K_d — альбедо (базовый цвет материала)
        //   K_s — цвет блика (обычно белый или близкий к нему)
        //   α   — блеск: чем больше, тем меньше и ярче блик
        glm::vec3 g_albedo(0.9f, 0.4f, 0.3f);      // K_d
        glm::vec3 g_specularColor(1.0f, 1.0f, 1.0f); // K_s
        float     g_shininess = 32.0f;              // α

        // ============================================================
        // ПРОЧЕЕ (шейдерные модули и т.п.)
        // ============================================================
        std::vector<char> readFile(const std::string& path) {
            std::ifstream f(path, std::ios::ate | std::ios::binary);
            if (!f.is_open()) {
                std::cerr << "Cannot open " << path << "\n";
                return {};
            }
            size_t sz = (size_t)f.tellg();
            std::vector<char> buf(sz);
            f.seekg(0);
            f.read(buf.data(), sz);
            return buf;
        }

        VkShaderModule createShaderModule(const std::vector<char>& code) {
            VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            info.codeSize = code.size();
            info.pCode = reinterpret_cast<const uint32_t*>(code.data());
            VkShaderModule mod = VK_NULL_HANDLE;
            vkCreateShaderModule(graphics::internal::context.device, &info, nullptr, &mod);
            return mod;
        }

        // ============================================================
        // ГЕОМЕТРИЯ ПИРАМИДЫ + ПРОЦЕДУРНАЯ ГЕНЕРАЦИЯ НОРМАЛЕЙ
        // ============================================================
        // Из ТЗ лабы №2: «Нормали вершин рисуемого объекта нужно
        // генерировать процедурно».
        //
        // Идея: для каждого треугольника (A, B, C) нормаль вычисляется
        // через векторное произведение двух его рёбер:
        //     N = normalize(cross(B - A, C - A))
        //
        // Это классическая формула плоской нормали. Все три вершины
        // одного треугольника получают одну и ту же нормаль —
        // такой подход называется FLAT SHADING (плоское затенение).
        // Он идеально подходит для пирамиды: грани плоские, поэтому
        // освещение каждой грани будет выглядеть как у настоящей
        // кристаллической пирамиды.
        void buildPyramid() {
            auto& ctx = graphics::internal::context;

            // Определяем 8 угловых точек пирамиды в локальных координатах.
            // Основание — квадрат в плоскости Y = -1. Вершина — (0, 1, 0).
            glm::vec3 p000(-1.0f, -1.0f,  1.0f);   // передний левый
            glm::vec3 p001(-1.0f, -1.0f, -1.0f);   // задний левый
            glm::vec3 p100( 1.0f, -1.0f, -1.0f);   // задний правый
            glm::vec3 p101( 1.0f, -1.0f,  1.0f);   // передний правый
            glm::vec3 apex( 0.0f,  1.0f,  0.0f);   // вершина

            // Базовые цвета граней (для отладки; в Блинн-Фонге основной
            // вклад даёт albedo, но оставим цвета для наглядности)
            glm::vec3 cBase1(0.8f, 0.8f, 0.2f);
            glm::vec3 cBase2(0.2f, 0.2f, 0.8f);
            glm::vec3 cSide1(0.2f, 0.8f, 0.2f);
            glm::vec3 cSide2(0.8f, 0.2f, 0.8f);
            glm::vec3 cSide3(0.8f, 0.8f, 0.2f);
            glm::vec3 cSide4(0.2f, 0.8f, 0.8f);

            std::vector<Vertex> verts;
            verts.reserve(18);   // 6 треугольников × 3 вершины

            // Лямбда: добавляет треугольник, вычисляя нормаль процедурно.
            auto addTriangle = [&](const glm::vec3& a, const glm::vec3& b,
                                   const glm::vec3& c,
                                   const glm::vec3& ca, const glm::vec3& cb,
                                   const glm::vec3& cc) {
                // Процедурная нормаль по формуле cross(B - A, C - A)
                glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
                verts.push_back({ a, ca, n });
                verts.push_back({ b, cb, n });
                verts.push_back({ c, cc, n });
            };

            // Основание — 2 треугольника (нормаль смотрит вниз, т.к. cross даёт -Y)
            addTriangle(p000, p001, p100, cBase1, cBase1, cBase1);
            addTriangle(p000, p100, p101, cBase2, cBase2, cBase2);

            // Боковые грани — 4 треугольника, каждая смотрит наружу
            addTriangle(p000, p101, apex, cSide1, cSide1, cSide1); // передняя
            addTriangle(p101, p100, apex, cSide2, cSide2, cSide2); // правая
            addTriangle(p100, p001, apex, cSide3, cSide3, cSide3); // задняя
            addTriangle(p001, p000, apex, cSide4, cSide4, cSide4); // левая

            ctx.vertex_count = (uint32_t)verts.size();

            VkBufferCreateInfo bufInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bufInfo.size = verts.size() * sizeof(Vertex);
            bufInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo mapInfo{};
            vmaCreateBuffer(ctx.allocator, &bufInfo, &allocInfo,
                &ctx.vertex_buffer, &ctx.vertex_allocation, &mapInfo);
            memcpy(mapInfo.pMappedData, verts.data(), bufInfo.size);
        }

        // ============================================================
        // ГРАФИЧЕСКИЙ КОНВЕЙЕР
        // ============================================================
        bool createPipeline() {
            auto& ctx = graphics::internal::context;

            auto vertCode = readFile("shader.vert.spv");
            auto fragCode = readFile("shader.frag.spv");
            if (vertCode.empty() || fragCode.empty()) {
                std::cerr << "Shader SPV not found\n";
                return false;
            }

            ctx.vert_shader_module = createShaderModule(vertCode);
            ctx.frag_shader_module = createShaderModule(fragCode);

            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = ctx.vert_shader_module;
            stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = ctx.frag_shader_module;
            stages[1].pName = "main";

            // ── ВЕРШИННЫЙ ВВОД ──────────────────────────────────────
            // Теперь у нас 3 атрибута: pos (location 0), color (location 1),
            // normal (location 2). Это ключевое изменение по сравнению с лабой №1.
            VkVertexInputBindingDescription binding{};
            binding.binding = 0;
            binding.stride = sizeof(Vertex);
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            VkVertexInputAttributeDescription attrs[3]{};
            attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos) };
            attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color) };
            attrs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal) };

            VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &binding;
            vi.vertexAttributeDescriptionCount = 3;
            vi.pVertexAttributeDescriptions = attrs;

            VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            vp.viewportCount = 1;
            vp.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE;   // оставляем отключённым — видно все грани
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            ds.depthTestEnable = VK_TRUE;
            ds.depthWriteEnable = VK_TRUE;
            ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

            VkPipelineColorBlendAttachmentState blendAtt{};
            blendAtt.colorWriteMask = 0xF;
            blendAtt.blendEnable = VK_FALSE;

            VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            cb.attachmentCount = 1;
            cb.pAttachments = &blendAtt;

            VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dynState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynState.dynamicStateCount = 2;
            dynState.pDynamicStates = dyn;

            VkDescriptorSetLayoutBinding uboBinding{};
            uboBinding.binding = 0;
            uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            uboBinding.descriptorCount = 1;
            uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo slInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slInfo.bindingCount = 1;
            slInfo.pBindings = &uboBinding;
            vkCreateDescriptorSetLayout(ctx.device, &slInfo, nullptr, &ctx.descriptor_set_layout);

            VkPipelineLayoutCreateInfo plInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plInfo.setLayoutCount = 1;
            plInfo.pSetLayouts = &ctx.descriptor_set_layout;
            vkCreatePipelineLayout(ctx.device, &plInfo, nullptr, &ctx.pipeline_layout);

            VkGraphicsPipelineCreateInfo pInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            pInfo.stageCount = 2;
            pInfo.pStages = stages;
            pInfo.pVertexInputState = &vi;
            pInfo.pInputAssemblyState = &ia;
            pInfo.pViewportState = &vp;
            pInfo.pRasterizationState = &rs;
            pInfo.pMultisampleState = &ms;
            pInfo.pDepthStencilState = &ds;
            pInfo.pColorBlendState = &cb;
            pInfo.pDynamicState = &dynState;
            pInfo.layout = ctx.pipeline_layout;
            pInfo.renderPass = ctx.render_pass;
            pInfo.subpass = 0;

            if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pInfo,
                nullptr, &ctx.graphics_pipeline) != VK_SUCCESS) {
                std::cerr << "Failed to create pipeline\n";
                return false;
            }

            // ── UNIFORM-БУФЕР (увеличенный) ─────────────────────────
            VkBufferCreateInfo ubInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ubInfo.size = sizeof(UBO);
            ubInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

            VmaAllocationCreateInfo uaInfo{};
            uaInfo.usage = VMA_MEMORY_USAGE_AUTO;
            uaInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo ubMap{};
            vmaCreateBuffer(ctx.allocator, &ubInfo, &uaInfo,
                &ctx.uniform_buffer, &ctx.uniform_allocation, &ubMap);
            ctx.uniform_mapped = ubMap.pMappedData;

            VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
            VkDescriptorPoolCreateInfo dpInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dpInfo.maxSets = 1;
            dpInfo.poolSizeCount = 1;
            dpInfo.pPoolSizes = &poolSize;
            vkCreateDescriptorPool(ctx.device, &dpInfo, nullptr, &ctx.descriptor_pool);

            VkDescriptorSetAllocateInfo dsInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsInfo.descriptorPool = ctx.descriptor_pool;
            dsInfo.descriptorSetCount = 1;
            dsInfo.pSetLayouts = &ctx.descriptor_set_layout;
            vkAllocateDescriptorSets(ctx.device, &dsInfo, &ctx.descriptor_set);

            VkDescriptorBufferInfo bufInfo{ ctx.uniform_buffer, 0, sizeof(UBO) };
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = ctx.descriptor_set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.pBufferInfo = &bufInfo;
            vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);

            return true;
        }

        // ============================================================
        // ХЕЛПЕР: векторы камеры из углов Эйлера
        // ============================================================
        // Из лекции №4 (слайды 5–9): камера описывается позицией и
        // ориентацией. Удобный способ — задать yaw (рыскание вокруг Y)
        // и pitch (наклон вокруг X). Тогда forward вычисляется так:
        //
        //   forward.x = cos(pitch) * cos(yaw)
        //   forward.y = sin(pitch)
        //   forward.z = cos(pitch) * sin(yaw)
        //
        // Это стандартная сферическая параметризация направления.
        glm::vec3 computeCameraForward() {
            float yaw   = glm::radians(g_cameraYaw);
            float pitch = glm::radians(g_cameraPitch);
            glm::vec3 f;
            f.x = std::cos(pitch) * std::cos(yaw);
            f.y = std::sin(pitch);
            f.z = std::cos(pitch) * std::sin(yaw);
            return glm::normalize(f);
        }

    } // anon namespace

    // ============================================================
    // PUBLIC API
    // ============================================================

    bool initialize() {
        buildPyramid();
        return createPipeline();
    }

    void shutdown() {
        auto& ctx = graphics::internal::context;
        vkDeviceWaitIdle(ctx.device);

        if (ctx.vertex_buffer)         vmaDestroyBuffer(ctx.allocator, ctx.vertex_buffer, ctx.vertex_allocation);
        if (ctx.uniform_buffer)        vmaDestroyBuffer(ctx.allocator, ctx.uniform_buffer, ctx.uniform_allocation);
        if (ctx.descriptor_pool)       vkDestroyDescriptorPool(ctx.device, ctx.descriptor_pool, nullptr);
        if (ctx.descriptor_set_layout) vkDestroyDescriptorSetLayout(ctx.device, ctx.descriptor_set_layout, nullptr);
        if (ctx.graphics_pipeline)     vkDestroyPipeline(ctx.device, ctx.graphics_pipeline, nullptr);
        if (ctx.pipeline_layout)       vkDestroyPipelineLayout(ctx.device, ctx.pipeline_layout, nullptr);
        if (ctx.vert_shader_module)    vkDestroyShaderModule(ctx.device, ctx.vert_shader_module, nullptr);
        if (ctx.frag_shader_module)    vkDestroyShaderModule(ctx.device, ctx.frag_shader_module, nullptr);
    }

    // ============================================================
    // ОБНОВЛЕНИЕ UI
    // ============================================================
    // Добавлены секции: Камера, Источник света, Рассеянный свет, Материал.
    // Все параметры из формулы Блинн-Фонга (слайд 29 лекции №4)
    // выведены в интерфейс, как требует базовое задание.
    void update(double time) {
        if (g_animPlaying) g_animTime += (float)time * g_animSpeed;

        ImGui::Begin("Лаба №2 — Камера и освещение");

        // ── ПРОЕКЦИЯ ────────────────────────────────────────────
        ImGui::Text("Проекция:");
        ImGui::RadioButton("Перспективная", &g_projectionMode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Ортографическая", &g_projectionMode, 0);

        ImGui::Separator();

        // ── ТРАНСФОРМАЦИЯ МОДЕЛИ ────────────────────────────────
        if (ImGui::CollapsingHeader("Модель", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat3("Позиция",  &g_position.x, -5.0f, 5.0f);
            ImGui::SliderFloat3("Поворот",  &g_rotation.x, -180.0f, 180.0f);
            ImGui::SliderFloat3("Масштаб",  &g_scale.x, 0.1f, 3.0f);
            ImGui::Checkbox("Анимация", &g_animPlaying);
            ImGui::SliderFloat("Скорость", &g_animSpeed, 0.0f, 1.0f);
            ImGui::SliderFloat("Радиус",   &g_animRadius, 0.0f, 5.0f);
        }

        ImGui::Separator();

        // ── КАМЕРА ──────────────────────────────────────────────
        // Управляем позицией, углами yaw/pitch и углом обзора.
        if (ImGui::CollapsingHeader("Камера", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat3("Позиция камеры", &g_cameraPos.x, -15.0f, 15.0f);
            ImGui::SliderFloat("Yaw",   &g_cameraYaw,   -180.0f, 180.0f);
            ImGui::SliderFloat("Pitch", &g_cameraPitch, -89.0f,  89.0f);
            ImGui::SliderFloat("Угол обзора", &g_cameraFov, 20.0f, 110.0f);
        }

        ImGui::Separator();

        // ── НАПРАВЛЕННЫЙ ИСТОЧНИК СВЕТА ─────────────────────────
        // Из лекции №4 (слайд 14): направленный источник — бесконечно
        // удалённый, все лучи параллельны. Задаётся только НАПРАВЛЕНИЕМ.
        if (ImGui::CollapsingHeader("Направленный источник", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat3("Направление", &g_lightDirection.x, -1.0f, 1.0f);
            ImGui::ColorEdit3("Цвет света",    &g_lightColor.x);
            ImGui::SliderFloat("Интенсивность", &g_lightIntensity, 0.0f, 5.0f);
        }

        ImGui::Separator();

        // ── РАССЕЯННЫЙ СВЕТ (AMBIENT) ───────────────────────────
        // Из лекции №4 (слайд 25): ambient имитирует свет окружения.
        if (ImGui::CollapsingHeader("Рассеянный свет", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::ColorEdit3("Цвет ambient", &g_ambientColor.x);
        }

        ImGui::Separator();

        // ── МАТЕРИАЛ ────────────────────────────────────────────
        // Из лекции №4 (слайды 21–24):
        //   K_d — альбедо, K_s — цвет блика, α — блеск.
        if (ImGui::CollapsingHeader("Материал", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::ColorEdit3("Альбедо (K_d)",   &g_albedo.x);
            ImGui::ColorEdit3("Цвет блика (K_s)", &g_specularColor.x);
            ImGui::SliderFloat("Блеск (α)", &g_shininess, 1.0f, 256.0f);
        }

        ImGui::Separator();

        // ── СБРОС ───────────────────────────────────────────────
        if (ImGui::Button("Сбросить всё")) {
            g_position = glm::vec3(0.0f);
            g_rotation = glm::vec3(30.0f, 45.0f, 0.0f);
            g_scale = glm::vec3(1.0f);
            g_animTime = 0.0f;
            g_animSpeed = 0.1f;
            g_animRadius = 2.0f;
            g_projectionMode = 1;

            g_cameraPos   = glm::vec3(0.0f, 2.0f, 6.0f);
            g_cameraYaw   = -90.0f;
            g_cameraPitch = -15.0f;
            g_cameraFov   = 45.0f;

            g_lightDirection = glm::vec3(-0.5f, -1.0f, -0.3f);
            g_lightColor     = glm::vec3(1.0f, 1.0f, 1.0f);
            g_lightIntensity = 1.5f;

            g_ambientColor = glm::vec3(0.15f, 0.15f, 0.18f);

            g_albedo        = glm::vec3(0.9f, 0.4f, 0.3f);
            g_specularColor = glm::vec3(1.0f, 1.0f, 1.0f);
            g_shininess     = 32.0f;
        }

        ImGui::End();
    }

    // ============================================================
    // РЕНДЕР
    // ============================================================
    void render(const graphics::internal::FrameData& fd) {
        auto& ctx = graphics::internal::context;

        // ── АНИМАЦИЯ СМЕЩЕНИЯ (из лабы №1) ──────────────────────
        glm::vec3 animOffset(0.0f);
        if (g_animPlaying || g_animTime > 0.0f) {
            animOffset = glm::vec3(
                glm::cos(g_animTime) * g_animRadius,
                glm::sin(g_animTime) * 0.5f,
                glm::sin(g_animTime) * g_animRadius
            );
        }

        // ── МОДЕЛЬНАЯ МАТРИЦА (M) ───────────────────────────────
        // Из лекции №4 (слайд 4): M = S · R · T, где порядок важен!
        // Трансформации применяются справа налево к вершине:
        // сначала перенос, потом поворот, потом масштаб.
        glm::mat4 model(1.0f);
        model = glm::translate(model, g_position + animOffset);
        model = glm::rotate(model, glm::radians(g_rotation.x), { 1,0,0 });
        model = glm::rotate(model, glm::radians(g_rotation.y), { 0,1,0 });
        model = glm::rotate(model, glm::radians(g_rotation.z), { 0,0,1 });
        model = glm::scale(model, g_scale);

        // ── МАТРИЦА ВИДА (V) ────────────────────────────────────
        // Из лекции №4 (слайды 5–9): V — это ОБРАТНАЯ матрица камеры.
        // Мы строим её через glm::lookAt (eye, center, up).
        // center = eye + forward, где forward вычисляется из yaw/pitch.
        glm::vec3 camForward = computeCameraForward();
        glm::vec3 camTarget  = g_cameraPos + camForward;
        glm::mat4 view = glm::lookAt(g_cameraPos, camTarget, glm::vec3(0, 1, 0));

        // ── МАТРИЦА ПРОЕКЦИИ (P) ────────────────────────────────
        float aspect = (ctx.swapchain_extent.height > 0)
            ? (float)ctx.swapchain_extent.width / (float)ctx.swapchain_extent.height
            : 1.0f;

        glm::mat4 proj;
        if (g_projectionMode == 0) {
            float orthoSize = g_animRadius + 4.0f;
            proj = glm::orthoRH_ZO(-orthoSize * aspect, orthoSize * aspect,
                                   -orthoSize, orthoSize, 0.1f, 100.0f);
        } else {
            proj = glm::perspectiveRH_ZO(glm::radians(g_cameraFov), aspect, 0.1f, 100.0f);
        }
        proj[1][1] *= -1.0f;   // Y-flip для Vulkan NDC

        // ── ЗАПОЛНЕНИЕ UNIFORM-БУФЕРА ───────────────────────────
        UBO ubo{};
        ubo.mvp = proj * view * model;
        ubo.model = model;
        ubo.camera_pos      = glm::vec4(g_cameraPos, 1.0f);
        ubo.light_direction = glm::vec4(glm::normalize(g_lightDirection), 0.0f);
        ubo.light_color     = glm::vec4(g_lightColor * g_lightIntensity, 1.0f);
        ubo.ambient_light   = glm::vec4(g_ambientColor, 1.0f);
        ubo.albedo          = glm::vec4(g_albedo, 1.0f);
        ubo.specular        = glm::vec4(g_specularColor, 1.0f);
        ubo.material        = glm::vec4(g_shininess, 0.0f, 0.0f, 0.0f);
        memcpy(ctx.uniform_mapped, &ubo, sizeof(UBO));

        // ── ЗАПИСЬ КОМАНД ───────────────────────────────────────
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(fd.command_buffer, &begin);

        VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rp.renderPass = ctx.render_pass;
        rp.framebuffer = fd.framebuffer;
        rp.renderArea = { {0,0}, ctx.swapchain_extent };

        VkClearValue clears[2]{};
        clears[0].color = { {0.05f, 0.05f, 0.07f, 1.0f} };  // фон чуть темнее
        clears[1].depthStencil = { 1.0f, 0 };
        rp.clearValueCount = 2;
        rp.pClearValues = clears;

        vkCmdBeginRenderPass(fd.command_buffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{ 0, 0,
            (float)ctx.swapchain_extent.width,
            (float)ctx.swapchain_extent.height, 0, 1 };
        VkRect2D sc{ {0,0}, ctx.swapchain_extent };
        vkCmdSetViewport(fd.command_buffer, 0, 1, &vp);
        vkCmdSetScissor(fd.command_buffer, 0, 1, &sc);

        vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.graphics_pipeline);
        vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            ctx.pipeline_layout, 0, 1, &ctx.descriptor_set, 0, nullptr);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &ctx.vertex_buffer, &offset);
        vkCmdDraw(fd.command_buffer, ctx.vertex_count, 1, 0, 0);

        vkCmdEndRenderPass(fd.command_buffer);
        vkEndCommandBuffer(fd.command_buffer);
    }

} // namespace application