#pragma once

#include "vk_frame.hpp"

struct Swap
{
    vk::raii::CommandPool       CommandPool = nullptr;
    vk::raii::CommandBuffer     CommandBuffer = nullptr;
    vk::raii::Fence             Fence = nullptr;
    vk::Image                   Backbuffer = nullptr;
    vk::raii::ImageView         BackbufferView = nullptr;
    vk::raii::Framebuffer       Framebuffer = nullptr;

    void init(const vk::raii::Device& device, uint32_t queue_family) {
        {
            vk::CommandPoolCreateInfo info = {
                .queueFamilyIndex = queue_family
            };
            CommandPool = device.createCommandPool(info);
        }
        {
            vk::CommandBufferAllocateInfo info = {};
            info.commandPool = CommandPool;
            info.level = vk::CommandBufferLevel::ePrimary;
            info.commandBufferCount = 1;
            CommandBuffer = std::move(device.allocateCommandBuffers(info)[0]);
        }
        {
            vk::FenceCreateInfo info = {
                .flags = vk::FenceCreateFlagBits::eSignaled,
            };
            Fence = device.createFence(info);
        }
/*
        vk::CommandBufferBeginInfo begin_info = {
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
        };
        CommandBuffer.begin(begin_info);

        vk::ImageMemoryBarrier imageBarrier = {
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = Backbuffer,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlags::BitsType::eColor,
                .levelCount = 1,
                .layerCount = 1,
            }
        };
        CommandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eBottomOfPipe, vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr, nullptr, imageBarrier);

        CommandBuffer.end();

        vk::SubmitInfo submit_info = {
            .commandBufferCount = 1,
            .pCommandBuffers = &*CommandBuffer,
        };
        auto queue = device.getQueue(queue_family, 0);
        queue.submit(submit_info, Fence);*/
    }
};

struct SwapSemaphores {
    vk::raii::Semaphore ImageAcquiredSemaphore = nullptr;
    vk::raii::Semaphore RenderCompleteSemaphore = nullptr;

    void init(const vk::raii::Device& device) {
        ImageAcquiredSemaphore = device.createSemaphore({});
        RenderCompleteSemaphore = device.createSemaphore({});
    }
};

class VkWindow {
public:
    // Input
    bool                    UseDynamicRendering = false;
    vk::SurfaceKHR          Surface;            // Surface created and destroyed by caller.
    vk::SurfaceFormatKHR      SurfaceFormat{};
    vk::PresentModeKHR        PresentMode{};
    vk::AttachmentDescription AttachmentDesc{};     // RenderPass creation: main attachment description.
    vk::ClearValue            ClearValue{};         // RenderPass creation: clear value when using VK_ATTACHMENT_LOAD_OP_CLEAR.

    // Internal
    int                     Width;              // Generally same as passed to ImGui_ImplVulkanH_CreateOrResizeWindow()
    int                     Height;
    vk::raii::SwapchainKHR  Swapchain = nullptr;
    vk::raii::RenderPass    RenderPass  = nullptr;
    uint32_t                FrameIndex;         // Current frame being rendered to (0 <= FrameIndex < FrameInFlightCount)
    uint32_t                ImageCount;         // Number of simultaneous in-flight frames (returned by vkGetSwapchainImagesKHR, usually derived from min_image_count)
    uint32_t                SemaphoreCount;     // Number of simultaneous in-flight frames + 1, to be able to use it in vkAcquireNextImageKHR
    uint32_t                SemaphoreIndex;     // Current set of swapchain wait semaphores we're using (needs to be distinct from per frame data)
    std::vector<Swap> Frames;
    std::vector<SwapSemaphores> FrameSemaphores;
    bool need_rebuild;

    int GetMinImageCountFromPresentMode(vk::PresentModeKHR present_mode)
    {
        if (present_mode == vk::PresentModeKHR::eMailbox)
            return 3;
        if (present_mode == vk::PresentModeKHR::eFifo || present_mode == vk::PresentModeKHR::eFifoRelaxed)
            return 2;
        if (present_mode == vk::PresentModeKHR::eImmediate)
            return 1;
        std::unreachable();
    }

