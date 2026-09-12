#pragma once

#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

extern "C" {
#include <libavutil/hwcontext_drm.h>
}

#include "vk_window.hpp"
#include "vk_frame.hpp"
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

void check_vk_result(VkResult err)
{
    if (err == VK_SUCCESS)
        return;
    SDL_Log("[vulkan] Error: VkResult = %d\n", err);
    if (err < 0)
        abort();
}

class AppVk {
private:
    VkAllocationCallbacks*   g_Allocator = nullptr;
    VkInstance               g_Instance = VK_NULL_HANDLE;
    VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice                 g_Device = VK_NULL_HANDLE;
    uint32_t                 g_QueueFamily = (uint32_t)-1;
    VkQueue                  g_Queue = VK_NULL_HANDLE;
    VkPipelineCache          g_PipelineCache = VK_NULL_HANDLE;

    uint32_t                 g_MinImageCount = 2;
    bool                     g_SwapChainRebuild = false;
    VkWindow vkWindow{};
    FrameData frameData[1];
    int frame_idx = 0;

	int wnd_w = 0;
	int wnd_h = 0;
	float base_scale = 0.0;
//	XferPool xfer_pool;
	SwsContext *sws_ctx = nullptr;

    VkPhysicalDevice SelectPhysicalDevice(VkInstance instance)
    {
        uint32_t gpu_count;
        VkResult err = vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
        check_vk_result(err);
        IM_ASSERT(gpu_count > 0);

        ImVector<VkPhysicalDevice> gpus;
        gpus.resize(gpu_count);
        err = vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.Data);
        check_vk_result(err);

        // If a number >1 of GPUs got reported, find discrete GPU if present, or use first one available. This covers
        // most common cases (multi-gpu/integrated+dedicated graphics). Handling more complicated setups (multiple
        // dedicated GPUs) is out of scope of this sample.
        for (VkPhysicalDevice& device : gpus)
        {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(device, &properties);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                return device;
        }

        // Use first GPU (Integrated) is a Discrete one is not available.
        if (gpu_count > 0)
            return gpus[0];
        return VK_NULL_HANDLE;
    }

    static uint32_t SelectQueueFamilyIndex(VkPhysicalDevice physical_device)
    {
        uint32_t count;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, nullptr);
        ImVector<VkQueueFamilyProperties> queues_properties;
        queues_properties.resize((int)count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, queues_properties.Data);
        for (uint32_t i = 0; i < count; i++)
            if (queues_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                return i;
        return (uint32_t)-1;
    }

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
        g_PhysicalDevice = SelectPhysicalDevice(g_Instance);
        IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);

        // Select graphics queue family
        g_QueueFamily = SelectQueueFamilyIndex(g_PhysicalDevice);
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
    }

    void SetupVulkanWindow(VkWindow *wd, VkSurfaceKHR surface, int width, int height)
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
        wd->SurfaceFormat = vkWindow.SelectSurfaceFormat(g_PhysicalDevice, surface, requestSurfaceImageFormat, (size_t)IM_COUNTOF(requestSurfaceImageFormat), requestSurfaceColorSpace);

        // Select Present Mode
    #ifdef APP_USE_UNLIMITED_FRAME_RATE
        VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
    #else
        VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
    #endif
        wd->PresentMode = wd->SelectPresentMode(g_PhysicalDevice, surface, &present_modes[0], IM_COUNTOF(present_modes));
        //printf("[vulkan] Selected PresentMode = %d\n", wd->PresentMode);

        // Create SwapChain, RenderPass, Framebuffer, etc.
        IM_ASSERT(g_MinImageCount >= 2);
        wd->CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, g_QueueFamily, g_Allocator, width, height, g_MinImageCount, 0);
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

    void FramePresent(VkWindow* wd)
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

    static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties, const char* extension)
    {
        for (const VkExtensionProperties& p : properties)
            if (strcmp(p.extensionName, extension) == 0)
                return true;
        return false;
    }

	void create_texture(AVFrame *frame)
	{
        if (FrameData::init_static(frame)) {
            for (auto data : frameData) {
                data.init(frame, g_Device, g_Allocator, g_QueueFamily);
            }
        }
		reset_scale();
	}

public:
    float video_scale = 1.0;
    float video_pan_x = 0.0;
    float video_pan_y = 0.0;
	auto get_pix_fmt() { return AV_PIX_FMT_NONE; }

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
        SetupVulkanWindow(&vkWindow, surface, wnd_w, wnd_h);

        return true;
    }

    void shutdown() {
        for (auto data : frameData) {
            data.destroy(g_Device, g_Allocator);
        }
        vkWindow.Destroy(g_Instance, g_Device, g_Allocator);
        vkDestroySurfaceKHR(g_Instance, vkWindow.Surface, g_Allocator);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        // Remove the debug report callback
        auto f_vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugReportCallbackEXT");
        f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif // APP_USE_VULKAN_DEBUG_REPORT

        vkDestroyDevice(g_Device, g_Allocator);
        vkDestroyInstance(g_Instance, g_Allocator);
    }

    void set_frame(AVFrame *frame, double play_time, AppSub sub) {
        frameData[frame_idx].upload(frame, g_Queue, g_Device, g_Allocator);
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