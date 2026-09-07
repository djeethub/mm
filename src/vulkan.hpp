#pragma once

#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

extern "C" {
#include <libavutil/hwcontext_drm.h>
}

#include "subtitle.hpp"

#include "vert.vert.h"
#include "nv12.frag.h"
#include "rgb.frag.h"
#include "yuv.frag.h"
#include "gray.frag.h"
#include "pal8.frag.h"
#include "yuv_10.frag.h"
#include "yuyv.frag.h"
#include "ya.frag.h"
#include "yuva.frag.h"

enum ShaderType
{
	VERT,
	RGB_FRAG,
	NV12_FRAG,
	YUV_FRAG,
	GRAY_FRAG,
	PAL8_FRAG,
	YUV_10_FRAG,
	YUYV_FRAG,
	YA_FRAG,
	YUVA_FRAG
};

struct alignas(16) Vertform {
    float position[2]; // x, y (in NDC: -1.0 to 1.0)
    float size[2];     // width, height (in NDC: 0.0 to 2.0)
};

struct alignas(16) Uniforms
{
	float tex_size[2]; // width, height of Y plane
	int32_t color_range; // 1 = full range, 0 = limited
	int32_t colorspace;	 // 1 = 709, 9,10 = 2090
};

struct XferData {
	SDL_GPUTransferBuffer *buf;
	Uint32 size;

	void destroy(SDL_GPUDevice *device) {
		if (buf)
			SDL_ReleaseGPUTransferBuffer(device, buf);
		delete this;
	}

	void reset() {}
};

class XferPool : public GPUPool<XferData> {
public:
	XferData *alloc(Uint32 size) {
		if (!list.empty()) {
			auto data = list.back();
			list.pop_back();
			if (data->size >= size) {
				return data;
			}
			data->destroy(device);
		}

		SDL_GPUTransferBufferCreateInfo tb_info = {
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = size,
		};
		SDL_GPUTransferBuffer *buf = SDL_CreateGPUTransferBuffer(device, &tb_info);
		if (!buf)
			return nullptr;
		auto data = new XferData{.buf = buf, .size = size};
		return data;
	}
};

class AppVk {
private:
    VkAllocationCallbacks*   g_Allocator = nullptr;
    VkInstance               g_Instance = VK_NULL_HANDLE;
    VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice                 g_Device = VK_NULL_HANDLE;
    uint32_t                 g_QueueFamily = (uint32_t)-1;
    VkQueue                  g_Queue = VK_NULL_HANDLE;
    VkPipelineCache          g_PipelineCache = VK_NULL_HANDLE;
    VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;

    ImGui_ImplVulkanH_Window g_MainWindowData;
    uint32_t                 g_MinImageCount = 2;
    bool                     g_SwapChainRebuild = false;

	AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
	const AVPixFmtDescriptor *fmt_desc;
	int width = 0;
	int height = 0;
	int de_width = 2;
	int de_height = 2;
	int wnd_w = 0;
	int wnd_h = 0;
	float base_scale = 0.0;
	AVFrame *frame = nullptr;
	int n_bindings = 0;
	int bpp;
	XferPool xfer_pool;
	SwsContext *sws_ctx = nullptr;
    VkImage image;

    void SetupVulkan(ImVector<const char*> instance_extensions)
    {
        VkResult err;
    #ifdef IMGUI_IMPL_VULKAN_USE_VOLK
        volkInitialize();
    #endif

        // Create Vulkan Instance
        {
            VkInstanceCreateInfo create_info = {};
            create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

            // Enumerate available extensions
            uint32_t properties_count;
            ImVector<VkExtensionProperties> properties;
            vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
            properties.resize(properties_count);
            err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
            check_vk_result(err);

            // Enable required extensions
            if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
                instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    #ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
            if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
            {
                instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
                create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            }
    #endif

            // Enabling validation layers
    #ifdef APP_USE_VULKAN_DEBUG_REPORT
            const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
            create_info.enabledLayerCount = 1;
            create_info.ppEnabledLayerNames = layers;
            instance_extensions.push_back("VK_EXT_debug_report");
    #endif

            // Create Vulkan Instance
            create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
            create_info.ppEnabledExtensionNames = instance_extensions.Data;
            err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
            check_vk_result(err);
    #ifdef IMGUI_IMPL_VULKAN_USE_VOLK
            volkLoadInstance(g_Instance);
    #endif

            // Setup the debug report callback
    #ifdef APP_USE_VULKAN_DEBUG_REPORT
            auto f_vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkCreateDebugReportCallbackEXT");
            IM_ASSERT(f_vkCreateDebugReportCallbackEXT != nullptr);
            VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
            debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
            debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
            debug_report_ci.pfnCallback = debug_report;
            debug_report_ci.pUserData = nullptr;
            err = f_vkCreateDebugReportCallbackEXT(g_Instance, &debug_report_ci, g_Allocator, &g_DebugReport);
            check_vk_result(err);
    #endif
        }

        // Select Physical Device (GPU)
        g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
        IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);