    // Also destroy old swap chain and in-flight frames data, if any.
    void CreateWindowSwapChain(const vk::raii::PhysicalDevice& physical_device, const vk::raii::Device& device, int w, int h, uint32_t min_image_count, uint32_t queue_family)
    {
        device.waitIdle();

        // We don't use ImGui_ImplVulkanH_DestroyWindow() because we want to preserve the old swapchain to create the new one.
        // Destroy old Framebuffer
        Frames.clear();
        FrameSemaphores.clear();

        // If min image count was not specified, request different count of images dependent on selected present mode
        if (min_image_count == 0)
            min_image_count = GetMinImageCountFromPresentMode(PresentMode);

        // Create Swapchain
        {
            auto cap = physical_device.getSurfaceCapabilitiesKHR(Surface);

            vk::SwapchainCreateInfoKHR info = {};
            info.surface = Surface;
            info.minImageCount = min_image_count;
            info.imageFormat = SurfaceFormat.format;
            info.imageColorSpace = SurfaceFormat.colorSpace;
            info.imageArrayLayers = 1;
            info.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
            info.imageSharingMode = vk::SharingMode::eExclusive;           // Assume that graphics family == present family
            info.preTransform = (cap.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity) ? vk::SurfaceTransformFlagBitsKHR::eIdentity : cap.currentTransform;
            if (cap.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)
                info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
            else if (cap.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eInherit)
                info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eInherit;
            else
                IM_ASSERT(false && "No supported composite alpha mode found!");
            info.presentMode = PresentMode;
            info.clipped = vk::True;
            info.oldSwapchain = std::move(Swapchain);
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
            Swapchain = device.createSwapchainKHR(info);
            auto backbuffers = Swapchain.getImages();
            ImageCount = backbuffers.size();
            
            SemaphoreCount = ImageCount + 1;
            Frames.resize(ImageCount);
            FrameSemaphores.resize(SemaphoreCount);
            for (uint32_t i = 0; i < ImageCount; i++) {
                Frames[i].Backbuffer = backbuffers[i];
                Frames[i].init(device, queue_family);
            }
            for (auto& x : FrameSemaphores) {
                x.init(device);
            }
        }

        // Create the Render Pass
        if (UseDynamicRendering == false)
        {
            vk::AttachmentDescription attachment = {
                .format = vk::Format::eUndefined,    // Will automatically use wd->SurfaceFormat.format.
                .samples = vk::SampleCountFlagBits::e1,
                .loadOp = vk::AttachmentLoadOp::eClear,
                .storeOp = vk::AttachmentStoreOp::eStore,
                .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
                .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
                .initialLayout = vk::ImageLayout::eUndefined,
                .finalLayout = vk::ImageLayout::ePresentSrcKHR
            };
            if (attachment.format == vk::Format::eUndefined)
                attachment.format = SurfaceFormat.format;
            vk::AttachmentReference color_attachment = {};
            color_attachment.attachment = 0;
            color_attachment.layout = vk::ImageLayout::eColorAttachmentOptimal;
            vk::SubpassDescription subpass = {};
            subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &color_attachment;
            vk::SubpassDependency dependency = {};
            dependency.srcSubpass = vk::SubpassExternal;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
            dependency.dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
//            dependency.srcAccessMask = 0;
            dependency.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
            vk::RenderPassCreateInfo info = {};
            info.attachmentCount = 1;
            info.pAttachments = &attachment;
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            info.dependencyCount = 1;
            info.pDependencies = &dependency;
            RenderPass = device.createRenderPass(info);

            // We do not create a pipeline by default as this is also used by examples' main.cpp,
            // but secondary viewport in multi-viewport mode may want to create one with:
            //ImGui_ImplVulkan_CreatePipeline(device, allocator, VK_NULL_HANDLE, wd->RenderPass, VK_SAMPLE_COUNT_1_BIT, &wd->Pipeline, v->Subpass);
        }

        // Create The Image Views
        {
            vk::ImageViewCreateInfo info = {};
            info.viewType = vk::ImageViewType::e2D;
            info.format = SurfaceFormat.format;
            info.components.r = vk::ComponentSwizzle::eR;
            info.components.g = vk::ComponentSwizzle::eG;
            info.components.b = vk::ComponentSwizzle::eB;
            info.components.a = vk::ComponentSwizzle::eA;
            vk::ImageSubresourceRange image_range = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
            info.subresourceRange = image_range;
            for (uint32_t i = 0; i < ImageCount; i++)
            {
                auto fd = &Frames[i];
                info.image = fd->Backbuffer;
                fd->BackbufferView = device.createImageView(info);
            }
        }

        // Create Framebuffer
        if (UseDynamicRendering == false)
        {
            vk::ImageView attachment[1];
            vk::FramebufferCreateInfo info = {};
            info.renderPass = *RenderPass;
            info.attachmentCount = 1;
            info.pAttachments = attachment;
            info.width = Width;
            info.height = Height;
            info.layers = 1;
            for (uint32_t i = 0; i < ImageCount; i++)
            {
                auto fd = &Frames[i];
                attachment[0] = fd->BackbufferView;
                fd->Framebuffer = device.createFramebuffer(info);
            }
        }

        need_rebuild = false;
    }

