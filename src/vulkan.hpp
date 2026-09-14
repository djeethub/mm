#pragma once

#include <print>

#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#include <vulkan/vulkan_raii.hpp>
#include <SDL3/SDL_vulkan.h>
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
    SDL_Log("[vulkan] Error: VkResult = %s\n", err);
    if (err < 0)
        abort();
}

static VKAPI_ATTR vk::Bool32 VKAPI_CALL debugCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    vk::DebugUtilsMessageTypeFlagsEXT messageType,
    const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) 
{
    // Filter or format your output here
    std::println("[Vulkan {}] {}", (int) messageSeverity, pCallbackData->pMessage);
//    std::cout << "[Vulkan Debug] " << pCallbackData->pMessage << std::endl;
    return vk::False; // Always return VK_FALSE unless you want to force-fail the call
}

class AppVk {
private:
    vk::raii::Instance instance = nullptr;
#ifndef NDEBUG
    vk::raii::DebugUtilsMessengerEXT debugMessenger = nullptr;
#endif
    vk::raii::PhysicalDevice physicalDevice = nullptr;
    vk::raii::Device device = nullptr;
    uint32_t queueFamily = (uint32_t)-1;
    vk::raii::Queue queue = nullptr;
    vk::raii::DescriptorPool descriptorPool = nullptr;    // for imgui

    uint32_t                 g_MinImageCount = 2;
    VkWindow vkWindow;
    FrameData frameData[1];
    int frame_idx = 0;

	int wnd_w = 0;
	int wnd_h = 0;
	float base_scale = 0.0;
//	XferPool xfer_pool;
	SwsContext *sws_ctx = nullptr;

    void SetupVulkan(ImVector<const char*> instance_extensions)
    {
        vk::raii::Context context;

        constexpr vk::ApplicationInfo appInfo{.pApplicationName   = "mm",
                                            .applicationVersion = VK_MAKE_VERSION( 1, 0, 0 ),
                                            .pEngineName        = "No Engine",
                                            .engineVersion      = VK_MAKE_VERSION( 1, 0, 0 ),
                                            .apiVersion         = vk::ApiVersion14};

        vk::InstanceCreateInfo createInfo{
            .pApplicationInfo = &appInfo,
        };

        auto properties = context.enumerateInstanceExtensionProperties();
        // Enable required extensions
        if (IsExtensionAvailable(properties, vk::KHRGetPhysicalDeviceProperties2ExtensionName))
            instance_extensions.push_back(vk::KHRGetPhysicalDeviceProperties2ExtensionName);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(properties, vk::KHRPortabilityEnumerationExtensionName))
        {
            instance_extensions.push_back(vk::KHRPortabilityEnumerationExtensionName);
            createInfo.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
        }
#endif
#ifndef NDEBUG
        if (IsExtensionAvailable(properties, vk::EXTDebugUtilsExtensionName)) {
            instance_extensions.push_back(vk::EXTDebugUtilsExtensionName);
        }
#endif

        // Enabling validation layers
#ifndef NDEBUG
        const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = layers;
        instance_extensions.push_back(vk::EXTDebugUtilsExtensionName);
#endif

        createInfo.enabledExtensionCount = (uint32_t)instance_extensions.Size;
        createInfo.ppEnabledExtensionNames = instance_extensions.Data;
        instance = context.createInstance(createInfo);

        // Setup the debug report callback
#ifndef NDEBUG
/*
        vk::DebugReportCallbackCreateInfoEXT debug_report_ci = {
            .flags = vk::DebugReportFlagsEXT::BitsType::eError | vk::DebugReportFlagsEXT::BitsType::eWarning | vk::DebugReportFlagsEXT::BitsType::ePerformanceWarning,
            .pfnCallback = debugCallback,
        };
        instance.createDebugReportCallbackEXT(debug_report_ci);
*/      
        vk::DebugUtilsMessengerCreateInfoEXT messengerInfo = {
            .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo    |
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
            .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral    |
                vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
            .pfnUserCallback = debugCallback,
        };
        debugMessenger = instance.createDebugUtilsMessengerEXT(messengerInfo);
#endif

        vk::raii::PhysicalDevices physicalDevices(instance);
        if (physicalDevices.empty()) {
            std::print("No Vulkan physical devices found!\n");
            return;
        }

        for (auto& x : physicalDevices) {
            vk::PhysicalDeviceProperties2 prop;
            x.getProperties2(&prop);
            if (prop.properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
                physicalDevice = x;
                break;
            }
        }
        if (!*physicalDevice)
            physicalDevice = physicalDevices[0];

