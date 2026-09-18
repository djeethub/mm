#pragma once

#include <print>

#include "vk_util.hpp"
#include <SDL3/SDL_vulkan.h>

extern "C" {
#include <libavutil/hwcontext_drm.h>
}

#include "vk_video.hpp"
#include "subtitle.hpp"

#ifdef NDEBUG
constexpr bool enableValidationLayers = false;
#else
constexpr bool enableValidationLayers = true;
#endif

const std::vector<char const *> validationLayers = {
    "VK_LAYER_KHRONOS_validation"};

void check_vk_result(VkResult err)
{
    if (err == VK_SUCCESS)
        return;
    SDL_Log("[vulkan] Error: VkResult = %s\n", err);
    if (err < 0)
        abort();
}

static VKAPI_ATTR vk::Bool32 VKAPI_CALL debugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT severity, vk::DebugUtilsMessageTypeFlagsEXT type, const vk::DebugUtilsMessengerCallbackDataEXT *pCallbackData, void *)
{
    std::println("[Vulkan {}] {}", to_string(type), pCallbackData->pMessage);
    return vk::False;
}

class AppVk {
private:
    const int      MAX_FRAMES_IN_FLIGHT = 2;
	std::vector<const char *> requiredDeviceExtension = {
	    vk::KHRSwapchainExtensionName,
		vk::EXTExternalMemoryDmaBufExtensionName,
		vk::EXTImageDrmFormatModifierExtensionName,
		vk::KHRExternalMemoryExtensionName,
		vk::KHRExternalMemoryFdExtensionName,
	};

    vk::raii::Context context;
    vk::raii::Instance instance = nullptr;
    vk::raii::DebugUtilsMessengerEXT debugMessenger = nullptr;
    vk::raii::SurfaceKHR surface = nullptr;
    vk::raii::PhysicalDevice physicalDevice = nullptr;
    vk::raii::Device device = nullptr;
    vk::raii::Queue queue = nullptr;
	vk::raii::SwapchainKHR           swapChain      = nullptr;
	std::vector<vk::Image>           swapChainImages;
	std::vector<vk::raii::ImageView> swapChainImageViews;
	vk::raii::CommandPool                commandPool = nullptr;
	std::vector<vk::raii::CommandBuffer> commandBuffers;

	std::vector<vk::raii::Semaphore> presentCompleteSemaphores;
	std::vector<vk::raii::Semaphore> renderFinishedSemaphores;
    std::vector<vk::raii::Fence>     inFlightFences;
    uint32_t                         frameIndex = 0;

    VkVideo video;

    bool framebufferResized = false;
	int wnd_w = 0;
	int wnd_h = 0;
	float base_scale = 0.0;
	SwsContext *sws_ctx = nullptr;

	std::vector<const char *> getRequiredInstanceExtensions()
	{
        uint32_t sdl_extensions_count = 0;
        const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extensions_count);

		std::vector extensions(sdl_extensions, sdl_extensions + sdl_extensions_count);
		if (enableValidationLayers)
		{
			extensions.push_back(vk::EXTDebugUtilsExtensionName);
		}
//		extensions.push_back(vk::KHRExternalMemoryCapabilitiesExtensionName);
//		extensions.push_back(vk::KHRGetPhysicalDeviceProperties2ExtensionName);

