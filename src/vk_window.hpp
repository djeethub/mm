#pragma once

void check_vk_result(VkResult err);

class VkWindow {
public:
    // Input
    bool                    UseDynamicRendering;
    VkSurfaceKHR            Surface;            // Surface created and destroyed by caller.
    VkSurfaceFormatKHR      SurfaceFormat;
    VkPresentModeKHR        PresentMode;
    VkAttachmentDescription AttachmentDesc;     // RenderPass creation: main attachment description.
    VkClearValue            ClearValue;         // RenderPass creation: clear value when using VK_ATTACHMENT_LOAD_OP_CLEAR.

    // Internal
    int                     Width;              // Generally same as passed to ImGui_ImplVulkanH_CreateOrResizeWindow()
    int                     Height;
    VkSwapchainKHR          Swapchain;
    VkRenderPass            RenderPass;
    VkPipeline              Pipeline;           // The window pipeline may uses a different VkRenderPass than the one passed in ImGui_ImplVulkan_InitInfo
    uint32_t                FrameIndex;         // Current frame being rendered to (0 <= FrameIndex < FrameInFlightCount)
    uint32_t                ImageCount;         // Number of simultaneous in-flight frames (returned by vkGetSwapchainImagesKHR, usually derived from min_image_count)
    uint32_t                SemaphoreCount;     // Number of simultaneous in-flight frames + 1, to be able to use it in vkAcquireNextImageKHR
    uint32_t                SemaphoreIndex;     // Current set of swapchain wait semaphores we're using (needs to be distinct from per frame data)
    ImVector<ImGui_ImplVulkanH_Frame>           Frames;
    ImVector<ImGui_ImplVulkanH_FrameSemaphores> FrameSemaphores;

    VkWindow() {
        // Parameters to create SwapChain
        PresentMode = VK_PRESENT_MODE_MAX_ENUM_KHR;     // Ensure we get an error if user doesn't set this.

        // Parameters to create RenderPass
        AttachmentDesc.format = VK_FORMAT_UNDEFINED;    // Will automatically use wd->SurfaceFormat.format.
        AttachmentDesc.samples = VK_SAMPLE_COUNT_1_BIT;
        AttachmentDesc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        AttachmentDesc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        AttachmentDesc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        AttachmentDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        AttachmentDesc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        AttachmentDesc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }

    void Destroy(VkInstance instance, VkDevice device, const VkAllocationCallbacks* allocator)
    {
        IM_UNUSED(instance);
        vkDeviceWaitIdle(device); // FIXME: We could wait on the Queue if we had the queue in wd-> (otherwise VulkanH functions can't use globals)
        //vkQueueWaitIdle(bd->Queue);

        for (uint32_t i = 0; i < ImageCount; i++)
            DestroyFrame(device, &Frames[i], allocator);
        for (uint32_t i = 0; i < SemaphoreCount; i++)
            DestroyFrameSemaphores(device, &FrameSemaphores[i], allocator);
        Frames.clear();
        FrameSemaphores.clear();
        vkDestroyPipeline(device, Pipeline, allocator);
        vkDestroyRenderPass(device, RenderPass, allocator);
        vkDestroySwapchainKHR(device, Swapchain, allocator);
        RenderPass = VK_NULL_HANDLE;
        Swapchain = VK_NULL_HANDLE;
        Width = Height = 0;
        FrameIndex = ImageCount = SemaphoreCount = SemaphoreIndex = 0;
        //vkDestroySurfaceKHR(instance, wd->Surface, allocator); // v1.92.6 (~2026-01-16): because wd->Surface is user provided we don't attempt to destroy it ourself.
    }

