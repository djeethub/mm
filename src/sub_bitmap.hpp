#pragma once

#include <list>

#include "subtitle.hpp"
#include "bitmap.vert.h"
#include "bitmap.frag.h"

struct alignas(8) BmpForm {
    float position[2]; // x, y (in NDC: -1.0 to 1.0)
    float size[2];     // width, height (in NDC: 0.0 to 2.0)
    float uv_size[2];
};

using BmpImageSet = ImageSet<BmpForm>;

class SubBitmap : public AppSubtitle {
private:
    BmpImageSet frames[N_INFLIGHT_SUB];
    int frame_idx = 0;

    std::list<ff::AVSubtitle_ *> sub_list;
    int canvas_w;
    int canvas_h;
    ff::AVSubtitle_ *prev = nullptr;

	void createGraphicsPipeline(const vk::raii::Device& device, vk::Format swap_format)
	{
        vk::ShaderModuleCreateInfo shader_info{
            .codeSize = bitmap_vert_len,
            .pCode = (uint32_t *) bitmap_vert,
        };
        auto vert_shader = device.createShaderModule(shader_info);
        shader_info = vk::ShaderModuleCreateInfo{
            .codeSize = bitmap_frag_len,
            .pCode = (uint32_t *) bitmap_frag,
        };
        auto frag_shader = device.createShaderModule(shader_info);

		vk::PipelineShaderStageCreateInfo vertShaderStageInfo{.stage = vk::ShaderStageFlagBits::eVertex, .module = vert_shader, .pName = "main"};
		vk::PipelineShaderStageCreateInfo fragShaderStageInfo{.stage = vk::ShaderStageFlagBits::eFragment, .module = frag_shader, .pName = "main"};
		vk::PipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

		vk::PipelineVertexInputStateCreateInfo   vertexInputInfo{.vertexBindingDescriptionCount   = 0,
		                                                         .pVertexBindingDescriptions      = nullptr,
		                                                         .vertexAttributeDescriptionCount = 0,
		                                                         .pVertexAttributeDescriptions    = nullptr};
		vk::PipelineInputAssemblyStateCreateInfo inputAssembly{.topology = vk::PrimitiveTopology::eTriangleList};

		vk::PipelineViewportStateCreateInfo      viewportState{.viewportCount = 1, .scissorCount = 1};

		vk::PipelineRasterizationStateCreateInfo rasterizer{.depthClampEnable        = vk::False,
		                                                    .rasterizerDiscardEnable = vk::False,
		                                                    .polygonMode             = vk::PolygonMode::eFill,
		                                                    .cullMode                = vk::CullModeFlagBits::eNone,
		                                                    .frontFace               = vk::FrontFace::eCounterClockwise,
		                                                    .depthBiasEnable         = vk::False,
		                                                    .lineWidth               = 1.0f};

		vk::PipelineMultisampleStateCreateInfo multisampling{.rasterizationSamples = vk::SampleCountFlagBits::e1, .sampleShadingEnable = vk::False};

		vk::PipelineColorBlendAttachmentState colorBlendAttachment{
		    .blendEnable    = vk::True,
            .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
            .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne,
            .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .alphaBlendOp = vk::BlendOp::eAdd,
		    .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};

		vk::PipelineColorBlendStateCreateInfo colorBlending{
		    .logicOpEnable = vk::False, .logicOp = vk::LogicOp::eCopy, .attachmentCount = 1, .pAttachments = &colorBlendAttachment};

		std::vector<vk::DynamicState>      dynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
		vk::PipelineDynamicStateCreateInfo dynamicState{.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()), .pDynamicStates = dynamicStates.data()};

		vk::PipelineLayoutCreateInfo pipelineLayoutInfo{.setLayoutCount = 1, .pSetLayouts = &*layout};
		pipelineLayout = vk::raii::PipelineLayout(device, pipelineLayoutInfo);

		vk::StructureChain<vk::GraphicsPipelineCreateInfo, vk::PipelineRenderingCreateInfo> pipelineCreateInfoChain = {
		    {
                .stageCount          = 2,
                .pStages             = shaderStages,
   		        .pVertexInputState   = &vertexInputInfo,
                .pInputAssemblyState = &inputAssembly,
                .pViewportState      = &viewportState,
                .pRasterizationState = &rasterizer,
                .pMultisampleState   = &multisampling,
                .pColorBlendState    = &colorBlending,
                .pDynamicState       = &dynamicState,
                .layout              = pipelineLayout,
            },
		    {.colorAttachmentCount = 1, .pColorAttachmentFormats = &swap_format}
        };