        auto queueFamilyProperties2 = physicalDevice.getQueueFamilyProperties2();
        for (uint32_t i = 0; i < queueFamilyProperties2.size(); i++)
            if (queueFamilyProperties2[i].queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) {
                queueFamily = i;
                break;
            }
        assert(queueFamily != -1);

        std::vector<const char*> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");

        float queuePriority = 1.0f;
        vk::DeviceQueueCreateInfo queueCreateInfo = {
            .queueFamilyIndex = queueFamily,
            .queueCount = 1,
            .pQueuePriorities = &queuePriority,
        };
        vk::DeviceCreateInfo deviceCreateInfo = {
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queueCreateInfo,
            .enabledExtensionCount = (uint32_t) device_extensions.size(),
            .ppEnabledExtensionNames = device_extensions.data()
        };
        // Device automatically loads device-specific dispatchers and handles cleanup
        device = physicalDevice.createDevice(deviceCreateInfo);
        queue = device.getQueue(queueFamily, 0);

        vk::DescriptorPoolSize pool_sizes[] = {
            { vk::DescriptorType::eSampledImage, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE },
            { vk::DescriptorType::eSampler, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE },
        };
        vk::DescriptorPoolCreateInfo pool_info = {
            .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
            .poolSizeCount = (uint32_t)IM_COUNTOF(pool_sizes),
            .pPoolSizes = pool_sizes,
        };
        for (auto& pool_size : pool_sizes)
            pool_info.maxSets += pool_size.descriptorCount;
        descriptorPool = device.createDescriptorPool(pool_info);
    }

    VkSurfaceFormatKHR SelectSurfaceFormat(const vk::raii::PhysicalDevice& physical_device, const vk::raii::SurfaceKHR& surface, const vk::Format* request_formats, int request_formats_count, vk::ColorSpaceKHR request_color_space)
    {
//        IM_ASSERT(g_FunctionsLoaded && "Need to call ImGui_ImplVulkan_LoadFunctions() if IMGUI_IMPL_VULKAN_NO_PROTOTYPES or VK_NO_PROTOTYPES are set!");
        IM_ASSERT(request_formats != nullptr);
        IM_ASSERT(request_formats_count > 0);

        // Per Spec Format and View Format are expected to be the same unless VK_IMAGE_CREATE_MUTABLE_BIT was set at image creation
        // Assuming that the default behavior is without setting this bit, there is no need for separate Swapchain image and image view format
        // Additionally several new color spaces were introduced with Vulkan Spec v1.0.40,
        // hence we must make sure that a format with the mostly available color space, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, is found and used.
        auto avail_format = physical_device.getSurfaceFormatsKHR(surface);

        // First check if only one format, VK_FORMAT_UNDEFINED, is available, which would imply that any format is available
        if (avail_format.size() == 1)
        {
            if (avail_format[0].format == vk::Format::eUndefined)
            {
                vk::SurfaceFormatKHR ret;
                ret.format = request_formats[0];
                ret.colorSpace = request_color_space;
                return ret;
            }
            else
            {
                // No point in searching another format
                return avail_format[0];
            }
        }
        else
        {
            // Request several formats, the first found will be used
            for (int request_i = 0; request_i < request_formats_count; request_i++)
                for (uint32_t avail_i = 0; avail_i < avail_format.size(); avail_i++)
                    if (avail_format[avail_i].format == request_formats[request_i] && avail_format[avail_i].colorSpace == request_color_space)
                        return avail_format[avail_i];

            // If none of the requested image formats could be found, use the first available
            return avail_format[0];
        }
    }

    vk::PresentModeKHR SelectPresentMode(const vk::raii::PhysicalDevice& physical_device, const vk::raii::SurfaceKHR& surface, const vk::PresentModeKHR* request_modes, int request_modes_count)
    {
//        IM_ASSERT(g_FunctionsLoaded && "Need to call ImGui_ImplVulkan_LoadFunctions() if IMGUI_IMPL_VULKAN_NO_PROTOTYPES or VK_NO_PROTOTYPES are set!");
        IM_ASSERT(request_modes != nullptr);
        IM_ASSERT(request_modes_count > 0);

        // Request a certain mode and confirm that it is available. If not use VK_PRESENT_MODE_FIFO_KHR which is mandatory
        auto avail_modes = physical_device.getSurfacePresentModesKHR(surface);
        //for (uint32_t avail_i = 0; avail_i < avail_count; avail_i++)
        //    printf("[vulkan] avail_modes[%d] = %d\n", avail_i, avail_modes[avail_i]);

        for (int request_i = 0; request_i < request_modes_count; request_i++)
            for (uint32_t avail_i = 0; avail_i < avail_modes.size(); avail_i++)
                if (request_modes[request_i] == avail_modes[avail_i])
                    return request_modes[request_i];

        return vk::PresentModeKHR::eFifo; // Always available
    }

    void SetupVulkanWindow(VkWindow *wd, VkSurfaceKHR surface, int width, int height)
    {
        // Check for WSI support
        auto res = physicalDevice.getSurfaceSupportKHR(queueFamily, surface);
        if (res != vk::True)
        {
            fprintf(stderr, "Error no WSI support on physical device 0\n");
            exit(-1);
        }

        // Select Surface Format
        const vk::Format requestSurfaceImageFormat[] = { vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Unorm, vk::Format::eB8G8R8Unorm, vk::Format::eR8G8B8Unorm };
        const vk::ColorSpaceKHR requestSurfaceColorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
        wd->Surface = vk::raii::SurfaceKHR(instance, surface);
        wd->SurfaceFormat = SelectSurfaceFormat(physicalDevice, wd->Surface, requestSurfaceImageFormat, (size_t)IM_COUNTOF(requestSurfaceImageFormat), requestSurfaceColorSpace);

        // Select Present Mode
    #ifdef APP_USE_UNLIMITED_FRAME_RATE
        VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
    #else
        vk::PresentModeKHR present_modes[] = { vk::PresentModeKHR::eFifo };
    #endif
        wd->PresentMode = SelectPresentMode(physicalDevice, wd->Surface, &present_modes[0], IM_COUNTOF(present_modes));
        //printf("[vulkan] Selected PresentMode = %d\n", wd->PresentMode);

        // Create SwapChain, RenderPass, Framebuffer, etc.
        IM_ASSERT(g_MinImageCount >= 2);
        wd->CreateOrResizeWindow(instance, physicalDevice, device, queueFamily, width, height, g_MinImageCount);
    }

    static bool IsExtensionAvailable(const std::vector<vk::ExtensionProperties> properties, const char* extension)
    {
        for (const auto& p : properties)
            if (strcmp(p.extensionName, extension) == 0)
                return true;
        return false;
    }

	void create_texture(AVFrame *frame)
	{
        if (FrameData::init_static(frame)) {
            for (auto& data : frameData) {
                data.init(frame, device, queueFamily);
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
        if (SDL_Vulkan_CreateSurface(window, *instance, nullptr, &surface) == 0)
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
#ifdef APP_USE_VULKAN_DEBUG_REPORT
        // Remove the debug report callback
//        auto f_vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugReportCallbackEXT");
//        f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif // APP_USE_VULKAN_DEBUG_REPORT
    }

    void set_frame(AVFrame *frame, double play_time, AppSub sub) {
//        frameData[frame_idx].upload(frame, queue, device);
    }

	void render(AppSub sub)
	{
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
        if (!is_minimized)
        {
            rebuild();
            vkWindow.render(device, queue, draw_data);
        }
	}

	void window_size_changed(Sint32 w, Sint32 h) {
        wnd_w = w;
        wnd_h = h;
    }

    void rebuild() {
        if ((vkWindow.need_rebuild || vkWindow.Width != wnd_w || vkWindow.Height != wnd_h))
        {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            vkWindow.CreateOrResizeWindow(instance, physicalDevice, device, queueFamily, wnd_w, wnd_h, g_MinImageCount);
            vkWindow.need_rebuild = false;
        }
    }

    void reset_scale() {
        
    }

    void wait_for_idle() {
        device.waitIdle();
    }

    void imgui_init() {
        ImGui_ImplVulkan_InitInfo init_info = {};
        //init_info.ApiVersion = VK_API_VERSION_1_3;              // Pass in your value of VkApplicationInfo::apiVersion, otherwise will default to header version.
        init_info.Instance = *instance;
        init_info.PhysicalDevice = *physicalDevice;
        init_info.Device = *device;
        init_info.QueueFamily = queueFamily;
        init_info.Queue = *queue;
        init_info.PipelineCache = VK_NULL_HANDLE;
        init_info.DescriptorPool = *descriptorPool;
        init_info.MinImageCount = g_MinImageCount;
        init_info.ImageCount = vkWindow.ImageCount;
        init_info.Allocator = nullptr;
        init_info.PipelineInfoMain.RenderPass = *vkWindow.RenderPass;
        init_info.PipelineInfoMain.Subpass = 0;
        init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.CheckVkResultFn = check_vk_result;
        ImGui_ImplVulkan_Init(&init_info);
    }
};