		return extensions;
	}

	void createInstance()
	{
		constexpr vk::ApplicationInfo appInfo{.pApplicationName   = "mm",
		                                      .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
		                                      .pEngineName        = "No Engine",
		                                      .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
		                                      .apiVersion         = vk::ApiVersion14};

		// Get the required layers
		std::vector<char const *> requiredLayers;
		if (enableValidationLayers)
		{
			requiredLayers.assign(validationLayers.begin(), validationLayers.end());
		}

		// Check if the required layers are supported by the Vulkan implementation.
		auto layerProperties    = context.enumerateInstanceLayerProperties();
		auto unsupportedLayerIt = std::ranges::find_if(requiredLayers,
		                                               [&layerProperties](auto const &requiredLayer) {
			                                               return std::ranges::none_of(layerProperties,
			                                                                           [requiredLayer](auto const &layerProperty) { return strcmp(layerProperty.layerName, requiredLayer) == 0; });
		                                               });
		if (unsupportedLayerIt != requiredLayers.end())
		{
			throw std::runtime_error("Required layer not supported: " + std::string(*unsupportedLayerIt));
		}

		// Get the required extensions.
		auto requiredExtensions = getRequiredInstanceExtensions();

		// Check if the required extensions are supported by the Vulkan implementation.
		auto extensionProperties = context.enumerateInstanceExtensionProperties();
		auto unsupportedPropertyIt =
		    std::ranges::find_if(requiredExtensions,
		                         [&extensionProperties](auto const &requiredExtension) {
			                         return std::ranges::none_of(extensionProperties,
			                                                     [requiredExtension](auto const &extensionProperty) { return strcmp(extensionProperty.extensionName, requiredExtension) == 0; });
		                         });
		if (unsupportedPropertyIt != requiredExtensions.end())
		{
			throw std::runtime_error("Required extension not supported: " + std::string(*unsupportedPropertyIt));
		}

		vk::InstanceCreateInfo createInfo{.pApplicationInfo        = &appInfo,
		                                  .enabledLayerCount       = static_cast<uint32_t>(requiredLayers.size()),
		                                  .ppEnabledLayerNames     = requiredLayers.data(),
		                                  .enabledExtensionCount   = static_cast<uint32_t>(requiredExtensions.size()),
		                                  .ppEnabledExtensionNames = requiredExtensions.data()};
		instance = vk::raii::Instance(context, createInfo);
	}

	void setupDebugMessenger()
	{
		if (!enableValidationLayers)
			return;

		vk::DebugUtilsMessageSeverityFlagsEXT severityFlags(
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eError);
		vk::DebugUtilsMessageTypeFlagsEXT     messageTypeFlags(
		    vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation);
		vk::DebugUtilsMessengerCreateInfoEXT debugUtilsMessengerCreateInfoEXT{.messageSeverity = severityFlags,
		                                                                      .messageType     = messageTypeFlags,
		                                                                      .pfnUserCallback = &debugCallback};
		debugMessenger = instance.createDebugUtilsMessengerEXT(debugUtilsMessengerCreateInfoEXT);
	}

   	void createSurface(SDL_Window *window)
	{
        VkSurfaceKHR _surface;
        if (SDL_Vulkan_CreateSurface(window, *instance, nullptr, &_surface) == 0)
        {
            throw std::runtime_error("failed to create window surface!");
        }
		surface = vk::raii::SurfaceKHR(instance, _surface);
	}

	bool isDeviceSuitable(vk::raii::PhysicalDevice const &physicalDevice)
	{
		// Check if the physicalDevice supports the Vulkan 1.3 API version
		bool supportsVulkan1_3 = physicalDevice.getProperties().apiVersion >= VK_API_VERSION_1_3;

		// Check if any of the queue families support both graphics and presentation to our surface
		auto     queueFamilies = physicalDevice.getQueueFamilyProperties();
		uint32_t qfpIndex      = 0;
		bool     supportsGraphicsAndPresent =
		    std::ranges::any_of(queueFamilies,
		                        [&physicalDevice, &surface = this->surface, &qfpIndex](auto const &qfp) {
			                        bool const suitable = (qfp.queueFlags & vk::QueueFlagBits::eGraphics) && physicalDevice.getSurfaceSupportKHR(qfpIndex, *surface);
			                        qfpIndex++;
			                        return suitable;
		                        });

		// Check if all required physicalDevice extensions are available
		auto availableDeviceExtensions = physicalDevice.enumerateDeviceExtensionProperties();
		bool supportsAllRequiredExtensions =
		    std::ranges::all_of(requiredDeviceExtension,
		                        [&availableDeviceExtensions](auto const &requiredDeviceExtension) {
			                        return std::ranges::any_of(availableDeviceExtensions,
			                                                   [requiredDeviceExtension](auto const &availableDeviceExtension) { return strcmp(availableDeviceExtension.extensionName, requiredDeviceExtension) == 0; });
		                        });

		// Check if the physicalDevice supports the required features
		auto features                 = physicalDevice.template getFeatures2<vk::PhysicalDeviceFeatures2,
		                                                                     vk::PhysicalDeviceVulkan13Features,
		                                                                     vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>();
		bool supportsRequiredFeatures = features.template get<vk::PhysicalDeviceFeatures2>().features.samplerAnisotropy &&
		                                features.template get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering &&
		                                features.template get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>().extendedDynamicState;

		// Return true if the physicalDevice meets all the criteria
		return supportsVulkan1_3 && supportsGraphicsAndPresent && supportsAllRequiredExtensions && supportsRequiredFeatures;
	}

	void pickPhysicalDevice()
	{
		std::vector<vk::raii::PhysicalDevice> physicalDevices = instance.enumeratePhysicalDevices();
		auto const                            devIter         = std::ranges::find_if(physicalDevices, [&](auto const &physicalDevice) { return isDeviceSuitable(physicalDevice); });
		if (devIter == physicalDevices.end())
		{
			throw std::runtime_error("failed to find a suitable GPU!");
		}
		physicalDevice = *devIter;
	}

	void createLogicalDevice()
	{
		std::vector<vk::QueueFamilyProperties> queueFamilyProperties = physicalDevice.getQueueFamilyProperties();

		// get the first index into queueFamilyProperties which supports both graphics and present
		for (uint32_t qfpIndex = 0; qfpIndex < queueFamilyProperties.size(); qfpIndex++)
		{
			if ((queueFamilyProperties[qfpIndex].queueFlags & vk::QueueFlagBits::eGraphics) &&
			    physicalDevice.getSurfaceSupportKHR(qfpIndex, *surface))
			{
				// found a queue family that supports both graphics and present
				queueIndex = qfpIndex;
				break;
			}
		}
		if (queueIndex == ~0)
		{
			throw std::runtime_error("Could not find a queue for graphics and present -> terminating");
		}

		// query for Vulkan 1.3 features
		vk::StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT, vk::PhysicalDeviceSamplerYcbcrConversionFeatures, vk::PhysicalDeviceDescriptorIndexingFeatures> featureChain = {
		    {.features = {.samplerAnisotropy = true}},                   // vk::PhysicalDeviceFeatures2
		    {.synchronization2 = true, .dynamicRendering = true},        // vk::PhysicalDeviceVulkan13Features
		    {.extendedDynamicState = true},                               // vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT
			{.samplerYcbcrConversion = true},
			{.shaderSampledImageArrayNonUniformIndexing = true, .runtimeDescriptorArray = true},
		};

		// create a Device
		float                     queuePriority = 1.0f;
		vk::DeviceQueueCreateInfo deviceQueueCreateInfo{.queueFamilyIndex = queueIndex, .queueCount = 1, .pQueuePriorities = &queuePriority};
		vk::DeviceCreateInfo      deviceCreateInfo{.pNext                   = &featureChain.get<vk::PhysicalDeviceFeatures2>(),
		                                           .queueCreateInfoCount    = 1,
		                                           .pQueueCreateInfos       = &deviceQueueCreateInfo,
		                                           .enabledExtensionCount   = static_cast<uint32_t>(requiredDeviceExtension.size()),
		                                           .ppEnabledExtensionNames = requiredDeviceExtension.data()};

		device = vk::raii::Device(physicalDevice, deviceCreateInfo);
		queue  = vk::raii::Queue(device, queueIndex, 0);
	}

	void createCommandPool()
	{
		vk::CommandPoolCreateInfo poolInfo{.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		                                   .queueFamilyIndex = queueIndex};
		commandPool = vk::raii::CommandPool(device, poolInfo);
	}

	void createCommandBuffers()
	{
		commandBuffers.clear();
		vk::CommandBufferAllocateInfo allocInfo{.commandPool = commandPool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = (uint32_t) MAX_FRAMES_IN_FLIGHT};
		commandBuffers = vk::raii::CommandBuffers(device, allocInfo);
	}

    void createSyncObjects()
	{
		assert(presentCompleteSemaphores.empty() && renderFinishedSemaphores.empty());

		for (size_t i = 0; i < swapChainImages.size(); i++)
		{
			renderFinishedSemaphores.emplace_back(device, vk::SemaphoreCreateInfo());
		}

		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		{
			presentCompleteSemaphores.emplace_back(device, vk::SemaphoreCreateInfo());
			inFlightFences.emplace_back(device, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
		}
	}

	void cleanupSwapChain()
	{
		swapChainImageViews.clear();
		swapChain = nullptr;
	}

	void recreateSwapChain()
	{
		device.waitIdle();

		cleanupSwapChain();
		createSwapChain();
		createImageViews();
	}

	static vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR> &availableFormats)
	{
		assert(!availableFormats.empty());
		const auto formatIt = std::ranges::find_if(
		    availableFormats,
		    [](const auto &format) { return format.format == vk::Format::eB8G8R8A8Unorm && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear; });
		return formatIt != availableFormats.end() ? *formatIt : availableFormats[0];
	}

	static vk::PresentModeKHR chooseSwapPresentMode(std::vector<vk::PresentModeKHR> const &availablePresentModes)
	{
		return vk::PresentModeKHR::eFifo;
		std::unreachable();
		assert(std::ranges::any_of(availablePresentModes, [](auto presentMode) { return presentMode == vk::PresentModeKHR::eFifo; }));
		return std::ranges::any_of(availablePresentModes,
		                           [](const vk::PresentModeKHR value) { return vk::PresentModeKHR::eMailbox == value; }) ?
		           vk::PresentModeKHR::eMailbox :
		           vk::PresentModeKHR::eFifo;
	}

	vk::Extent2D chooseSwapExtent(vk::SurfaceCapabilitiesKHR const &capabilities)
	{
		if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
		{
			return capabilities.currentExtent;
		}

		return {
		    std::clamp<uint32_t>(wnd_w, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
		    std::clamp<uint32_t>(wnd_h, capabilities.minImageExtent.height, capabilities.maxImageExtent.height)};
	}

	static uint32_t chooseSwapMinImageCount(vk::SurfaceCapabilitiesKHR const &surfaceCapabilities)
	{
		auto minImageCount = std::max(3u, surfaceCapabilities.minImageCount);
		if ((0 < surfaceCapabilities.maxImageCount) && (surfaceCapabilities.maxImageCount < minImageCount))
		{
			minImageCount = surfaceCapabilities.maxImageCount;
		}
		return minImageCount;
	}

	void createSwapChain()
	{
		vk::SurfaceCapabilitiesKHR surfaceCapabilities = physicalDevice.getSurfaceCapabilitiesKHR(*surface);
		swapChainExtent                                = chooseSwapExtent(surfaceCapabilities);
		uint32_t minImageCount                         = chooseSwapMinImageCount(surfaceCapabilities);

		std::vector<vk::SurfaceFormatKHR> availableFormats = physicalDevice.getSurfaceFormatsKHR(*surface);
		swapChainSurfaceFormat                             = chooseSwapSurfaceFormat(availableFormats);

		std::vector<vk::PresentModeKHR> availablePresentModes = physicalDevice.getSurfacePresentModesKHR(*surface);
		vk::PresentModeKHR              presentMode           = chooseSwapPresentMode(availablePresentModes);

		vk::SwapchainCreateInfoKHR swapChainCreateInfo{.surface          = *surface,
		                                               .minImageCount    = minImageCount,
		                                               .imageFormat      = swapChainSurfaceFormat.format,
		                                               .imageColorSpace  = swapChainSurfaceFormat.colorSpace,
		                                               .imageExtent      = swapChainExtent,
		                                               .imageArrayLayers = 1,
		                                               .imageUsage       = vk::ImageUsageFlagBits::eColorAttachment,
		                                               .imageSharingMode = vk::SharingMode::eExclusive,
		                                               .preTransform     = surfaceCapabilities.currentTransform,
		                                               .compositeAlpha   = vk::CompositeAlphaFlagBitsKHR::eOpaque,
		                                               .presentMode      = presentMode,
		                                               .clipped          = true};

		swapChain       = vk::raii::SwapchainKHR(device, swapChainCreateInfo);
		swapChainImages = swapChain.getImages();
	}
    
	vk::raii::ImageView createImageView(vk::Image const &image, vk::Format format)
	{
		vk::ImageViewCreateInfo viewInfo{
		    .image            = image,
		    .viewType         = vk::ImageViewType::e2D,
		    .format           = format,
		    .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1}};
		return vk::raii::ImageView(device, viewInfo);
	}

	void createImageViews()
	{
		assert(swapChainImageViews.empty());

		swapChainImageViews.reserve(swapChainImages.size());
		for (auto &image : swapChainImages)
		{
			swapChainImageViews.emplace_back(createImageView(image, swapChainSurfaceFormat.format));
		}
	}

    void SetupVulkan(SDL_Window *window)
    {
		createInstance();
		setupDebugMessenger();
		createSurface(window);
		pickPhysicalDevice();
		createLogicalDevice();
		createSwapChain();
		createImageViews();
		createCommandPool();
		createCommandBuffers();
        createSyncObjects();
		
        memProperties = physicalDevice.getMemoryProperties();
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

    static bool IsExtensionAvailable(const std::vector<vk::ExtensionProperties> properties, const char* extension)
    {
        for (const auto& p : properties)
            if (strcmp(p.extensionName, extension) == 0)
                return true;
        return false;
    }

	void transition_image_layout(
	    uint32_t                imageIndex,
	    vk::ImageLayout         old_layout,
	    vk::ImageLayout         new_layout,
	    vk::AccessFlags2        src_access_mask,
	    vk::AccessFlags2        dst_access_mask,
	    vk::PipelineStageFlags2 src_stage_mask,
	    vk::PipelineStageFlags2 dst_stage_mask)
	{
		vk::ImageMemoryBarrier2 barrier = {
		    .srcStageMask        = src_stage_mask,
		    .srcAccessMask       = src_access_mask,
		    .dstStageMask        = dst_stage_mask,
		    .dstAccessMask       = dst_access_mask,
		    .oldLayout           = old_layout,
		    .newLayout           = new_layout,
		    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .image               = swapChainImages[imageIndex],
		    .subresourceRange    = {
		        .aspectMask     = vk::ImageAspectFlagBits::eColor,
		        .baseMipLevel   = 0,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 1}};
		vk::DependencyInfo dependency_info = {
		    .dependencyFlags         = {},
		    .imageMemoryBarrierCount = 1,
		    .pImageMemoryBarriers    = &barrier};
		commandBuffers[frameIndex].pipelineBarrier2(dependency_info);
	}

public:
    float video_scale = 1.0;
    float video_pan_x = 0.0;
    float video_pan_y = 0.0;
	auto get_pix_fmt() { return video.pix_fmt; }

    bool init(SDL_Window *window) {
        SDL_GetWindowSizeInPixels(window, &wnd_w, &wnd_h);
        SetupVulkan(window);

        return true;
    }

    void shutdown() {
#ifdef APP_USE_VULKAN_DEBUG_REPORT
        // Remove the debug report callback
//        auto f_vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugReportCallbackEXT");
//        f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif // APP_USE_VULKAN_DEBUG_REPORT
    }

    void set_frame(AvFrameData frame_data, AppSub sub) {
        if (!video.check_frame(frame_data.frame))
		{
			device.waitIdle();
		    video.init(frame_data.frame, physicalDevice, device);
			reset_scale();
		}
        video.upload(std::move(frame_data), device, queue);
		std::visit([&](auto&& sub){
			if (sub)
				sub->prepare_draw(queue, frame_data.play_time);
		}, sub);
    }

    bool check_next_frame(double play_time) {
        return video.check_next_frame(play_time, device);
    }

	void render_frame(const vk::CommandBuffer& commandBuffer) {
        auto& vf = video.get_current_frame();
        if (vf.status != VkFrame::Ready)
            return;

        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *video.pipeline);
		commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, video.pipelineLayout, 0, *vf.set, nullptr);
        auto scale = base_scale * video_scale;
		float w = 2.0f * scale * vf.frame_data.frame->width / wnd_w;
		float h = 2.0f * scale * vf.frame_data.frame->height / wnd_h;
		Vertform tf = {
			.position = { video_pan_x / wnd_w, video_pan_y / wnd_h },
			.size = { w, h }
		};
        Uniforms uf = {
            .tex_size = {(float)vf.frame_data.frame->width, (float)vf.frame_data.frame->height},
//            .color_range = fd.frame->color_range == AVCOL_RANGE_UNSPECIFIED ? AVCOL_RANGE_MPEG : fd.frame->color_range,
//            .colorspace = fd.frame->colorspace == AVCOL_SPC_UNSPECIFIED ? AVCOL_SPC_BT709 : fd.frame->colorspace
        };
        commandBuffer.pushConstants(video.pipelineLayout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(tf), &tf);
        commandBuffer.pushConstants(video.pipelineLayout, vk::ShaderStageFlagBits::eFragment, sizeof(Vertform), sizeof(uf), &uf);
		commandBuffer.draw(4, 1, 0, 0);
	}

	void render(AppSub sub)
	{
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
        if (is_minimized)
            return;

		auto fenceResult = device.waitForFences(*inFlightFences[frameIndex], vk::True, UINT64_MAX);
		if (fenceResult != vk::Result::eSuccess)
		{
			throw std::runtime_error("failed to wait for fence!");
		}

		auto [result, imageIndex] = swapChain.acquireNextImage(UINT64_MAX, *presentCompleteSemaphores[frameIndex], nullptr);

		// Due to VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS being defined, eErrorOutOfDateKHR can be checked as a result
		// here and does not need to be caught by an exception.
		if (result == vk::Result::eErrorOutOfDateKHR)
		{
			recreateSwapChain();
			return;
		}
		// On other success codes than eSuccess and eSuboptimalKHR we just throw an exception.
		// On any error code, aquireNextImage already threw an exception.
		if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR)
		{
			assert(result == vk::Result::eTimeout || result == vk::Result::eNotReady);
			throw std::runtime_error("failed to acquire swap chain image!");
		}
