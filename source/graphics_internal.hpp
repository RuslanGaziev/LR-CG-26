#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include <vk_mem_alloc.h>

struct GLFWwindow;

namespace graphics::internal {

struct Context {
	VkPhysicalDevice physical_device;
	VkDevice device;
	VmaAllocator allocator;

	uint32_t graphics_queue_index;
	VkQueue graphics_queue;

	// Оставлен для совместимости: используется только ImGui-бэкендом.
	// Основная геометрия рендерится через dynamic rendering — без VkRenderPass.
	VkRenderPass render_pass;

	VkFormat   swapchain_format;
	VkExtent2D swapchain_extent;

	// === ЛР4: теневая карта ===
	VkImage       shadow_image;
	VmaAllocation shadow_allocation;
	VkImageView   shadow_view;
	VkSampler     shadow_sampler;
	VkExtent2D    shadow_extent;

	// === ЛР4: теневой пайплайн и его дескрипторы ===
	VkPipeline            shadow_pipeline;
	VkPipelineLayout      shadow_pipeline_layout;
	VkDescriptorSetLayout shadow_descriptor_set_layout;
	VkDescriptorPool      shadow_descriptor_pool;
	VkDescriptorSet       shadow_descriptor_set;
	VkBuffer              shadow_uniform_buffer;
	VmaAllocation         shadow_uniform_allocation;
	void*                 shadow_uniform_mapped;
	VkDeviceSize          shadow_uniform_stride;

	// === Основной пайплайн ===
	VkDescriptorSetLayout descriptor_set_layout;
	VkDescriptorPool      descriptor_pool;
	VkDescriptorSet       descriptor_set;
	VkPipelineLayout      pipeline_layout;
	VkPipeline            graphics_pipeline;
	VkBuffer              uniform_buffer;
	VmaAllocation         uniform_allocation;
	void*                 uniform_mapped;
	VkDeviceSize          uniform_stride;

	VkShaderModule vert_shader_module;
	VkShaderModule frag_shader_module;
	VkShaderModule shadow_vert_shader_module;

	// === Текстура (ЛР3) ===
	VkImage       albedo_image;
	VmaAllocation albedo_image_allocation;
	VkImageView   albedo_image_view;
	VkSampler     albedo_sampler;

	// === Геометрия (ЛР1–ЛР4) ===
	VkBuffer      vertex_buffer;
	VmaAllocation vertex_allocation;
	uint32_t      vertex_count;
};

extern Context context;

struct FrameData {
	VkFramebuffer   framebuffer;         // оставлен для совместимости (не используется)
	VkCommandBuffer command_buffer;
	VkImageView     swapchain_image_view;
	VkImage         swapchain_image;     // ЛР4: для барьера цвета
	VkImageView     depth_image_view;
	VkImage         depth_image;         // ЛР4: для барьера глубины
	VkExtent2D      extent;
};

bool initialize(GLFWwindow* window);
void shutdown();
void resize(uint32_t width, uint32_t height);
FrameData prepare();
void submitAndPresent();

} // namespace graphics::internal