    bool render(const vk::raii::Device& device, const vk::raii::Queue& queue, FrameData *frame_data, ImDrawData* draw_data)
    {
        vk::Semaphore image_acquired_semaphore = FrameSemaphores[SemaphoreIndex].ImageAcquiredSemaphore;
        vk::Semaphore render_complete_semaphore = FrameSemaphores[SemaphoreIndex].RenderCompleteSemaphore;
        auto [err, FrameIndex] = Swapchain.acquireNextImage(UINT64_MAX, image_acquired_semaphore);
        if (err != vk::Result::eSuccess) {
            if (err == vk::Result::eErrorOutOfDateKHR || err == vk::Result::eSuboptimalKHR) {
                need_rebuild = true;
            }
            return false;
        }

        auto* fd = &Frames[FrameIndex];
        err = device.waitForFences(*fd->Fence, vk::True, UINT64_MAX);
        if (err != vk::Result::eSuccess)
            return false;
        device.resetFences(*fd->Fence);
        {
            fd->CommandPool.reset();
            vk::CommandBufferBeginInfo info = {
                .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
            };
            fd->CommandBuffer.begin(info);
        }
        {
            vk::RenderPassBeginInfo info = {};
            info.renderPass = RenderPass;
            info.framebuffer = fd->Framebuffer;
            info.renderArea.extent.width = Width;
            info.renderArea.extent.height = Height;
            info.clearValueCount = 1;
            info.pClearValues = &ClearValue;
            fd->CommandBuffer.beginRenderPass(info, vk::SubpassContents::eInline);
        }

        // Record dear imgui primitives into command buffer
        ImGui_ImplVulkan_RenderDrawData(draw_data, *fd->CommandBuffer);

        // Submit command buffer
        fd->CommandBuffer.endRenderPass();
        {
            vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
            vk::SubmitInfo info = {};
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores = &image_acquired_semaphore;
            info.pWaitDstStageMask = &wait_stage;
            info.commandBufferCount = 1;
            info.pCommandBuffers = &*fd->CommandBuffer;
            info.signalSemaphoreCount = 1;
            info.pSignalSemaphores = &render_complete_semaphore;

            fd->CommandBuffer.end();
            queue.submit(info, fd->Fence);
        }

        vk::PresentInfoKHR info = {};
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &render_complete_semaphore;
        info.swapchainCount = 1;
        info.pSwapchains = &*Swapchain;
        info.pImageIndices = &FrameIndex;
        err = queue.presentKHR(info);
        if (err == vk::Result::eErrorOutOfDateKHR || err == vk::Result::eSuboptimalKHR)
            need_rebuild = true;
        SemaphoreIndex = (SemaphoreIndex + 1) % SemaphoreCount; // Now we can use the next set of semaphores

        return true;
    }
};