		pipeline = vk::raii::Pipeline(device, nullptr, pipelineCreateInfoChain.get<vk::GraphicsPipelineCreateInfo>());
	}

public:
    SubBitmap(const vk::raii::Device& gpu) : AppSubtitle(gpu) {
        init_once();
    }
    ~SubBitmap() {
        shutdown();
    }

    void shutdown() {
        flush();
    }

    void init_once() {
        vk::SamplerCreateInfo info = {
            .magFilter = vk::Filter::eLinear,
            .minFilter = vk::Filter::eLinear,
            .mipmapMode = vk::SamplerMipmapMode::eLinear,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        };
        sampler = device.createSampler(info);

        // Define the Descriptor Set Layout with an Immutable Sampler
        vk::DescriptorSetLayoutBinding bindings[] = {
            {
                .binding = 0,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .descriptorCount = N_MAX_SUBS,
                .stageFlags = vk::ShaderStageFlagBits::eFragment,
            },
            {
                .binding = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eVertex,
            },
        };
        vk::DescriptorSetLayoutCreateInfo layoutInfo{
            .bindingCount = std::size(bindings),
            .pBindings = bindings,
        };
        layout = device.createDescriptorSetLayout(layoutInfo);

        vk::DescriptorPoolSize pool_sizes[] =
        {
            { vk::DescriptorType::eCombinedImageSampler, N_INFLIGHT_SUB * N_MAX_SUBS },
            { vk::DescriptorType::eStorageBuffer, N_INFLIGHT_SUB },
        };
        vk::DescriptorPoolCreateInfo pool_info{
//            .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
            .maxSets = N_INFLIGHT_SUB,
            .poolSizeCount = std::size(pool_sizes),
            .pPoolSizes = pool_sizes
        };
        pool = device.createDescriptorPool(pool_info);

        std::vector<vk::DescriptorSetLayout> layouts(N_INFLIGHT_SUB, layout);
        vk::DescriptorSetAllocateInfo alloc_info{
            .descriptorPool = pool,
            .descriptorSetCount = N_INFLIGHT_SUB,
            .pSetLayouts = layouts.data()
        };
        auto sets = (*device).allocateDescriptorSets(alloc_info);

        vk::CommandPoolCreateInfo poolInfo = {
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
            .queueFamilyIndex = queueIndex
        };
        commandPool = device.createCommandPool(poolInfo);

        vk::CommandBufferAllocateInfo allocInfo = {
            .commandPool = commandPool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = N_INFLIGHT_SUB
        };
        auto commandBuffers = (*device).allocateCommandBuffers(allocInfo);

        vk::FenceCreateInfo fence_info = {
            .flags = vk::FenceCreateFlagBits::eSignaled,
        };

        vk::SemaphoreTypeCreateInfo timelineInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        };
        vk::SemaphoreCreateInfo semaphoreInfo{
            .pNext = &timelineInfo
        };

        for (auto i = 0; i < N_INFLIGHT_SUB; i++) {
            frames[i] = {
                .semaphore = device.createSemaphore(semaphoreInfo),
                .copyFence = device.createFence(fence_info),
                .set = sets[i],
                .commandBuffer = commandBuffers[i],
            };
        }

        createGraphicsPipeline(device, swapChainSurfaceFormat.format);
    }

    bool init(AVCodecContext *sub_codec_ctx, SDL_Window *window) {
        flush();

        SDL_GetWindowSizeInPixels(window, &wnd_w, &wnd_h);

        canvas_w = sub_codec_ctx->width;
        canvas_h = sub_codec_ctx->height;

        // Level 1: Fallback to video stream dimensions
        // (Most bitmap subtitles without explicit headers are authored to match video canvas)
        if (canvas_w <= 0 || canvas_h <= 0) {
//            canvas_w = video_codec_ctx->width;
//            canvas_h = video_codec_ctx->height;
        }

        // Level 2: Format-specific defaults (if video dimensions are also unavailable)
        if (canvas_w <= 0 || wnd_h <= 0) {
            if (sub_codec_ctx->codec_id == AV_CODEC_ID_DVD_SUBTITLE) {
                canvas_w = 720;
                canvas_h = 480;
            } else {
                // Last resort global default
                canvas_w = 1920;
                canvas_h = 1080;
            }
        }

        commandPool.reset();

        return true;
    }

    void flush() {
        for (auto sub : sub_list) {
            ff::subtitle_recycle(sub);
        }
        sub_list.clear();
        prev = nullptr;

        for (auto i = 0; i < N_INFLIGHT_SUB; i++) {
            frames[i].status = BmpImageSet::Discard;
        }
    }

    void add_sub(ff::AVSubtitle_ *sub) {
        sub_list.push_back(sub);
    }

    void upload_data(BmpImageSet& ds, const vk::raii::Queue& queue) {
        if (ds.n_images == 0)
            return;

        auto size = ds.vertices.size() * sizeof(BmpForm);
        ds.alloc_buf(device, size);
        uint8_t *map = (uint8_t *) ds.memory.mapMemory(0, size);
        uint8_t *src = (uint8_t *) ds.vertices.data();
        memcpy(map, src, size);
        ds.memory.unmapMemory();

        auto textureCount = ds.n_images;
        std::vector<vk::DescriptorImageInfo> imageInfos(textureCount);
        std::vector<vk::WriteDescriptorSet> descriptorWrites(textureCount + 1);

        device.resetFences(*ds.copyFence);
        vk::CommandBufferBeginInfo begin_info{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
        };
        ds.commandBuffer.begin(begin_info);

        uint32_t i = 0;
        for (; i < textureCount; i++) {
            auto& id = ds.images[i];
//            SDL_Log("idx %i atlas %i vertices %i\n", dataIdx, ad.atlas_pool.in_use_list.size(), atlas->vertices.size());
            vk::ImageMemoryBarrier2 imageBarrier = {
                .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eHost,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
//                .oldLayout = atlas->newly_created ? vk::ImageLayout::eUndefined : vk::ImageLayout::eShaderReadOnlyOptimal,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .image = *id.image,
                .subresourceRange = {
                    .aspectMask = vk::ImageAspectFlags::BitsType::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
            };
            vk::DependencyInfo dep_info{
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &imageBarrier,
            };
            ds.commandBuffer.pipelineBarrier2(dep_info);

            vk::BufferImageCopy2 region{
                .imageSubresource = {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .layerCount = 1,
                },
                .imageExtent = {id.w, id.h, 1},
            };
            vk::CopyBufferToImageInfo2 copy_info = {
                .srcBuffer = id.up_buffer,
                .dstImage = id.image,
                .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
                .regionCount = 1,
                .pRegions = &region,
            };
            ds.commandBuffer.copyBufferToImage2(copy_info);

            imageBarrier = {
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .image = *id.image,
                .subresourceRange = {
                    .aspectMask = vk::ImageAspectFlags::BitsType::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
            };
            dep_info = {
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &imageBarrier,
            };
            ds.commandBuffer.pipelineBarrier2(dep_info);

            // Define the image view and sampler for this array slot
            imageInfos[i] = {
                .sampler     = sampler,
                .imageView   = id.imageView,
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            };
            descriptorWrites[i] = {
                .dstSet          = ds.set,
                .dstArrayElement = i,
                .descriptorCount = 1,
                .descriptorType  = vk::DescriptorType::eCombinedImageSampler,
                .pImageInfo      = &imageInfos[i],
            };
        }

        ds.commandBuffer.end();
        vk::CommandBufferSubmitInfo cmd_info{
            .commandBuffer = ds.commandBuffer,
        };
        vk::SemaphoreSubmitInfo signal_info{
            .semaphore = ds.semaphore,
            .value = ++ds.counter
        };
        vk::SubmitInfo2 submit_info{
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &cmd_info,
            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos = &signal_info
        };
        queue.submit2(submit_info, ds.copyFence);

        vk::DescriptorBufferInfo bufInfo{
            .buffer = *ds.buffer,
            .range = (vk::DeviceSize)size
        };
        descriptorWrites[i] = {
            .dstSet = ds.set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &bufInfo,
        };
        device.updateDescriptorSets(descriptorWrites, nullptr);
    }

    ff::AVSubtitle_ *get_next_subtitle(double play_time, bool& changed) {
        ff::AVSubtitle_ *sub = nullptr;

        for (auto it = sub_list.begin(); it != sub_list.end(); ) {
            if ((*it)->frame_time > play_time)
                break;
            while (true) {
                auto next = std::next(it);
                if (next == sub_list.end() || (*next)->frame_time > play_time)
                    break;
                ff::subtitle_recycle((*it));
                it = sub_list.erase(it);
            }
            sub = (*it);
            if (sub->frame_time + sub->duration < play_time) {
                ff::subtitle_recycle(sub);
                it = sub_list.erase(it);
                sub = nullptr;
            }
            break;
        }
        changed = prev != sub;
        prev = sub;
        return sub;
    }

    void prepare_draw(const vk::raii::Queue& queue, double play_time) {
        auto next_idx = (frame_idx + 1) % N_INFLIGHT_SUB;
        auto& ds = frames[next_idx];
        auto err = device.waitForFences(*ds.copyFence, vk::True, 0);
        if (err != vk::Result::eSuccess)
            return;

        bool changed = false;
        auto sub = get_next_subtitle(play_time, changed);
        if (!changed)
            return;

        int idx = 0;
        if (sub) {
            ds.vertices.clear();
            int alloc_images = ds.images.size();
            for (auto i = 0; i < sub->num_rects; i++) {
                auto rect = sub->rects[i];
                if (rect->type != SUBTITLE_BITMAP)
                    continue;
                if (rect->w == 0 || rect->h == 0)
                    continue;

                if (idx >= alloc_images)
                    ds.images.emplace_back();
                ImageData& id = ds.images[idx];
                id.init(device, rect->w, rect->h, vk::Format::eB8G8R8A8Unorm);

                const auto w   = rect->w;
                const auto h   = rect->h;
                const auto src_stride = rect->linesize[0];
                uint8_t *src = rect->data[0];
                uint32_t *pal = (uint32_t *) rect->data[1];
                uint32_t *map = (uint32_t *) id.up_memory.mapMemory(0, w * h * 4);
                if (src_stride == w) {
                    const auto n = w * h;
                    for (auto x = 0; x < n; ++x)
                        *map++ = pal[*src++];
                } else {
                    for (auto y = 0; y < h; y++) {
                        for (auto x = 0; x < w; x++) {
                            *map++ = pal[*src++];
                        }
                        src += src_stride - w;
                    }
                }
                id.up_memory.unmapMemory();

                ds.vertices.push_back({
                    2.0f * (rect->x + w / 2.0f) / canvas_w - 1.0f, 1.0f - 2.0f * (rect->y + h / 2.0f) / canvas_h,
                    2.0f * w / wnd_w, 2.0f * h / wnd_h,
                    (float) w / id.alloc_w, (float) h / id.alloc_h
                });
                idx++;
            }
        }
        ds.n_images = idx;

        upload_data(ds, queue);
        ds.status = BmpImageSet::Upload;
        ds.play_time = play_time;
    }

    void draw(const vk::CommandBuffer commandBuffer, std::vector<vk::SemaphoreSubmitInfo>& wait_info) {
        auto next_idx = (frame_idx + 1) % N_INFLIGHT_SUB;
        if (frames[next_idx].status == BmpImageSet::Ready) {
            frames[frame_idx].status = BmpImageSet::New;
            frame_idx = next_idx;
        }
        auto& ds = frames[frame_idx];
        if (ds.status != BmpImageSet::Ready || ds.n_images == 0)
            return;

        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, ds.set, nullptr);
        commandBuffer.draw(ds.vertices.size() * 6, 1, 0, 0);

        wait_info.push_back({
            .semaphore = ds.semaphore,
            .value = ds.counter,
            .stageMask = vk::PipelineStageFlagBits2::eFragmentShader
        });
    }

    void window_size_changed(Sint32 w, Sint32 h) {
        wnd_w = w;
        wnd_h = h;
    }

    bool check_next_frame(double play_time) {
        auto next_idx = (frame_idx + 1) % N_INFLIGHT_SUB;
        auto& ad = frames[next_idx];
        switch (ad.status) {
        case BmpImageSet::Upload:
            if (ad.play_time <= play_time) {
                ad.status = BmpImageSet::Ready;
            }
            break;
        case BmpImageSet::Discard:
            if (device.waitForFences(*ad.copyFence, vk::True, 0) == vk::Result::eSuccess) {
                ad.status = BmpImageSet::New;
                return true;
            }
            return false;
        case BmpImageSet::Ready:
            break;
        default:
            return true;
        }

        return false;
    }
};
