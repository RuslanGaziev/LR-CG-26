#include "application.hpp"

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <fstream>
#include <vector>
#include <cstring>
#include <iostream>
#include <cmath>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace application {

    namespace {

        struct Vertex {
            glm::vec3 pos;
            glm::vec3 color;
            glm::vec3 normal;
            glm::vec2 uv;
        };

        struct UBO {
            glm::mat4 mvp;
            glm::mat4 model;
            glm::vec4 camera_pos;
            glm::vec4 light_direction;
            glm::vec4 light_color;
            glm::vec4 ambient_light;
            glm::vec4 albedo;
            glm::vec4 specular;
            glm::vec4 material;
            glm::mat4 light_view_proj;
            glm::vec4 shadow_params;
        };

        struct ShadowUBO {
            glm::mat4 light_view_proj;
            glm::mat4 model;
        };

        // === Состояние UI ===
        glm::vec3 g_position(0.0f);
        glm::vec3 g_rotation(0.0f, 0.0f, 0.0f); // Убрали поворот для наглядности
        glm::vec3 g_scale(1.0f);
        int       g_projectionMode = 1;

        bool  g_animPlaying = false; 
        float g_animSpeed = 0.1f;
        float g_animRadius = 2.0f;
        float g_animTime = 0.0f;

        glm::vec3 g_cameraPos(0.0f, 2.0f, 6.0f);
        float     g_cameraYaw   = -90.0f;
        float     g_cameraPitch = -15.0f;
        float     g_cameraFov   = 45.0f;

        glm::vec3 g_lightDirection(-0.5f, -1.0f, -0.3f);
        glm::vec3 g_lightColor(1.0f, 1.0f, 1.0f);
        float     g_lightIntensity = 1.5f;
        glm::vec3 g_ambientColor(0.15f, 0.15f, 0.18f);

        glm::vec3 g_albedo(0.9f, 0.4f, 0.3f);
        glm::vec3 g_specularColor(1.0f, 1.0f, 1.0f);
        float     g_shininess = 32.0f;
        bool g_useTexture = true;

        float g_shadowOrthoSize = 8.0f;
        float g_shadowDepthBias = 0.01f; 
        bool  g_shadowsEnabled  = true;

        uint32_t g_pyramid_vertex_count = 0;
        uint32_t g_floor_first_vertex   = 0;
        uint32_t g_floor_vertex_count   = 0;

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

        void copyBufferToVRAM(VkBuffer dst_buffer, const void* src_data, VkDeviceSize size) {
            auto& ctx = graphics::internal::context;
            VkBufferCreateInfo staging_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            staging_info.size = size;
            staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo staging_alloc{};
            staging_alloc.usage = VMA_MEMORY_USAGE_AUTO;
            staging_alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VkBuffer staging = VK_NULL_HANDLE;
            VmaAllocation staging_alloc_obj = VK_NULL_HANDLE;
            VmaAllocationInfo staging_map{};
            vmaCreateBuffer(ctx.allocator, &staging_info, &staging_alloc, &staging, &staging_alloc_obj, &staging_map);
            memcpy(staging_map.pMappedData, src_data, size);

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
        }

        void uploadVerticesToVRAM(const std::vector<Vertex>& verts) {
            auto& ctx = graphics::internal::context;
            VkDeviceSize size = verts.size() * sizeof(Vertex);
            VkBufferCreateInfo buf_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            buf_info.size = size;
            buf_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo alloc_info{};
            alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
            if (vmaCreateBuffer(ctx.allocator, &buf_info, &alloc_info, &ctx.vertex_buffer, &ctx.vertex_allocation, nullptr) != VK_SUCCESS) {
                std::cerr << "Failed to create vertex buffer in VRAM\n";
                return;
            }
            copyBufferToVRAM(ctx.vertex_buffer, verts.data(), size);
            ctx.vertex_count = (uint32_t)verts.size();
        }

        bool loadTexture() {
            auto& ctx = graphics::internal::context;
            int width = 0, height = 0, channels = 0;
            uint8_t* raw = stbi_load("brick.png", &width, &height, &channels, 4);
            if (!raw) {
                std::cerr << "Failed to load texture 'brick.png': " << stbi_failure_reason() << "\n";
                return false;
            }
            std::vector<uint8_t> pixels(raw, raw + (width * height * 4));
            stbi_image_free(raw);
            VkDeviceSize image_size = (VkDeviceSize)width * height * 4;

            VkImageCreateInfo image_info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = VK_FORMAT_R8G8B8A8_SRGB;
            image_info.extent = { (uint32_t)width, (uint32_t)height, 1 };
            image_info.mipLevels = 1;
            image_info.arrayLayers = 1;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo alloc_info{};
            alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            if (vmaCreateImage(ctx.allocator, &image_info, &alloc_info, &ctx.albedo_image, &ctx.albedo_image_allocation, nullptr) != VK_SUCCESS) return false;

            VkBufferCreateInfo staging_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            staging_info.size = image_size;
            staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo s_alloc{};
            s_alloc.usage = VMA_MEMORY_USAGE_AUTO;
            s_alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VkBuffer staging = VK_NULL_HANDLE;
            VmaAllocation staging_alloc_obj = VK_NULL_HANDLE;
            VmaAllocationInfo staging_map{};
            vmaCreateBuffer(ctx.allocator, &staging_info, &s_alloc, &staging, &staging_alloc_obj, &staging_map);
            memcpy(staging_map.pMappedData, pixels.data(), image_size);

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

            VkImageMemoryBarrier to_dst{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.srcAccessMask = 0;
            to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_dst.image = ctx.albedo_image;
            to_dst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
            vkCmdCopyBufferToImage(cmd, staging, ctx.albedo_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            VkImageMemoryBarrier to_read{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            to_read.image = ctx.albedo_image;
            to_read.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_read);
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

            VkImageViewCreateInfo view_info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            view_info.image = ctx.albedo_image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = VK_FORMAT_R8G8B8A8_SRGB;
            view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vkCreateImageView(ctx.device, &view_info, nullptr, &ctx.albedo_image_view) != VK_SUCCESS) return false;

            VkSamplerCreateInfo sampler_info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sampler_info.magFilter = VK_FILTER_LINEAR;
            sampler_info.minFilter = VK_FILTER_LINEAR;
            sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            if (vkCreateSampler(ctx.device, &sampler_info, nullptr, &ctx.albedo_sampler) != VK_SUCCESS) return false;
            return true;
        }

        std::vector<Vertex> buildAllVertices() {
            glm::vec3 p000(-1.0f, -1.0f,  1.0f);
            glm::vec3 p001(-1.0f, -1.0f, -1.0f);
            glm::vec3 p100( 1.0f, -1.0f, -1.0f);
            glm::vec3 p101( 1.0f, -1.0f,  1.0f);
            glm::vec3 apex( 0.0f,  1.0f,  0.0f);

            glm::vec3 cBase1(0.8f, 0.8f, 0.2f);
            glm::vec3 cBase2(0.2f, 0.2f, 0.8f);
            glm::vec3 cSide1(0.2f, 0.8f, 0.2f);
            glm::vec3 cSide2(0.8f, 0.2f, 0.8f);
            glm::vec3 cSide3(0.8f, 0.8f, 0.2f);
            glm::vec3 cSide4(0.2f, 0.8f, 0.8f);

            glm::vec2 uv0(0.0f, 0.0f);
            glm::vec2 uv1(1.0f, 0.0f);
            glm::vec2 uv2(0.0f, 1.0f);

            std::vector<Vertex> verts;
            verts.reserve(18 + 6);

            auto addTriangle = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                                   const glm::vec3& ca, const glm::vec3& cb, const glm::vec3& cc) {
                glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
                verts.push_back({ a, ca, n, uv0 });
                verts.push_back({ b, cb, n, uv1 });
                verts.push_back({ c, cc, n, uv2 });
            };

            addTriangle(p000, p001, p100, cBase1, cBase1, cBase1);
            addTriangle(p000, p100, p101, cBase2, cBase2, cBase2);
            addTriangle(p000, p101, apex, cSide1, cSide1, cSide1);
            addTriangle(p101, p100, apex, cSide2, cSide2, cSide2);
            addTriangle(p100, p001, apex, cSide3, cSide3, cSide3);
            addTriangle(p001, p000, apex, cSide4, cSide4, cSide4);

            g_pyramid_vertex_count = (uint32_t)verts.size();
            g_floor_first_vertex   = g_pyramid_vertex_count;

            float y = -1.001f;
            float s = 5.0f;
            glm::vec3 f0(-s, y,  s);
            glm::vec3 f1( s, y,  s);
            glm::vec3 f2( s, y, -s);
            glm::vec3 f3(-s, y, -s);
            glm::vec3 fc(1.0f, 1.0f, 1.0f);
            glm::vec3 fn(0.0f, 1.0f, 0.0f);
            glm::vec2 t0(0.0f, 0.0f), t1(4.0f, 0.0f), t2(4.0f, 4.0f), t3(0.0f, 4.0f);

            verts.push_back({ f0, fc, fn, t0 });
            verts.push_back({ f1, fc, fn, t1 });
            verts.push_back({ f2, fc, fn, t2 });
            verts.push_back({ f0, fc, fn, t0 });
            verts.push_back({ f2, fc, fn, t2 });
            verts.push_back({ f3, fc, fn, t3 });

            g_floor_vertex_count = (uint32_t)verts.size() - g_floor_first_vertex;
            return verts;
        }

        void buildScene() {
            auto verts = buildAllVertices();
            uploadVerticesToVRAM(verts);
        }

        bool createPipeline() {
            auto& ctx = graphics::internal::context;
            auto vertCode = readFile("shader.vert.spv");
            auto fragCode = readFile("shader.frag.spv");
            if (vertCode.empty() || fragCode.empty()) return false;

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
            rs.cullMode = VK_CULL_MODE_BACK_BIT; // Для нормального блика
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

            VkDescriptorSetLayoutBinding bindings[3]{};
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[2].binding = 2;
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo slInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slInfo.bindingCount = 3;
            slInfo.pBindings = bindings;
            vkCreateDescriptorSetLayout(ctx.device, &slInfo, nullptr, &ctx.descriptor_set_layout);

            VkPipelineLayoutCreateInfo plInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plInfo.setLayoutCount = 1;
            plInfo.pSetLayouts = &ctx.descriptor_set_layout;
            vkCreatePipelineLayout(ctx.device, &plInfo, nullptr, &ctx.pipeline_layout);

            VkPipelineRenderingCreateInfo renderingInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachmentFormats = &ctx.swapchain_format;
            renderingInfo.depthAttachmentFormat = VK_FORMAT_D16_UNORM;

            VkGraphicsPipelineCreateInfo pInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            pInfo.pNext = &renderingInfo;
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
            pInfo.renderPass = VK_NULL_HANDLE;
            pInfo.subpass = 0;

            if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pInfo, nullptr, &ctx.graphics_pipeline) != VK_SUCCESS) return false;

            ctx.uniform_stride = sizeof(UBO);
            VkDeviceSize ubSize = 2 * ctx.uniform_stride;
            VkBufferCreateInfo ubInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ubInfo.size = ubSize;
            ubInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo uaInfo{};
            uaInfo.usage = VMA_MEMORY_USAGE_AUTO;
            uaInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo ubMap{};
            vmaCreateBuffer(ctx.allocator, &ubInfo, &uaInfo, &ctx.uniform_buffer, &ctx.uniform_allocation, &ubMap);
            ctx.uniform_mapped = ubMap.pMappedData;

            VkDescriptorPoolSize poolSizes[2]{};
            poolSizes[0] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 };
            poolSizes[1] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
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

            VkDescriptorBufferInfo bufInfo{ ctx.uniform_buffer, 0, ctx.uniform_stride };
            VkDescriptorImageInfo albedoInfo{};
            albedoInfo.sampler = ctx.albedo_sampler;
            albedoInfo.imageView = ctx.albedo_image_view;
            albedoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorImageInfo shadowInfo{};
            shadowInfo.sampler = ctx.shadow_sampler;
            shadowInfo.imageView = ctx.shadow_view;
            shadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet writes[3]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = ctx.descriptor_set;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[0].pBufferInfo = &bufInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = ctx.descriptor_set;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &albedoInfo;
            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = ctx.descriptor_set;
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].pImageInfo = &shadowInfo;
            vkUpdateDescriptorSets(ctx.device, 3, writes, 0, nullptr);
            return true;
        }

        bool createShadowPipeline() {
            auto& ctx = graphics::internal::context;
            auto shadowVertCode = readFile("shadow.vert.spv");
            if (shadowVertCode.empty()) return false;
            ctx.shadow_vert_shader_module = createShaderModule(shadowVertCode);

            VkPipelineShaderStageCreateInfo stage{};
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
            stage.module = ctx.shadow_vert_shader_module;
            stage.pName = "main";

            VkVertexInputBindingDescription binding{};
            binding.binding = 0;
            binding.stride = sizeof(Vertex);
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            VkVertexInputAttributeDescription attr{};
            attr.location = 0;
            attr.binding = 0;
            attr.format = VK_FORMAT_R32G32B32_SFLOAT;
            attr.offset = offsetof(Vertex, pos);
            VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &binding;
            vi.vertexAttributeDescriptionCount = 1;
            vi.pVertexAttributeDescriptions = &attr;
            VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            vp.viewportCount = 1;
            vp.scissorCount = 1;
            
            VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE; // Отключаем отсечение граней для теней!
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth = 1.0f;
            
            // ЖЕСТКИЙ БОЛЬШОЙ BIAS ЧТОБЫ УБРАТЬ САМОЗАТЕНЕНИЕ ПИРАМИДЫ
            rs.depthBiasEnable = VK_TRUE;
            rs.depthBiasConstantFactor = 4.0f;
            rs.depthBiasSlopeFactor = 4.0f;
            
            VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            ds.depthTestEnable = VK_TRUE;
            ds.depthWriteEnable = VK_TRUE;
            ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            
            VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dynState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynState.dynamicStateCount = 2;
            dynState.pDynamicStates = dyn;

            VkDescriptorSetLayoutBinding bindingUBO{};
            bindingUBO.binding = 0;
            bindingUBO.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            bindingUBO.descriptorCount = 1;
            bindingUBO.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            VkDescriptorSetLayoutCreateInfo slInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slInfo.bindingCount = 1;
            slInfo.pBindings = &bindingUBO;
            vkCreateDescriptorSetLayout(ctx.device, &slInfo, nullptr, &ctx.shadow_descriptor_set_layout);

            VkPipelineLayoutCreateInfo plInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plInfo.setLayoutCount = 1;
            plInfo.pSetLayouts = &ctx.shadow_descriptor_set_layout;
            vkCreatePipelineLayout(ctx.device, &plInfo, nullptr, &ctx.shadow_pipeline_layout);

            VkPipelineRenderingCreateInfo renderingInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
            renderingInfo.colorAttachmentCount = 0;
            renderingInfo.pColorAttachmentFormats = nullptr;
            renderingInfo.depthAttachmentFormat = VK_FORMAT_D16_UNORM;

            VkGraphicsPipelineCreateInfo pInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            pInfo.pNext = &renderingInfo;
            pInfo.stageCount = 1;
            pInfo.pStages = &stage;
            pInfo.pVertexInputState = &vi;
            pInfo.pInputAssemblyState = &ia;
            pInfo.pViewportState = &vp;
            pInfo.pRasterizationState = &rs;
            pInfo.pMultisampleState = &ms;
            pInfo.pDepthStencilState = &ds;
            pInfo.pColorBlendState = &cb;
            pInfo.pDynamicState = &dynState;
            pInfo.layout = ctx.shadow_pipeline_layout;
            pInfo.renderPass = VK_NULL_HANDLE;
            pInfo.subpass = 0;
            if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pInfo, nullptr, &ctx.shadow_pipeline) != VK_SUCCESS) return false;

            ctx.shadow_uniform_stride = sizeof(ShadowUBO);
            VkDeviceSize ubSize = 2 * ctx.shadow_uniform_stride;
            VkBufferCreateInfo ubInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ubInfo.size = ubSize;
            ubInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo uaInfo{};
            uaInfo.usage = VMA_MEMORY_USAGE_AUTO;
            uaInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo ubMap{};
            vmaCreateBuffer(ctx.allocator, &ubInfo, &uaInfo, &ctx.shadow_uniform_buffer, &ctx.shadow_uniform_allocation, &ubMap);
            ctx.shadow_uniform_mapped = ubMap.pMappedData;

            VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 };
            VkDescriptorPoolCreateInfo dpInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dpInfo.maxSets = 1;
            dpInfo.poolSizeCount = 1;
            dpInfo.pPoolSizes = &poolSize;
            vkCreateDescriptorPool(ctx.device, &dpInfo, nullptr, &ctx.shadow_descriptor_pool);

            VkDescriptorSetAllocateInfo dsInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsInfo.descriptorPool = ctx.shadow_descriptor_pool;
            dsInfo.descriptorSetCount = 1;
            dsInfo.pSetLayouts = &ctx.shadow_descriptor_set_layout;
            vkAllocateDescriptorSets(ctx.device, &dsInfo, &ctx.shadow_descriptor_set);

            VkDescriptorBufferInfo bufInfo{ ctx.shadow_uniform_buffer, 0, ctx.shadow_uniform_stride };
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = ctx.shadow_descriptor_set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            write.pBufferInfo = &bufInfo;
            vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
            return true;
        }

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

    bool initialize() {
        if (!loadTexture()) return false;
        buildScene();
        if (!createPipeline()) return false;
        return createShadowPipeline();
    }

    void shutdown() {
        auto& ctx = graphics::internal::context;
        vkDeviceWaitIdle(ctx.device);
        if (ctx.albedo_sampler)    vkDestroySampler(ctx.device, ctx.albedo_sampler, nullptr);
        if (ctx.albedo_image_view) vkDestroyImageView(ctx.device, ctx.albedo_image_view, nullptr);
        if (ctx.albedo_image)      vmaDestroyImage(ctx.allocator, ctx.albedo_image, ctx.albedo_image_allocation);
        if (ctx.vertex_buffer)     vmaDestroyBuffer(ctx.allocator, ctx.vertex_buffer, ctx.vertex_allocation);
        if (ctx.uniform_buffer)    vmaDestroyBuffer(ctx.allocator, ctx.uniform_buffer, ctx.uniform_allocation);
        if (ctx.descriptor_pool)   vkDestroyDescriptorPool(ctx.device, ctx.descriptor_pool, nullptr);
        if (ctx.descriptor_set_layout) vkDestroyDescriptorSetLayout(ctx.device, ctx.descriptor_set_layout, nullptr);
        if (ctx.graphics_pipeline) vkDestroyPipeline(ctx.device, ctx.graphics_pipeline, nullptr);
        if (ctx.pipeline_layout)   vkDestroyPipelineLayout(ctx.device, ctx.pipeline_layout, nullptr);
        if (ctx.vert_shader_module) vkDestroyShaderModule(ctx.device, ctx.vert_shader_module, nullptr);
        if (ctx.frag_shader_module) vkDestroyShaderModule(ctx.device, ctx.frag_shader_module, nullptr);
        if (ctx.shadow_pipeline)   vkDestroyPipeline(ctx.device, ctx.shadow_pipeline, nullptr);
        if (ctx.shadow_pipeline_layout) vkDestroyPipelineLayout(ctx.device, ctx.shadow_pipeline_layout, nullptr);
        if (ctx.shadow_descriptor_pool) vkDestroyDescriptorPool(ctx.device, ctx.shadow_descriptor_pool, nullptr);
        if (ctx.shadow_descriptor_set_layout) vkDestroyDescriptorSetLayout(ctx.device, ctx.shadow_descriptor_set_layout, nullptr);
        if (ctx.shadow_uniform_buffer) vmaDestroyBuffer(ctx.allocator, ctx.shadow_uniform_buffer, ctx.shadow_uniform_allocation);
        if (ctx.shadow_vert_shader_module) vkDestroyShaderModule(ctx.device, ctx.shadow_vert_shader_module, nullptr);
    }

    void update(double time) {
        if (g_animPlaying) g_animTime += (float)time * g_animSpeed;
        ImGui::Begin("Лаба №4 — Тени (shadow mapping)");
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
        if (ImGui::CollapsingHeader("Тени (ЛР4)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Тени включены", &g_shadowsEnabled);
            ImGui::SliderFloat("Площадь ортоматрицы", &g_shadowOrthoSize, 1.0f, 20.0f);
            ImGui::SliderFloat("Depth bias", &g_shadowDepthBias, 0.0f, 0.1f, "%.4f");
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
            g_rotation = glm::vec3(0.0f, 0.0f, 0.0f);
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
            g_shadowOrthoSize = 8.0f;
            g_shadowDepthBias = 0.01f;
            g_shadowsEnabled  = true;
        }
        ImGui::End();
    }

    void render(const graphics::internal::FrameData& fd) {
        auto& ctx = graphics::internal::context;

        glm::vec3 animOffset(0.0f);
        if (g_animPlaying || g_animTime > 0.0f) {
            animOffset = glm::vec3(std::cos(g_animTime) * g_animRadius, std::sin(g_animTime) * 0.5f, std::sin(g_animTime) * g_animRadius);
        }

        glm::mat4 pyramidModel(1.0f);
        pyramidModel = glm::translate(pyramidModel, g_position + animOffset);
        pyramidModel = glm::rotate(pyramidModel, glm::radians(g_rotation.x), { 1,0,0 });
        pyramidModel = glm::rotate(pyramidModel, glm::radians(g_rotation.y), { 0,1,0 });
        pyramidModel = glm::rotate(pyramidModel, glm::radians(g_rotation.z), { 0,0,1 });
        pyramidModel = glm::scale(pyramidModel, g_scale);

        glm::mat4 floorModel(1.0f);

        glm::vec3 camForward = computeCameraForward();
        glm::vec3 camTarget  = g_cameraPos + camForward;
        glm::mat4 view = glm::lookAt(g_cameraPos, camTarget, glm::vec3(0, 1, 0));

        float aspect = (fd.extent.height > 0) ? (float)fd.extent.width / (float)fd.extent.height : 1.0f;

        glm::mat4 proj;
        if (g_projectionMode == 0) {
            float orthoSize = g_animRadius + 4.0f;
            proj = glm::orthoRH_ZO(-orthoSize * aspect, orthoSize * aspect, -orthoSize, orthoSize, 0.1f, 100.0f);
        } else {
            proj = glm::perspectiveRH_ZO(glm::radians(g_cameraFov), aspect, 0.1f, 100.0f);
        }
        proj[1][1] *= -1.0f;

        glm::vec3 sunDir = glm::normalize(g_lightDirection);
        glm::vec3 sunPos = -sunDir * 20.0f;
        glm::vec3 sunTarget(0.0f, 0.0f, 0.0f);
        glm::mat4 lightView = glm::lookAt(sunPos, sunTarget, glm::vec3(0, 1, 0));
        float hs = g_shadowOrthoSize;
        glm::mat4 lightProj = glm::orthoRH_ZO(-hs, hs, -hs, hs, 0.1f, 100.0f);
        glm::mat4 lightViewProj = lightProj * lightView;

        UBO uboP{};
        uboP.mvp = proj * view * pyramidModel;
        uboP.model = pyramidModel;
        uboP.camera_pos      = glm::vec4(g_cameraPos, 1.0f);
        uboP.light_direction = glm::vec4(sunDir, 0.0f);
        uboP.light_color     = glm::vec4(g_lightColor * g_lightIntensity, 1.0f);
        uboP.ambient_light   = glm::vec4(g_ambientColor, 1.0f);
        uboP.albedo          = glm::vec4(g_albedo, 1.0f);
        uboP.specular        = glm::vec4(g_specularColor, 1.0f);
        uboP.material        = glm::vec4(g_shininess, g_useTexture ? 1.0f : 0.0f, 0.0f, 0.0f);
        uboP.light_view_proj = lightViewProj;
        uboP.shadow_params   = glm::vec4(g_shadowDepthBias, g_shadowOrthoSize, g_shadowsEnabled ? 1.0f : 0.0f, 0.0f);

        UBO uboF{};
        uboF.mvp = proj * view * floorModel;
        uboF.model = floorModel;
        uboF.camera_pos      = uboP.camera_pos;
        uboF.light_direction = uboP.light_direction;
        uboF.light_color     = uboP.light_color;
        uboF.ambient_light   = uboP.ambient_light;
        uboF.albedo          = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        uboF.specular        = glm::vec4(0.2f, 0.2f, 0.2f, 1.0f);
        uboF.material        = glm::vec4(8.0f, g_useTexture ? 1.0f : 0.0f, 0.0f, 0.0f);
        uboF.light_view_proj = lightViewProj;
        uboF.shadow_params   = uboP.shadow_params;

        uint8_t* base = (uint8_t*)ctx.uniform_mapped;
        memcpy(base + 0 * ctx.uniform_stride, &uboP, sizeof(UBO));
        memcpy(base + 1 * ctx.uniform_stride, &uboF, sizeof(UBO));

        ShadowUBO sp{ lightViewProj, pyramidModel };
        ShadowUBO sf{ lightViewProj, floorModel };
        uint8_t* sbase = (uint8_t*)ctx.shadow_uniform_mapped;
        memcpy(sbase + 0 * ctx.shadow_uniform_stride, &sp, sizeof(ShadowUBO));
        memcpy(sbase + 1 * ctx.shadow_uniform_stride, &sf, sizeof(ShadowUBO));

        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(fd.command_buffer, &begin);

        {
            VkImageMemoryBarrier to_depth{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            to_depth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_depth.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            to_depth.srcAccessMask = 0;
            to_depth.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            to_depth.image = ctx.shadow_image;
            to_depth.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(fd.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_depth);

            VkRenderingAttachmentInfo depthAtt{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            depthAtt.imageView = ctx.shadow_view;
            depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAtt.clearValue.depthStencil = { 1.0f, 0 };

            VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea = { {0, 0}, ctx.shadow_extent };
            ri.layerCount = 1;
            ri.colorAttachmentCount = 0;
            ri.pDepthAttachment = &depthAtt;

            vkCmdBeginRendering(fd.command_buffer, &ri);

            VkViewport vp{ 0, 0, (float)ctx.shadow_extent.width, (float)ctx.shadow_extent.height, 0, 1 };
            VkRect2D sc{ {0,0}, ctx.shadow_extent };
            vkCmdSetViewport(fd.command_buffer, 0, 1, &vp);
            vkCmdSetScissor(fd.command_buffer, 0, 1, &sc);

            vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.shadow_pipeline);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &ctx.vertex_buffer, &offset);

            uint32_t offP = 0;
            vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                ctx.shadow_pipeline_layout, 0, 1, &ctx.shadow_descriptor_set, 1, &offP);
            vkCmdDraw(fd.command_buffer, g_pyramid_vertex_count, 1, 0, 0);
            
            vkCmdEndRendering(fd.command_buffer);

            VkImageMemoryBarrier to_sample{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            to_sample.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            to_sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            to_sample.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            to_sample.image = ctx.shadow_image;
            to_sample.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(fd.command_buffer, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_sample);
        }

        {
            VkImageMemoryBarrier to_color{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            to_color.srcAccessMask = 0;
            to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            to_color.image = fd.swapchain_image;
            to_color.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(fd.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_color);

            VkRenderingAttachmentInfo colorAtt{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            colorAtt.imageView = fd.swapchain_image_view;
            colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAtt.clearValue.color = { {0.05f, 0.05f, 0.07f, 1.0f} };

            VkRenderingAttachmentInfo depthAtt{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            depthAtt.imageView = fd.depth_image_view;
            depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAtt.clearValue.depthStencil = { 1.0f, 0 };

            VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea = { {0, 0}, fd.extent };
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &colorAtt;
            ri.pDepthAttachment = &depthAtt;

            VkImageMemoryBarrier to_depth{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            to_depth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_depth.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            to_depth.srcAccessMask = 0;
            to_depth.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            to_depth.image = fd.depth_image;
            to_depth.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(fd.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_depth);

            vkCmdBeginRendering(fd.command_buffer, &ri);

            VkViewport vp{ 0, 0, (float)fd.extent.width, (float)fd.extent.height, 0, 1 };
            VkRect2D sc{ {0,0}, fd.extent };
            vkCmdSetViewport(fd.command_buffer, 0, 1, &vp);
            vkCmdSetScissor(fd.command_buffer, 0, 1, &sc);

            vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.graphics_pipeline);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &ctx.vertex_buffer, &offset);

            uint32_t offP = 0;
            vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                ctx.pipeline_layout, 0, 1, &ctx.descriptor_set, 1, &offP);
            vkCmdDraw(fd.command_buffer, g_pyramid_vertex_count, 1, 0, 0);

            uint32_t offF = (uint32_t)ctx.uniform_stride;
            vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                ctx.pipeline_layout, 0, 1, &ctx.descriptor_set, 1, &offF);
            vkCmdDraw(fd.command_buffer, g_floor_vertex_count, 1, g_floor_first_vertex, 0);

            vkCmdEndRendering(fd.command_buffer);
        }

        vkEndCommandBuffer(fd.command_buffer);
    }

} // namespace application