    VkSurfaceFormatKHR SelectSurfaceFormat(VkPhysicalDevice physical_device, VkSurfaceKHR surface, const VkFormat* request_formats, int request_formats_count, VkColorSpaceKHR request_color_space)
    {
//        IM_ASSERT(g_FunctionsLoaded && "Need to call ImGui_ImplVulkan_LoadFunctions() if IMGUI_IMPL_VULKAN_NO_PROTOTYPES or VK_NO_PROTOTYPES are set!");
        IM_ASSERT(request_formats != nullptr);
        IM_ASSERT(request_formats_count > 0);

        // Per Spec Format and View Format are expected to be the same unless VK_IMAGE_CREATE_MUTABLE_BIT was set at image creation
        // Assuming that the default behavior is without setting this bit, there is no need for separate Swapchain image and image view format
        // Additionally several new color spaces were introduced with Vulkan Spec v1.0.40,
        // hence we must make sure that a format with the mostly available color space, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, is found and used.
        uint32_t avail_count;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &avail_count, nullptr);
        ImVector<VkSurfaceFormatKHR> avail_format;
        avail_format.resize((int)avail_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &avail_count, avail_format.Data);

        // First check if only one format, VK_FORMAT_UNDEFINED, is available, which would imply that any format is available
        if (avail_count == 1)
        {
            if (avail_format[0].format == VK_FORMAT_UNDEFINED)
            {
                VkSurfaceFormatKHR ret;
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
                for (uint32_t avail_i = 0; avail_i < avail_count; avail_i++)
                    if (avail_format[avail_i].format == request_formats[request_i] && avail_format[avail_i].colorSpace == request_color_space)
                        return avail_format[avail_i];

            // If none of the requested image formats could be found, use the first available
            return avail_format[0];
        }
    }

    VkPresentModeKHR SelectPresentMode(VkPhysicalDevice physical_device, VkSurfaceKHR surface, const VkPresentModeKHR* request_modes, int request_modes_count)
    {
//        IM_ASSERT(g_FunctionsLoaded && "Need to call ImGui_ImplVulkan_LoadFunctions() if IMGUI_IMPL_VULKAN_NO_PROTOTYPES or VK_NO_PROTOTYPES are set!");
        IM_ASSERT(request_modes != nullptr);
        IM_ASSERT(request_modes_count > 0);

        // Request a certain mode and confirm that it is available. If not use VK_PRESENT_MODE_FIFO_KHR which is mandatory
        uint32_t avail_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &avail_count, nullptr);
        ImVector<VkPresentModeKHR> avail_modes;
        avail_modes.resize((int)avail_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &avail_count, avail_modes.Data);
        //for (uint32_t avail_i = 0; avail_i < avail_count; avail_i++)
        //    printf("[vulkan] avail_modes[%d] = %d\n", avail_i, avail_modes[avail_i]);

        for (int request_i = 0; request_i < request_modes_count; request_i++)
            for (uint32_t avail_i = 0; avail_i < avail_count; avail_i++)
                if (request_modes[request_i] == avail_modes[avail_i])
                    return request_modes[request_i];

