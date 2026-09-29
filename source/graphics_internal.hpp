#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

#include <vk_mem_alloc.h>

struct GLFWwindow;

namespace graphics::internal {

struct Context {
	VkPhysicalDevice physical_device;
	VkDevice device;

	VmaAllocator allocator;

	VkQueue graphics_queue;
	uint32_t graphics_queue_index;

	VkFormat swapchain_format;
	VkExtent2D swapchain_extent;

	VkRenderPass render_pass;

	// === 3D-пайплайн ===
	VkPipeline            graphics_pipeline     = VK_NULL_HANDLE;
	VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
	VkDescriptorPool      descriptor_pool       = VK_NULL_HANDLE;
	VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;

	// === Буфер вершин ===
	VkBuffer              vertex_buffer         = VK_NULL_HANDLE;
	VmaAllocation         vertex_allocation     = VK_NULL_HANDLE;
	uint32_t              vertex_count          = 0;

	// === Uniform-буфер ===
	VkBuffer              uniform_buffer        = VK_NULL_HANDLE;
	VmaAllocation         uniform_allocation    = VK_NULL_HANDLE;
	void*                 uniform_mapped        = nullptr;

	// === Shader-модули ===
	VkShaderModule        vert_shader_module    = VK_NULL_HANDLE;
	VkShaderModule        frag_shader_module    = VK_NULL_HANDLE;
};

struct FrameData {
	VkFramebuffer framebuffer;
	VkCommandBuffer command_buffer;
};

extern Context context;

bool initialize(GLFWwindow* const window);
void shutdown();

void resize(uint32_t width, uint32_t height);

FrameData prepare();
void submitAndPresent();

} // namespace graphics::internal