        // Select graphics queue family
        g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
        IM_ASSERT(g_QueueFamily != (uint32_t)-1);

        // Create Logical Device (with 1 queue)
        {
            ImVector<const char*> device_extensions;
            device_extensions.push_back("VK_KHR_swapchain");

            // Enumerate physical device extension
            uint32_t properties_count;
            ImVector<VkExtensionProperties> properties;
            vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, nullptr);
            properties.resize(properties_count);
            vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, properties.Data);
    #ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
            if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
                device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    #endif

            const float queue_priority[] = { 1.0f };
            VkDeviceQueueCreateInfo queue_info[1] = {};
            queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_info[0].queueFamilyIndex = g_QueueFamily;
            queue_info[0].queueCount = 1;
            queue_info[0].pQueuePriorities = queue_priority;
            VkDeviceCreateInfo create_info = {};
            create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            create_info.queueCreateInfoCount = sizeof(queue_info) / sizeof(queue_info[0]);
            create_info.pQueueCreateInfos = queue_info;
            create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
            create_info.ppEnabledExtensionNames = device_extensions.Data;
            err = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_Device);
            check_vk_result(err);
            vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
        }

        // Create Descriptor Pool
        // If you wish to load e.g. additional textures you may need to alter pools sizes and maxSets.
        {
            VkDescriptorPoolSize pool_sizes[] =
            {
                { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE },
                { VK_DESCRIPTOR_TYPE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE },
            };
            VkDescriptorPoolCreateInfo pool_info = {};
            pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            pool_info.maxSets = 0;
            for (VkDescriptorPoolSize& pool_size : pool_sizes)
                pool_info.maxSets += pool_size.descriptorCount;
            pool_info.poolSizeCount = (uint32_t)IM_COUNTOF(pool_sizes);
            pool_info.pPoolSizes = pool_sizes;
            err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
            check_vk_result(err);
        }
    }

    // All the ImGui_ImplVulkanH_XXX structures/functions are optional helpers used by the demo.
    // Your real engine/app may not use them.
    void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height)
    {
        // Check for WSI support
        VkBool32 res;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, surface, &res);
        if (res != VK_TRUE)
        {
            fprintf(stderr, "Error no WSI support on physical device 0\n");
            exit(-1);
        }

        // Select Surface Format
        const VkFormat requestSurfaceImageFormat[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
        const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
        wd->Surface = surface;
        wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat, (size_t)IM_COUNTOF(requestSurfaceImageFormat), requestSurfaceColorSpace);

        // Select Present Mode
    #ifdef APP_USE_UNLIMITED_FRAME_RATE
        VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
    #else
        VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
    #endif
        wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, wd->Surface, &present_modes[0], IM_COUNTOF(present_modes));
        //printf("[vulkan] Selected PresentMode = %d\n", wd->PresentMode);

        // Create SwapChain, RenderPass, Framebuffer, etc.
        IM_ASSERT(g_MinImageCount >= 2);
        ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount, 0);
    }

    void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data)
    {
        VkSemaphore image_acquired_semaphore  = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
        VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
        VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
        if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
            g_SwapChainRebuild = true;
        if (err == VK_ERROR_OUT_OF_DATE_KHR)
            return;
        if (err != VK_SUBOPTIMAL_KHR)
            check_vk_result(err);

        ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
        {
            err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);    // wait indefinitely instead of periodically checking
            check_vk_result(err);

            err = vkResetFences(g_Device, 1, &fd->Fence);
            check_vk_result(err);
        }
        {
            err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
            check_vk_result(err);
            VkCommandBufferBeginInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
            check_vk_result(err);
        }
        {
            VkRenderPassBeginInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            info.renderPass = wd->RenderPass;
            info.framebuffer = fd->Framebuffer;
            info.renderArea.extent.width = wd->Width;
            info.renderArea.extent.height = wd->Height;
            info.clearValueCount = 1;
            info.pClearValues = &wd->ClearValue;
            vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
        }

        // Record dear imgui primitives into command buffer
        ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

        // Submit command buffer
        vkCmdEndRenderPass(fd->CommandBuffer);
        {
            VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores = &image_acquired_semaphore;
            info.pWaitDstStageMask = &wait_stage;
            info.commandBufferCount = 1;
            info.pCommandBuffers = &fd->CommandBuffer;
            info.signalSemaphoreCount = 1;
            info.pSignalSemaphores = &render_complete_semaphore;

            err = vkEndCommandBuffer(fd->CommandBuffer);
            check_vk_result(err);
            err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
            check_vk_result(err);
        }
    }

    void FramePresent(ImGui_ImplVulkanH_Window* wd)
    {
        if (g_SwapChainRebuild)
            return;
        VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
        VkPresentInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &render_complete_semaphore;
        info.swapchainCount = 1;
        info.pSwapchains = &wd->Swapchain;
        info.pImageIndices = &wd->FrameIndex;
        VkResult err = vkQueuePresentKHR(g_Queue, &info);
        if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
            g_SwapChainRebuild = true;
        if (err == VK_ERROR_OUT_OF_DATE_KHR)
            return;
        if (err != VK_SUBOPTIMAL_KHR)
            check_vk_result(err);
        wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount; // Now we can use the next set of semaphores
    }

