#include "application.hpp"

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <fstream>
#include <vector>
#include <cstring>
#include <iostream>
#include <cmath>

// === ЛР3: загрузка изображений ===
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace application {

    namespace {

        // ============================================================
        // ВЕРШИНА
        // ============================================================
        // Из лекции №4: для освещения по Блинн-Фонгу нам нужна
        // НОРМАЛЬ поверхности в каждой вершине. Без неё невозможно
        // вычислить cos(θ) — угол между нормалью и направлением света.
        // Поэтому в структуру добавляется третье поле: normal.
        //
        // ЛР3: добавлено поле uv — текстурные координаты.
        // По ТЗ ЛР3: «У вершин, помимо атрибутов позиции и нормали,
        // должны быть текстурные координаты».
        struct Vertex {
            glm::vec3 pos;
            glm::vec3 color;
            glm::vec3 normal;   // ← НОВОЕ (ЛР2): нормаль поверхности
            glm::vec2 uv;       // ← НОВОЕ (ЛР3): текстурные координаты
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
        //
        // ЛР3: albedo остаётся как фолбэк, если текстура выключена.
        // Основной цвет берётся из текстуры во фрагментном шейдере.
        struct UBO {
            glm::mat4 mvp;             // 64 байта
            glm::mat4 model;           // 64 байта

            glm::vec4 camera_pos;      // xyz = позиция, w = не используется
            glm::vec4 light_direction; // xyz = направление (мировое), w = 0
            glm::vec4 light_color;     // xyz = цвет × интенсивность, w = 1
            glm::vec4 ambient_light;   // xyz = цвет ambient, w = 1

            glm::vec4 albedo;          // K_d (цвет материала — фолбэк)
            glm::vec4 specular;        // K_s (цвет блика)
            glm::vec4 material;        // x = shininess (α), y = use_texture
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
        glm::vec3 g_cameraPos(0.0f, 2.0f, 6.0f);
        float     g_cameraYaw   = -90.0f;
        float     g_cameraPitch = -15.0f;
        float     g_cameraFov   = 45.0f;

        // ============================================================
        // СОСТОЯНИЕ UI — ИСТОЧНИК СВЕТА (НОВОЕ)
        // ============================================================
        glm::vec3 g_lightDirection(-0.5f, -1.0f, -0.3f);
        glm::vec3 g_lightColor(1.0f, 1.0f, 1.0f);
        float     g_lightIntensity = 1.5f;

        // ============================================================
        // СОСТОЯНИЕ UI — AMBIENT (рассеянный свет)
        // ============================================================
        glm::vec3 g_ambientColor(0.15f, 0.15f, 0.18f);

        // ============================================================
        // СОСТОЯНИЕ UI — МАТЕРИАЛ (НОВОЕ)
        // ============================================================
        glm::vec3 g_albedo(0.9f, 0.4f, 0.3f);        // K_d
        glm::vec3 g_specularColor(1.0f, 1.0f, 1.0f); // K_s
        float     g_shininess = 32.0f;                // α

        // === ЛР3: флаг использования текстуры ===
        bool g_useTexture = true;

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
        // ЛР3: ВСПОМОГАТЕЛЬНАЯ ФУНКЦИЯ — КОПИРОВАНИЕ БУФЕРА В VRAM
        // ============================================================
        // Из лекции №5.2 (слайды 11–14): память GPU (VRAM) недоступна
        // для маппинга CPU. Поэтому: создаём staging буфер в RAM (mapped),
        // копируем данные туда через memcpy, потом одной командой GPU
        // копирует из staging в VRAM. Это нужно по ТЗ ЛР3:
        // «Вершинные и индексные данные модели должны быть в VRAM».
        void copyBufferToVRAM(VkBuffer dst_buffer, const void* src_data, VkDeviceSize size) {
            auto& ctx = graphics::internal::context;

            // 1. Staging buffer в RAM (mapped)
            VkBufferCreateInfo staging_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            staging_info.size = size;
            staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

            VmaAllocationCreateInfo staging_alloc{};
            staging_alloc.usage = VMA_MEMORY_USAGE_AUTO;
            staging_alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                  VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VkBuffer staging = VK_NULL_HANDLE;
            VmaAllocation staging_alloc_obj = VK_NULL_HANDLE;
            VmaAllocationInfo staging_map{};

            vmaCreateBuffer(ctx.allocator, &staging_info, &staging_alloc,
                            &staging, &staging_alloc_obj, &staging_map);

            memcpy(staging_map.pMappedData, src_data, size);

            // 2. One-shot команда копирования
            VkCommandPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pool_info.queueFamilyIndex = ctx.graphics_queue_index;

            VkCommandPool pool = VK_NULL_HANDLE;
            vkCreateCommandPool(ctx.device, &pool_info, nullptr, &pool);

            VkCommandBufferAllocateInfo cmd_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cmd_info.commandPool = pool;
            cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cmd_info.commandBufferCount = 1;

            VkCommandBuffer cmd = VK_NULL_HANDLE;
            vkAllocateCommandBuffers(ctx.device, &cmd_info, &cmd);

            VkCommandBufferBeginInfo begin_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &begin_info);

            VkBufferCopy region{};
            region.size = size;
            vkCmdCopyBuffer(cmd, staging, dst_buffer, 1, &region);

            vkEndCommandBuffer(cmd);

            // 3. Submit + wait
            VkFenceCreateInfo fence_info{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            VkFence fence = VK_NULL_HANDLE;
            vkCreateFence(ctx.device, &fence_info, nullptr, &fence);

            VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            vkQueueSubmit(ctx.graphics_queue, 1, &submit, fence);
            vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX);

            // 4. Уборка
            vkDestroyFence(ctx.device, fence, nullptr);
            vkDestroyCommandPool(ctx.device, pool, nullptr);
            vmaDestroyBuffer(ctx.allocator, staging, staging_alloc_obj);
        }

        // ============================================================
        // ЛР3: ВЕРШИННЫЙ БУФЕР → VRAM
        // ============================================================
        // По ТЗ ЛР3: «Вершинные и индексные данные модели должны быть
        // в VRAM (GPU)». В ЛР2 буфер был HOST_ACCESS (RAM). Теперь GPU_ONLY.
        void uploadVerticesToVRAM(const std::vector<Vertex>& verts) {
            auto& ctx = graphics::internal::context;

            VkDeviceSize size = verts.size() * sizeof(Vertex);

            VkBufferCreateInfo buf_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            buf_info.size = size;
            buf_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT;

            VmaAllocationCreateInfo alloc_info{};
            alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
            alloc_info.flags = 0;   // GPU_ONLY, без HOST_ACCESS

            if (vmaCreateBuffer(ctx.allocator, &buf_info, &alloc_info,
                                &ctx.vertex_buffer, &ctx.vertex_allocation,
                                nullptr) != VK_SUCCESS) {
                std::cerr << "Failed to create vertex buffer in VRAM\n";
                return;
            }

            copyBufferToVRAM(ctx.vertex_buffer, verts.data(), size);

            ctx.vertex_count = (uint32_t)verts.size();
        }

        // ============================================================
        // ЛР3: ЗАГРУЗКА ТЕКСТУРЫ ИЗ PNG
        // ============================================================
        // Из лекции №5.2: создать VkImage в VRAM, залить пиксели
        // через staging буфер с барьерами, создать VkImageView и VkSampler.
        //
        // Из ТЗ ЛР3: «Изображения должны быть загружены из файловой системы.
        // Формат файлов растровой графики может быть любым: PNG, JPEG, ...»
        //
        // Функция ищет файл "brick.png" в текущей рабочей директории
        // (обычно там же, где .exe).
        bool loadTexture() {
            auto& ctx = graphics::internal::context;

            // === ЛР3: загрузка PNG через stb_image ===
            // stbi_load читает файл, декодирует в RGBA8 (4 байта на пиксель),
            // возвращает указатель на массив пикселей + ширину/высоту через out-параметры.
            int width = 0, height = 0, channels = 0;
            uint8_t* raw = stbi_load("brick.png", &width, &height, &channels, 4);

            if (!raw) {
                std::cerr << "Failed to load texture 'brick.png': "
                          << stbi_failure_reason() << "\n";
                std::cerr << "Put brick.png next to the .exe file\n";
                return false;
            }

            std::cout << "Loaded texture: " << width << "x" << height
                      << ", channels = " << channels << "\n";

            // Копируем пиксели в std::vector, освобождаем память stbi
            std::vector<uint8_t> pixels(raw, raw + (width * height * 4));
            stbi_image_free(raw);

            VkDeviceSize image_size = (VkDeviceSize)width * height * 4;

            // 1. VkImage в VRAM
            VkImageCreateInfo image_info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = VK_FORMAT_R8G8B8A8_SRGB;
            image_info.extent = { (uint32_t)width, (uint32_t)height, 1 };
            image_info.mipLevels = 1;
            image_info.arrayLayers = 1;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT;
            image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VmaAllocationCreateInfo alloc_info{};
            alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

            if (vmaCreateImage(ctx.allocator, &image_info, &alloc_info,
                               &ctx.albedo_image, &ctx.albedo_image_allocation,
                               nullptr) != VK_SUCCESS) {
                std::cerr << "Failed to create texture image\n";
                return false;
            }

            // 2. Staging buffer
            VkBufferCreateInfo staging_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            staging_info.size = image_size;
            staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

            VmaAllocationCreateInfo s_alloc{};
            s_alloc.usage = VMA_MEMORY_USAGE_AUTO;
            s_alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VkBuffer staging = VK_NULL_HANDLE;
            VmaAllocation staging_alloc_obj = VK_NULL_HANDLE;
            VmaAllocationInfo staging_map{};

            vmaCreateBuffer(ctx.allocator, &staging_info, &s_alloc,
                            &staging, &staging_alloc_obj, &staging_map);

            memcpy(staging_map.pMappedData, pixels.data(), image_size);

            // 3. One-shot: копирование staging → VkImage с барьерами
            VkCommandPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pool_info.queueFamilyIndex = ctx.graphics_queue_index;

            VkCommandPool pool = VK_NULL_HANDLE;
            vkCreateCommandPool(ctx.device, &pool_info, nullptr, &pool);

            VkCommandBufferAllocateInfo cmd_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cmd_info.commandPool = pool;
            cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cmd_info.commandBufferCount = 1;

            VkCommandBuffer cmd = VK_NULL_HANDLE;
            vkAllocateCommandBuffers(ctx.device, &cmd_info, &cmd);

            VkCommandBufferBeginInfo begin_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &begin_info);

            // Барьер: UNDEFINED → TRANSFER_DST
            VkImageMemoryBarrier to_dst{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.srcAccessMask = 0;
            to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_dst.image = ctx.albedo_image;
            to_dst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &to_dst);

            // Копирование
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };

            vkCmdCopyBufferToImage(cmd, staging, ctx.albedo_image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            // Барьер: TRANSFER_DST → SHADER_READ_ONLY
            VkImageMemoryBarrier to_read{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            to_read.image = ctx.albedo_image;
            to_read.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &to_read);

            vkEndCommandBuffer(cmd);

            VkFenceCreateInfo fence_info{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            VkFence fence = VK_NULL_HANDLE;
            vkCreateFence(ctx.device, &fence_info, nullptr, &fence);

            VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            vkQueueSubmit(ctx.graphics_queue, 1, &submit, fence);
            vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX);

            vkDestroyFence(ctx.device, fence, nullptr);
            vkDestroyCommandPool(ctx.device, pool, nullptr);
            vmaDestroyBuffer(ctx.allocator, staging, staging_alloc_obj);

            // 4. VkImageView
            VkImageViewCreateInfo view_info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            view_info.image = ctx.albedo_image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = VK_FORMAT_R8G8B8A8_SRGB;
            view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            if (vkCreateImageView(ctx.device, &view_info, nullptr,
                                  &ctx.albedo_image_view) != VK_SUCCESS) {
                std::cerr << "Failed to create image view\n";
                return false;
            }

            // 5. VkSampler
            VkSamplerCreateInfo sampler_info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sampler_info.magFilter = VK_FILTER_LINEAR;
            sampler_info.minFilter = VK_FILTER_LINEAR;
            sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

            if (vkCreateSampler(ctx.device, &sampler_info, nullptr,
                                &ctx.albedo_sampler) != VK_SUCCESS) {
                std::cerr << "Failed to create sampler\n";
                return false;
            }

            return true;
        }

        // ============================================================
        // ГЕОМЕТРИЯ ПИРАМИДЫ + ПРОЦЕДУРНАЯ ГЕНЕРАЦИЯ НОРМАЛЕЙ
        // ============================================================
        void buildPyramid() {
            // Определяем 8 угловых точек пирамиды в локальных координатах.
            // Основание — квадрат в плоскости Y = -1. Вершина — (0, 1, 0).
            glm::vec3 p000(-1.0f, -1.0f,  1.0f);   // передний левый
            glm::vec3 p001(-1.0f, -1.0f, -1.0f);   // задний левый
            glm::vec3 p100( 1.0f, -1.0f, -1.0f);   // задний правый
            glm::vec3 p101( 1.0f, -1.0f,  1.0f);   // передний правый
            glm::vec3 apex( 0.0f,  1.0f,  0.0f);   // вершина

            glm::vec3 cBase1(0.8f, 0.8f, 0.2f);
            glm::vec3 cBase2(0.2f, 0.2f, 0.8f);
            glm::vec3 cSide1(0.2f, 0.8f, 0.2f);
            glm::vec3 cSide2(0.8f, 0.2f, 0.8f);
            glm::vec3 cSide3(0.8f, 0.8f, 0.2f);
            glm::vec3 cSide4(0.2f, 0.8f, 0.8f);

            // === ЛР3: UV-координаты для треугольника (0,0) (1,0) (0,1) ===
            glm::vec2 uv0(0.0f, 0.0f);
            glm::vec2 uv1(1.0f, 0.0f);
            glm::vec2 uv2(0.0f, 1.0f);

            std::vector<Vertex> verts;
            verts.reserve(18);

            auto addTriangle = [&](const glm::vec3& a, const glm::vec3& b,
                                   const glm::vec3& c,
                                   const glm::vec3& ca, const glm::vec3& cb,
                                   const glm::vec3& cc) {
                glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
                verts.push_back({ a, ca, n, uv0 });
                verts.push_back({ b, cb, n, uv1 });
                verts.push_back({ c, cc, n, uv2 });
            };

            // Основание
            addTriangle(p000, p001, p100, cBase1, cBase1, cBase1);
            addTriangle(p000, p100, p101, cBase2, cBase2, cBase2);

            // Боковые грани
            addTriangle(p000, p101, apex, cSide1, cSide1, cSide1);
            addTriangle(p101, p100, apex, cSide2, cSide2, cSide2);
            addTriangle(p100, p001, apex, cSide3, cSide3, cSide3);
            addTriangle(p001, p000, apex, cSide4, cSide4, cSide4);

            uploadVerticesToVRAM(verts);
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

            VkVertexInputBindingDescription binding{};
            binding.binding = 0;
            binding.stride = sizeof(Vertex);
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            VkVertexInputAttributeDescription attrs[4]{};
            attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos) };
            attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color) };
            attrs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal) };
            attrs[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex, uv) };

            VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &binding;
            vi.vertexAttributeDescriptionCount = 4;
            vi.pVertexAttributeDescriptions = attrs;

            VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            vp.viewportCount = 1;
            vp.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE;
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

            VkDescriptorSetLayoutBinding bindings[2]{};

            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo slInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slInfo.bindingCount = 2;
            slInfo.pBindings = bindings;
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

            VkDescriptorPoolSize poolSizes[2]{};
            poolSizes[0] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
            poolSizes[1] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };

            VkDescriptorPoolCreateInfo dpInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dpInfo.maxSets = 1;
            dpInfo.poolSizeCount = 2;
            dpInfo.pPoolSizes = poolSizes;
            vkCreateDescriptorPool(ctx.device, &dpInfo, nullptr, &ctx.descriptor_pool);

            VkDescriptorSetAllocateInfo dsInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsInfo.descriptorPool = ctx.descriptor_pool;
            dsInfo.descriptorSetCount = 1;
            dsInfo.pSetLayouts = &ctx.descriptor_set_layout;
            vkAllocateDescriptorSets(ctx.device, &dsInfo, &ctx.descriptor_set);

            VkDescriptorBufferInfo bufInfo{ ctx.uniform_buffer, 0, sizeof(UBO) };

            VkDescriptorImageInfo image_info{};
            image_info.sampler = ctx.albedo_sampler;
            image_info.imageView = ctx.albedo_image_view;
            image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet writes[2]{};

            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = ctx.descriptor_set;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &bufInfo;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = ctx.descriptor_set;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &image_info;

            vkUpdateDescriptorSets(ctx.device, 2, writes, 0, nullptr);

            return true;
        }

        // ============================================================
        // ХЕЛПЕР: векторы камеры из углов Эйлера
        // ============================================================
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
        // === ЛР3: сначала загружаем текстуру, потом пайплайн ===
        if (!loadTexture()) {
            std::cerr << "Failed to load texture\n";
            return false;
        }

        buildPyramid();
        return createPipeline();
    }

    void shutdown() {
        auto& ctx = graphics::internal::context;
        vkDeviceWaitIdle(ctx.device);

        // === ЛР3: освобождаем текстуру ===
        if (ctx.albedo_sampler)    vkDestroySampler(ctx.device, ctx.albedo_sampler, nullptr);
        if (ctx.albedo_image_view) vkDestroyImageView(ctx.device, ctx.albedo_image_view, nullptr);
        if (ctx.albedo_image)      vmaDestroyImage(ctx.allocator, ctx.albedo_image, ctx.albedo_image_allocation);

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
    void update(double time) {
        if (g_animPlaying) g_animTime += (float)time * g_animSpeed;

        ImGui::Begin("Лаба №3 — Текстурирование");

        ImGui::Text("Проекция:");
        ImGui::RadioButton("Перспективная", &g_projectionMode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Ортографическая", &g_projectionMode, 0);

        ImGui::Separator();

        if (ImGui::CollapsingHeader("Модель", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat3("Позиция",  &g_position.x, -5.0f, 5.0f);
            ImGui::SliderFloat3("Поворот",  &g_rotation.x, -180.0f, 180.0f);
            ImGui::SliderFloat3("Масштаб",  &g_scale.x, 0.1f, 3.0f);
            ImGui::Checkbox("Анимация", &g_animPlaying);
            ImGui::SliderFloat("Скорость", &g_animSpeed, 0.0f, 1.0f);
            ImGui::SliderFloat("Радиус",   &g_animRadius, 0.0f, 5.0f);
        }

        ImGui::Separator();

        if (ImGui::CollapsingHeader("Камера", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat3("Позиция камеры", &g_cameraPos.x, -15.0f, 15.0f);
            ImGui::SliderFloat("Yaw",   &g_cameraYaw,   -180.0f, 180.0f);
            ImGui::SliderFloat("Pitch", &g_cameraPitch, -89.0f,  89.0f);
            ImGui::SliderFloat("Угол обзора", &g_cameraFov, 20.0f, 110.0f);
        }

        ImGui::Separator();

        if (ImGui::CollapsingHeader("Направленный источник", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat3("Направление", &g_lightDirection.x, -1.0f, 1.0f);
            ImGui::ColorEdit3("Цвет света",    &g_lightColor.x);
            ImGui::SliderFloat("Интенсивность", &g_lightIntensity, 0.0f, 5.0f);
        }

        ImGui::Separator();

        if (ImGui::CollapsingHeader("Рассеянный свет", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::ColorEdit3("Цвет ambient", &g_ambientColor.x);
        }

        ImGui::Separator();

        if (ImGui::CollapsingHeader("Материал", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Использовать текстуру", &g_useTexture);
            ImGui::ColorEdit3("Альбедо (K_d)",   &g_albedo.x);
            ImGui::ColorEdit3("Цвет блика (K_s)", &g_specularColor.x);
            ImGui::SliderFloat("Блеск (α)", &g_shininess, 1.0f, 256.0f);
        }

        ImGui::Separator();

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

            g_useTexture = true;
        }

        ImGui::End();
    }

    // ============================================================
    // РЕНДЕР
    // ============================================================
    void render(const graphics::internal::FrameData& fd) {
        auto& ctx = graphics::internal::context;

        glm::vec3 animOffset(0.0f);
        if (g_animPlaying || g_animTime > 0.0f) {
            animOffset = glm::vec3(
                glm::cos(g_animTime) * g_animRadius,
                glm::sin(g_animTime) * 0.5f,
                glm::sin(g_animTime) * g_animRadius
            );
        }

        glm::mat4 model(1.0f);
        model = glm::translate(model, g_position + animOffset);
        model = glm::rotate(model, glm::radians(g_rotation.x), { 1,0,0 });
        model = glm::rotate(model, glm::radians(g_rotation.y), { 0,1,0 });
        model = glm::rotate(model, glm::radians(g_rotation.z), { 0,0,1 });
        model = glm::scale(model, g_scale);

        glm::vec3 camForward = computeCameraForward();
        glm::vec3 camTarget  = g_cameraPos + camForward;
        glm::mat4 view = glm::lookAt(g_cameraPos, camTarget, glm::vec3(0, 1, 0));

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
        proj[1][1] *= -1.0f;

        UBO ubo{};
        ubo.mvp = proj * view * model;
        ubo.model = model;
        ubo.camera_pos      = glm::vec4(g_cameraPos, 1.0f);
        ubo.light_direction = glm::vec4(glm::normalize(g_lightDirection), 0.0f);
        ubo.light_color     = glm::vec4(g_lightColor * g_lightIntensity, 1.0f);
        ubo.ambient_light   = glm::vec4(g_ambientColor, 1.0f);
        ubo.albedo          = glm::vec4(g_albedo, 1.0f);
        ubo.specular        = glm::vec4(g_specularColor, 1.0f);
        ubo.material        = glm::vec4(g_shininess,
                                        g_useTexture ? 1.0f : 0.0f,
                                        0.0f, 0.0f);
        memcpy(ctx.uniform_mapped, &ubo, sizeof(UBO));

        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(fd.command_buffer, &begin);

        VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rp.renderPass = ctx.render_pass;
        rp.framebuffer = fd.framebuffer;
        rp.renderArea = { {0,0}, ctx.swapchain_extent };

        VkClearValue clears[2]{};
        clears[0].color = { {0.05f, 0.05f, 0.07f, 1.0f} };
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