        return VK_PRESENT_MODE_FIFO_KHR; // Always available
    }

    void DestroyFrame(VkDevice device, ImGui_ImplVulkanH_Frame* fd, const VkAllocationCallbacks* allocator)
    {
        vkDestroyFence(device, fd->Fence, allocator);
        vkFreeCommandBuffers(device, fd->CommandPool, 1, &fd->CommandBuffer);
        vkDestroyCommandPool(device, fd->CommandPool, allocator);
        fd->Fence = VK_NULL_HANDLE;
        fd->CommandBuffer = VK_NULL_HANDLE;
        fd->CommandPool = VK_NULL_HANDLE;

        vkDestroyImageView(device, fd->BackbufferView, allocator);
        vkDestroyFramebuffer(device, fd->Framebuffer, allocator);
    }

    void DestroyFrameSemaphores(VkDevice device, ImGui_ImplVulkanH_FrameSemaphores* fsd, const VkAllocationCallbacks* allocator)
    {
        vkDestroySemaphore(device, fsd->ImageAcquiredSemaphore, allocator);
        vkDestroySemaphore(device, fsd->RenderCompleteSemaphore, allocator);
        fsd->ImageAcquiredSemaphore = fsd->RenderCompleteSemaphore = VK_NULL_HANDLE;
    }

    int GetMinImageCountFromPresentMode(VkPresentModeKHR present_mode)
    {
        if (present_mode == VK_PRESENT_MODE_MAILBOX_KHR)
            return 3;
        if (present_mode == VK_PRESENT_MODE_FIFO_KHR || present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR)
            return 2;
        if (present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
            return 1;
        IM_ASSERT(0);
        return 1;
    }

    // Also destroy old swap chain and in-flight frames data, if any.
    void CreateWindowSwapChain(VkPhysicalDevice physical_device, VkDevice device, const VkAllocationCallbacks* allocator, int w, int h, uint32_t min_image_count, VkImageUsageFlags image_usage)
    {
        VkResult err;
        VkSwapchainKHR old_swapchain = Swapchain;
        Swapchain = VK_NULL_HANDLE;
        err = vkDeviceWaitIdle(device);
        check_vk_result(err);

        // We don't use ImGui_ImplVulkanH_DestroyWindow() because we want to preserve the old swapchain to create the new one.
        // Destroy old Framebuffer
        for (uint32_t i = 0; i < ImageCount; i++)
            DestroyFrame(device, &Frames[i], allocator);
        for (uint32_t i = 0; i < SemaphoreCount; i++)
            DestroyFrameSemaphores(device, &FrameSemaphores[i], allocator);
        Frames.clear();
        FrameSemaphores.clear();
        ImageCount = 0;
        if (RenderPass)
            vkDestroyRenderPass(device, RenderPass, allocator);
        if (Pipeline)
            vkDestroyPipeline(device, Pipeline, allocator);

        // If min image count was not specified, request different count of images dependent on selected present mode
        if (min_image_count == 0)
            min_image_count = GetMinImageCountFromPresentMode(PresentMode);

        // Create Swapchain
        {
            VkSurfaceCapabilitiesKHR cap;
            err = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, Surface, &cap);
            check_vk_result(err);

            VkSwapchainCreateInfoKHR info = {};
            info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
            info.surface = Surface;
            info.minImageCount = min_image_count;
            info.imageFormat = SurfaceFormat.format;
            info.imageColorSpace = SurfaceFormat.colorSpace;
            info.imageArrayLayers = 1;
            info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | image_usage;
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;           // Assume that graphics family == present family
            info.preTransform = (cap.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : cap.currentTransform;
            if (cap.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            else if (cap.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
                info.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
            else
                IM_ASSERT(false && "No supported composite alpha mode found!");
            info.presentMode = PresentMode;
            info.clipped = VK_TRUE;
            info.oldSwapchain = old_swapchain;
            if (info.minImageCount < cap.minImageCount)
                info.minImageCount = cap.minImageCount;
            else if (cap.maxImageCount != 0 && info.minImageCount > cap.maxImageCount)
                info.minImageCount = cap.maxImageCount;
            if (cap.currentExtent.width == 0xffffffff)
            {
                info.imageExtent.width = Width = w;
                info.imageExtent.height = Height = h;
            }
            else
            {
                info.imageExtent.width = Width = cap.currentExtent.width;
                info.imageExtent.height = Height = cap.currentExtent.height;
            }
            err = vkCreateSwapchainKHR(device, &info, allocator, &Swapchain);
            check_vk_result(err);
            err = vkGetSwapchainImagesKHR(device, Swapchain, &ImageCount, nullptr);
            check_vk_result(err);
            VkImage backbuffers[16] = {};
            IM_ASSERT(ImageCount >= min_image_count);
            IM_ASSERT(ImageCount < IM_COUNTOF(backbuffers));
            err = vkGetSwapchainImagesKHR(device, Swapchain, &ImageCount, backbuffers);
            check_vk_result(err);

            SemaphoreCount = ImageCount + 1;
            Frames.resize(ImageCount);
            FrameSemaphores.resize(SemaphoreCount);
            memset(Frames.Data, 0, Frames.size_in_bytes());
            memset(FrameSemaphores.Data, 0, FrameSemaphores.size_in_bytes());
            for (uint32_t i = 0; i < ImageCount; i++)
                Frames[i].Backbuffer = backbuffers[i];
        }
        if (old_swapchain)
            vkDestroySwapchainKHR(device, old_swapchain, allocator);

        // Create the Render Pass
        if (UseDynamicRendering == false)
        {
            VkAttachmentDescription attachment = AttachmentDesc;
            if (attachment.format == VK_FORMAT_UNDEFINED)
                attachment.format = SurfaceFormat.format;
            VkAttachmentReference color_attachment = {};
            color_attachment.attachment = 0;
            color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkSubpassDescription subpass = {};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &color_attachment;
            VkSubpassDependency dependency = {};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.srcAccessMask = 0;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            info.attachmentCount = 1;
            info.pAttachments = &attachment;
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            info.dependencyCount = 1;
            info.pDependencies = &dependency;
            err = vkCreateRenderPass(device, &info, allocator, &RenderPass);
            check_vk_result(err);

            // We do not create a pipeline by default as this is also used by examples' main.cpp,
            // but secondary viewport in multi-viewport mode may want to create one with:
            //ImGui_ImplVulkan_CreatePipeline(device, allocator, VK_NULL_HANDLE, wd->RenderPass, VK_SAMPLE_COUNT_1_BIT, &wd->Pipeline, v->Subpass);
        }

        // Create The Image Views
        {
            VkImageViewCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            info.format = SurfaceFormat.format;
            info.components.r = VK_COMPONENT_SWIZZLE_R;
            info.components.g = VK_COMPONENT_SWIZZLE_G;
            info.components.b = VK_COMPONENT_SWIZZLE_B;
            info.components.a = VK_COMPONENT_SWIZZLE_A;
            VkImageSubresourceRange image_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            info.subresourceRange = image_range;
            for (uint32_t i = 0; i < ImageCount; i++)
            {
                ImGui_ImplVulkanH_Frame* fd = &Frames[i];
                info.image = fd->Backbuffer;
                err = vkCreateImageView(device, &info, allocator, &fd->BackbufferView);
                check_vk_result(err);
            }
        }

        // Create Framebuffer
        if (UseDynamicRendering == false)
        {
            VkImageView attachment[1];
            VkFramebufferCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            info.renderPass = RenderPass;
            info.attachmentCount = 1;
            info.pAttachments = attachment;
            info.width = Width;
            info.height = Height;
            info.layers = 1;
            for (uint32_t i = 0; i < ImageCount; i++)
            {
                ImGui_ImplVulkanH_Frame* fd = &Frames[i];
                attachment[0] = fd->BackbufferView;
                err = vkCreateFramebuffer(device, &info, allocator, &fd->Framebuffer);
                check_vk_result(err);
            }
        }
    }

    void CreateWindowCommandBuffers(VkPhysicalDevice physical_device, VkDevice device, uint32_t queue_family, const VkAllocationCallbacks* allocator)
    {
        IM_ASSERT(physical_device != VK_NULL_HANDLE && device != VK_NULL_HANDLE);
        IM_UNUSED(physical_device);

        // Create Command Buffers
        VkResult err;
        for (uint32_t i = 0; i < ImageCount; i++)
        {
            ImGui_ImplVulkanH_Frame* fd = &Frames[i];
            {
                VkCommandPoolCreateInfo info = {};
                info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                info.flags = 0;
                info.queueFamilyIndex = queue_family;
                err = vkCreateCommandPool(device, &info, allocator, &fd->CommandPool);
                check_vk_result(err);
            }
            {
                VkCommandBufferAllocateInfo info = {};
                info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                info.commandPool = fd->CommandPool;
                info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                info.commandBufferCount = 1;
                err = vkAllocateCommandBuffers(device, &info, &fd->CommandBuffer);
                check_vk_result(err);
            }
            {
                VkFenceCreateInfo info = {};
                info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
                err = vkCreateFence(device, &info, allocator, &fd->Fence);
                check_vk_result(err);
            }
        }

        for (uint32_t i = 0; i < SemaphoreCount; i++)
        {
            ImGui_ImplVulkanH_FrameSemaphores* fsd = &FrameSemaphores[i];
            {
                VkSemaphoreCreateInfo info = {};
                info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                err = vkCreateSemaphore(device, &info, allocator, &fsd->ImageAcquiredSemaphore);
                check_vk_result(err);
                err = vkCreateSemaphore(device, &info, allocator, &fsd->RenderCompleteSemaphore);
                check_vk_result(err);
            }
        }
    }
 
    // - 2025/09/26: v1.92.4 added a trailing 'VkImageUsageFlags image_usage' parameter which is usually VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT.
    void CreateOrResizeWindow(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, uint32_t queue_family, const VkAllocationCallbacks* allocator, int width, int height, uint32_t min_image_count, VkImageUsageFlags image_usage)
    {
//        IM_ASSERT(g_FunctionsLoaded && "Need to call ImGui_ImplVulkan_LoadFunctions() if IMGUI_IMPL_VULKAN_NO_PROTOTYPES or VK_NO_PROTOTYPES are set!");
        IM_ASSERT(Surface != VK_NULL_HANDLE);
        IM_UNUSED(instance);

        CreateWindowSwapChain(physical_device, device, allocator, width, height, min_image_count, image_usage);
        CreateWindowCommandBuffers(physical_device, device, queue_family, allocator);

        // FIXME: to submit the command buffer, we need a queue. In the examples folder, the ImGui_ImplVulkanH_CreateOrResizeWindow function is called
        // before the ImGui_ImplVulkan_Init function, so we don't have access to the queue yet. Here we have the queue_family that we can use to grab
        // a queue from the device and submit the command buffer. It would be better to have access to the queue as suggested in the FIXME below.
        VkCommandPool command_pool;
        VkCommandPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = queue_family;
        VkResult err = vkCreateCommandPool(device, &pool_info, allocator, &command_pool);
        check_vk_result(err);

        VkFenceCreateInfo fence_info = {};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence;
        err = vkCreateFence(device, &fence_info, allocator, &fence);
        check_vk_result(err);

        VkCommandBufferAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = command_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;
        VkCommandBuffer command_buffer;
        err = vkAllocateCommandBuffers(device, &alloc_info, &command_buffer);
        check_vk_result(err);

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(command_buffer, &begin_info);
        check_vk_result(err);

        // Transition the images to the correct layout for rendering
        for (uint32_t i = 0; i < ImageCount; i++)
        {
            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.image = Frames[i].Backbuffer;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        err = vkEndCommandBuffer(command_buffer);
        check_vk_result(err);
        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;

        VkQueue queue;
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        err = vkQueueSubmit(queue, 1, &submit_info, fence);
        check_vk_result(err);
        err = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        check_vk_result(err);
        err = vkResetFences(device, 1, &fence);
        check_vk_result(err);

        err = vkResetCommandPool(device, command_pool, 0);
        check_vk_result(err);

        // Destroy command buffer and fence and command pool
        vkFreeCommandBuffers(device, command_pool, 1, &command_buffer);
        vkDestroyCommandPool(device, command_pool, allocator);
        vkDestroyFence(device, fence, allocator);
        command_pool = VK_NULL_HANDLE;
        command_buffer = VK_NULL_HANDLE;
        fence = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE;
    }
};