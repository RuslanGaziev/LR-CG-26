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

        struct Vertex {
            glm::vec3 pos;
            glm::vec3 color;
        };

        struct UBO {
            glm::mat4 mvp;
            glm::vec4 color;
        };

        // --- Состояние UI ---
        glm::vec3 g_position(0.0f);
        glm::vec3 g_rotation(30.0f, 45.0f, 0.0f);
        glm::vec3 g_scale(1.0f);
        glm::vec3 g_color(1.0f, 0.6f, 0.2f);
        int       g_projectionMode = 1;

        // --- Анимация ---
        bool  g_animPlaying = true;
        float g_animSpeed = 0.002f;
        float g_animRadius = 2.0f;
        float g_animTime = 0.0f;

        // ---------- helpers ----------
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

        // ---------- Пирамида ----------
        void buildPyramid() {
            auto& ctx = graphics::internal::context;

            std::vector<Vertex> verts = {
                {{-1,-1, 1}, {0.2f,0.8f,0.2f}}, {{-1,-1,-1}, {0.8f,0.2f,0.8f}}, {{ 1,-1,-1}, {0.8f,0.8f,0.2f}},
                {{-1,-1, 1}, {0.2f,0.8f,0.2f}}, {{ 1,-1,-1}, {0.8f,0.8f,0.2f}}, {{ 1,-1, 1}, {0.2f,0.2f,0.8f}},
                {{ 0, 1, 0}, {1,0,0}}, {{-1,-1, 1}, {0.2f,0.8f,0.2f}}, {{ 1,-1, 1}, {0.2f,0.2f,0.8f}},
                {{ 0, 1, 0}, {1,0,0}}, {{ 1,-1, 1}, {0.2f,0.2f,0.8f}}, {{ 1,-1,-1}, {0.8f,0.8f,0.2f}},
                {{ 0, 1, 0}, {1,0,0}}, {{ 1,-1,-1}, {0.8f,0.8f,0.2f}}, {{-1,-1,-1}, {0.8f,0.2f,0.8f}},
                {{ 0, 1, 0}, {1,0,0}}, {{-1,-1,-1}, {0.8f,0.2f,0.8f}}, {{-1,-1, 1}, {0.2f,0.8f,0.2f}},
            };

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

        // ---------- Пайплайн ----------
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

            VkVertexInputAttributeDescription attrs[2]{};
            attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos) };
            attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color) };

            VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &binding;
            vi.vertexAttributeDescriptionCount = 2;
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

    void update(double time) {
        if (g_animPlaying) g_animTime += (float)time * g_animSpeed;

        ImGui::Begin("Лаба №1 — Пирамида");

        ImGui::Text("Проекция:");
        ImGui::RadioButton("Перспективная", &g_projectionMode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Ортографическая", &g_projectionMode, 0);

        ImGui::Separator();

        ImGui::SliderFloat3("Позиция", &g_position.x, -5.0f, 5.0f);
        ImGui::SliderFloat3("Поворот", &g_rotation.x, -180.0f, 180.0f);
        ImGui::SliderFloat3("Масштаб", &g_scale.x, 0.1f, 3.0f);

        ImGui::Separator();

        ImGui::Checkbox("Анимация", &g_animPlaying);
        ImGui::SliderFloat("Скорость", &g_animSpeed, 0.00000f, 0.50f);
        ImGui::SliderFloat("Радиус", &g_animRadius, 0.0f, 5.0f);

        ImGui::Separator();

        ImGui::ColorEdit3("Цвет", &g_color.x);

        ImGui::Separator();
        if (ImGui::Button("Сбросить всё")) {
            g_position       = glm::vec3(0.0f);
            g_rotation       = glm::vec3(30.0f, 45.0f, 0.0f);
            g_scale          = glm::vec3(1.0f);
            g_color          = glm::vec3(1.0f, 0.6f, 0.2f);
            g_animTime       = 0.0f;
            g_animSpeed      = 0.02f;
            g_animRadius     = 2.0f;
            g_projectionMode = 1;
        }

        ImGui::End();
    }

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

        glm::mat4 view = glm::lookAt(glm::vec3(0, 2, 6),
                                     glm::vec3(0, 0, 0),
                                     glm::vec3(0, 1, 0));

        float aspect = (ctx.swapchain_extent.height > 0)
            ? (float)ctx.swapchain_extent.width / (float)ctx.swapchain_extent.height
            : 1.0f;

        glm::mat4 proj;
        if (g_projectionMode == 0) {
            float orthoSize = g_animRadius + 4.0f;
            proj = glm::orthoRH_ZO(-orthoSize * aspect, orthoSize * aspect,
                                   -orthoSize, orthoSize,
                                   0.1f, 100.0f);
        } else {
            proj = glm::perspectiveRH_ZO(glm::radians(45.0f), aspect, 0.1f, 100.0f);
        }
        proj[1][1] *= -1.0f;

        UBO ubo{};
        ubo.mvp = proj * view * model;
        ubo.color = glm::vec4(g_color, 1.0f);
        memcpy(ctx.uniform_mapped, &ubo, sizeof(UBO));

        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(fd.command_buffer, &begin);

        VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rp.renderPass = ctx.render_pass;
        rp.framebuffer = fd.framebuffer;
        rp.renderArea = { {0,0}, ctx.swapchain_extent };

        VkClearValue clears[2]{};
        clears[0].color = { {0.1f, 0.1f, 0.12f, 1.0f} };
        clears[1].depthStencil = { 1.0f, 0 };
        rp.clearValueCount = 2;
        rp.pClearValues = clears;

        vkCmdBeginRenderPass(fd.command_buffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{ 0, 0,
            (float)ctx.swapchain_extent.width,
            (float)ctx.swapchain_extent.height,
            0, 1 };
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