public:
    float video_scale = 1.0;
    float video_pan_x = 0.0;
    float video_pan_y = 0.0;
	auto get_pix_fmt() { return AV_PIX_FMT_NONE; }

    static void check_vk_result(VkResult err)
    {
        if (err == VK_SUCCESS)
            return;
        SDL_Log("[vulkan] Error: VkResult = %d\n", err);
        if (err < 0)
            abort();
    }

    static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties, const char* extension)
    {
        for (const VkExtensionProperties& p : properties)
            if (strcmp(p.extensionName, extension) == 0)
                return true;
        return false;
    }

    void set_frame(AVFrame *frame, double play_time, AppSub sub) {
        if (frame->hw_frames_ctx) {
            AVDRMFrameDescriptor *drm_desc = (AVDRMFrameDescriptor*)frame->data[0];

            int dma_fd = drm_desc->objects[0].fd; 
            uint64_t modifier = drm_desc->objects[0].format_modifier; // AMD Tiling information

            VkExternalMemoryImageCreateInfo extImageInfo = {
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
            };

            VkImageDrmFormatModifierExplicitCreateInfoEXT modInfo = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
                .pNext = &extImageInfo,
                .drmFormatModifier = modifier,
                // Provide plane offsets and strides collected from drm_desc->layers
            };

            VkImageCreateInfo imgInfo = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = &modInfo,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, // Representing NV12 in Vulkan
                .extent = { (uint32_t) frame->width, (uint32_t) frame->height, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,          // or LINEAR if needed
                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
            };
            auto err = vkCreateImage(g_Device, &imgInfo, nullptr, &image);
            if (err != VK_SUCCESS)
                return;
            
            // Memory requirements
            VkMemoryRequirements mem_req;
            vkGetImageMemoryRequirements(g_Device, image, &mem_req);

            // Import the DMA-BUF
            VkImportMemoryFdInfoKHR import_info = {
                .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                .fd = dma_fd          // ownership is transferred to Vulkan
            };

            VkMemoryAllocateInfo alloc_info = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &import_info,
                .allocationSize = mem_req.size,
                .memoryTypeIndex = /* find a type that supports DEVICE_LOCAL + the external handle */
            };

            // Find suitable memory type that supports the external handle
            VkPhysicalDeviceMemoryProperties mem_props;
            vkGetPhysicalDeviceMemoryProperties(phys_dev, &mem_props);
            // … (standard loop to pick memoryTypeIndex)

            VkDeviceMemory memory;
            res = vkAllocateMemory(device, &alloc_info, nullptr, &memory);
            if (res != VK_SUCCESS) {
                vkDestroyImage(device, image, nullptr);
                return res;
            }

            res = vkBindImageMemory(device, image, memory, 0);
            if (res != VK_SUCCESS) {
                vkFreeMemory(device, memory, nullptr);
                vkDestroyImage(device, image, nullptr);
                return res;
            }
        }
    }

	void create_texture(AVFrame *frame)
	{
		if (frame->format == pix_fmt && frame->width == width && frame->height == height)
			return;

		destroy_textures();
		pix_fmt = static_cast<AVPixelFormat>(frame->format);
		fmt_desc = av_pix_fmt_desc_get(pix_fmt);
//		SDL_Log("out_pix_fmt: %s\n", av_get_pix_fmt_name(pix_fmt));
		init_pipeline();
		width = frame->width;
		height = frame->height;
		reset_scale();

		if (n_bindings == 1) {
			if (fmt_desc->nb_components == 1) {
				yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
				if (fmt_desc->flags & AV_PIX_FMT_FLAG_PAL)
					uTexture = create_plane_texture(256, 1, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
			} else if (fmt_desc->nb_components == 2) {
				yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM);
			} else {
				switch (pix_fmt)
				{
				case AV_PIX_FMT_BGR24:
					setup_sws_context(pix_fmt, AV_PIX_FMT_BGRA);
				case AV_PIX_FMT_BGRA:
					yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
					break;

				case AV_PIX_FMT_RGB24:
					setup_sws_context(pix_fmt, AV_PIX_FMT_RGBA);
				default:
					yTexture = create_plane_texture(width / de_width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
				}
			}
		} else if (n_bindings == 2) {
			if (bpp > 1) {
				yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R16_UNORM);
				uTexture = create_plane_texture(width / de_width, height / de_height, SDL_GPU_TEXTUREFORMAT_R16G16_UNORM);
			} else {
				yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
				uTexture = create_plane_texture(width / de_width, height / de_height, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM);
			}
		} else {
			if (bpp > 1) {
				yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R16_UNORM);
				uTexture = create_plane_texture(width / de_width, height / de_height, SDL_GPU_TEXTUREFORMAT_R16_UNORM);
				vTexture = create_plane_texture(width / de_width, height / de_height, SDL_GPU_TEXTUREFORMAT_R16_UNORM);
				if (n_bindings > 3)
					aTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R16_UNORM);
			} else {
				yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
				uTexture = create_plane_texture(width / de_width, height / de_height, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
				vTexture = create_plane_texture(width / de_width, height / de_height, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
				if (n_bindings > 3)
					aTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
			}
		}
	}

    bool init_pipeline() {
    }

    bool init(SDL_Window *window) {
        ImVector<const char*> extensions;
        {
            uint32_t sdl_extensions_count = 0;
            const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extensions_count);
            for (uint32_t n = 0; n < sdl_extensions_count; n++)
                extensions.push_back(sdl_extensions[n]);
        }
        SetupVulkan(extensions);

        // Create Window Surface
        VkSurfaceKHR surface;
        VkResult err;
        if (SDL_Vulkan_CreateSurface(window, g_Instance, g_Allocator, &surface) == 0)
        {
            printf("Failed to create Vulkan surface.\n");
            return 1;
        }

        // Create Framebuffers
        SDL_GetWindowSizeInPixels(window, &wnd_w, &wnd_h);
        ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
        SetupVulkanWindow(wd, surface, wnd_w, wnd_h);

        ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
        wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
        wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
        wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
        wd->ClearValue.color.float32[3] = clear_color.w;

        return true;
    }

    void shutdown() {
        ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
        vkDestroySurfaceKHR(g_Instance, g_MainWindowData.Surface, g_Allocator);

        vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        // Remove the debug report callback
        auto f_vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugReportCallbackEXT");
        f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif // APP_USE_VULKAN_DEBUG_REPORT

        vkDestroyDevice(g_Device, g_Allocator);
        vkDestroyInstance(g_Instance, g_Allocator);
    }

    bool imgui_init() {
        ImGui_ImplVulkan_InitInfo init_info = {};
        //init_info.ApiVersion = VK_API_VERSION_1_3;              // Pass in your value of VkApplicationInfo::apiVersion, otherwise will default to header version.
        init_info.Instance = g_Instance;
        init_info.PhysicalDevice = g_PhysicalDevice;
        init_info.Device = g_Device;
        init_info.QueueFamily = g_QueueFamily;
        init_info.Queue = g_Queue;
        init_info.PipelineCache = g_PipelineCache;
        init_info.DescriptorPool = g_DescriptorPool;
        init_info.MinImageCount = g_MinImageCount;
        init_info.ImageCount = g_MainWindowData.ImageCount;
        init_info.Allocator = g_Allocator;
        init_info.PipelineInfoMain.RenderPass = g_MainWindowData.RenderPass;
        init_info.PipelineInfoMain.Subpass = 0;
        init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.CheckVkResultFn = check_vk_result;
        return ImGui_ImplVulkan_Init(&init_info);
    }

	void render(AppSub sub)
	{
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
        if (!is_minimized)
        {
            ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
            FrameRender(wd, draw_data);
            FramePresent(wd);
        }
	}

	void window_size_changed(Sint32 w, Sint32 h) {
        wnd_w = w;
        wnd_h = h;
    }

    void rebuild() {
        if ((g_SwapChainRebuild || g_MainWindowData.Width != wnd_w || g_MainWindowData.Height != wnd_h))
        {
            ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, wnd_w, wnd_h, g_MinImageCount, 0);
            g_MainWindowData.FrameIndex = 0;
            g_SwapChainRebuild = false;
        }
    }

    void reset_scale() {
        
    }

    void wait_for_idle() {
        auto err = vkDeviceWaitIdle(g_Device);
        check_vk_result(err);
    }
};