//		updateUniformBuffer(frameIndex);

		// Only reset the fence if we are submitting work
		device.resetFences(*inFlightFences[frameIndex]);

        auto commandBuffer = *commandBuffers[frameIndex];
		commandBuffer.reset();
        vk::CommandBufferBeginInfo begin_info{};
        commandBuffer.begin(begin_info);

        // Before starting rendering, transition the swapchain image to vk::ImageLayout::eColorAttachmentOptimal
		transition_image_layout(
		    imageIndex,
		    vk::ImageLayout::eUndefined,
		    vk::ImageLayout::eColorAttachmentOptimal,
		    {},                                                        // srcAccessMask (no need to wait for previous operations)
		    vk::AccessFlagBits2::eColorAttachmentWrite,                // dstAccessMask
		    vk::PipelineStageFlagBits2::eColorAttachmentOutput,        // srcStage
		    vk::PipelineStageFlagBits2::eColorAttachmentOutput         // dstStage
		);
		vk::ClearValue              clearColor     = vk::ClearColorValue(0.0f, 0.0f, 0.0f, 1.0f);
		vk::RenderingAttachmentInfo attachmentInfo = {
		    .imageView   = swapChainImageViews[imageIndex],
		    .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
		    .loadOp      = vk::AttachmentLoadOp::eClear,
		    .storeOp     = vk::AttachmentStoreOp::eStore,
		    .clearValue  = clearColor};
		vk::RenderingInfo renderingInfo = {
		    .renderArea           = {.offset = {0, 0}, .extent = swapChainExtent},
		    .layerCount           = 1,
		    .colorAttachmentCount = 1,
		    .pColorAttachments    = &attachmentInfo};
		commandBuffer.beginRendering(renderingInfo);

		commandBuffer.setViewport(0, vk::Viewport(0.0f, static_cast<float>(swapChainExtent.height), static_cast<float>(swapChainExtent.width), -static_cast<float>(swapChainExtent.height), 0.0f, 1.0f));
		commandBuffer.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), swapChainExtent));
		render_frame(commandBuffer);
		std::visit([&](auto&& sub){
			if (sub)
				sub->draw(commandBuffer);
		}, sub);
        ImGui_ImplVulkan_RenderDrawData(draw_data, commandBuffer);

		commandBuffer.endRendering();

		// After rendering, transition the swapchain image to vk::ImageLayout::ePresentSrcKHR
		transition_image_layout(
		    imageIndex,
		    vk::ImageLayout::eColorAttachmentOptimal,
		    vk::ImageLayout::ePresentSrcKHR,
		    vk::AccessFlagBits2::eColorAttachmentWrite,                // srcAccessMask
		    {},                                                        // dstAccessMask
		    vk::PipelineStageFlagBits2::eColorAttachmentOutput,        // srcStage
		    vk::PipelineStageFlagBits2::eBottomOfPipe                  // dstStage
		);
        commandBuffer.end();

		vk::PipelineStageFlags waitDestinationStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput);
		const vk::SubmitInfo   submitInfo{.waitSemaphoreCount   = 1,
		                                  .pWaitSemaphores      = &*presentCompleteSemaphores[frameIndex],
		                                  .pWaitDstStageMask    = &waitDestinationStageMask,
		                                  .commandBufferCount   = 1,
		                                  .pCommandBuffers      = &*commandBuffers[frameIndex],
		                                  .signalSemaphoreCount = 1,
		                                  .pSignalSemaphores    = &*renderFinishedSemaphores[imageIndex]};
		queue.submit(submitInfo, *inFlightFences[frameIndex]);

		const vk::PresentInfoKHR presentInfoKHR{.waitSemaphoreCount = 1,
		                                        .pWaitSemaphores    = &*renderFinishedSemaphores[imageIndex],
		                                        .swapchainCount     = 1,
		                                        .pSwapchains        = &*swapChain,
		                                        .pImageIndices      = &imageIndex};
		result = queue.presentKHR(presentInfoKHR);
		// Due to VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS being defined, eErrorOutOfDateKHR can be checked as a result
		// here and does not need to be caught by an exception.
		if ((result == vk::Result::eSuboptimalKHR) || (result == vk::Result::eErrorOutOfDateKHR) || framebufferResized)
		{
			framebufferResized = false;
			recreateSwapChain();
		}
		else
		{
			// There are no other success codes than eSuccess; on any error code, presentKHR already threw an exception.
			assert(result == vk::Result::eSuccess);
		}
        frameIndex = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
	}

	void window_size_changed(Sint32 w, Sint32 h) {
        wnd_w = w;
        wnd_h = h;
        framebufferResized = true;
    }

    void reset_scale() {
		base_scale = SDL_max((float) wnd_w / video.width, (float) wnd_h / video.height);
    }

    void wait_for_idle() {
        device.waitIdle();
    }

    void imgui_init(int min_image_count) {
        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.ApiVersion = VK_API_VERSION_1_4;              // Pass in your value of VkApplicationInfo::apiVersion, otherwise will default to header version.
        init_info.Instance = *instance;
        init_info.PhysicalDevice = *physicalDevice;
        init_info.Device = *device;
        init_info.QueueFamily = queueIndex;
        init_info.Queue = *queue;
        init_info.PipelineCache = VK_NULL_HANDLE;
//        init_info.DescriptorPool = *descriptorPool;
        init_info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
        init_info.MinImageCount = min_image_count;
        init_info.ImageCount = swapChainImages.size();
        init_info.Allocator = nullptr;
        init_info.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;
        init_info.PipelineInfoMain.Subpass = 0;
        init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.CheckVkResultFn = check_vk_result;
        init_info.UseDynamicRendering = true;
        init_info.PipelineInfoMain.PipelineRenderingCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = reinterpret_cast<const VkFormat*>(&swapChainSurfaceFormat.format),
            .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
        };
        ImGui_ImplVulkan_Init(&init_info);
        ImGui_ImplVulkan_SetMinImageCount(min_image_count);
    }

	void discard_pending() {
		video.discard_pending();
	}

	const auto& get_device() {
		return device;
	}
};