#pragma once

#include <ass/ass.h>

#include "ffmpeg.hpp"
#include "subtitle.hpp"
#include "ass.vert.h"
#include "ass.frag.h"

class SubAss : public AppSubtitle {
private:
    std::vector<DataSet> data;
    int dataIdx = 0;

    ASS_Library *ass_library = nullptr;
    ASS_Renderer *ass_renderer = nullptr;
    ASS_Track *ass_track = nullptr;
    
	void createGraphicsPipeline(const vk::raii::Device& device, vk::Format swap_format)
	{
        vk::ShaderModuleCreateInfo shader_info{
            .codeSize = ass_vert_len,
            .pCode = (uint32_t *) ass_vert,
        };
        auto vert_shader = device.createShaderModule(shader_info);
        shader_info = vk::ShaderModuleCreateInfo{
            .codeSize = ass_frag_len,
            .pCode = (uint32_t *) ass_frag,
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

		vk::PipelineLayoutCreateInfo pipelineLayoutInfo{.setLayoutCount = 1, .pSetLayouts = &*layout, .pushConstantRangeCount = 0, .pPushConstantRanges = {}};
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
    SubAss(const vk::raii::Device& gpu) : AppSubtitle(gpu) {
        init_once();
    }
    ~SubAss() {
        shutdown();
    }

    void shutdown() {
        if (ass_track) {
            ass_free_track(ass_track);
            ass_track = nullptr;
        }
        if (ass_renderer) {
            ass_renderer_done(ass_renderer);
            ass_renderer = nullptr;
        }
        if (ass_library) {
            ass_library_done(ass_library);
            ass_library = nullptr;
        }
    }

    void init_once() {
        vk::SamplerCreateInfo info = {
            .magFilter = vk::Filter::eLinear,
            .minFilter = vk::Filter::eLinear,
            .mipmapMode = vk::SamplerMipmapMode::eLinear,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge,
/*            .maxAnisotropy = 1.0f,
            .minLod = -1000,
            .maxLod = 1000,*/
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
            .bindingCount = 2,
            .pBindings = bindings,
        };
        layout = device.createDescriptorSetLayout(layoutInfo);

        vk::DescriptorPoolSize pool_sizes[] =
        {
            { vk::DescriptorType::eCombinedImageSampler, N_INFLIGHT * N_MAX_SUBS },
            { vk::DescriptorType::eStorageBuffer, N_INFLIGHT },
        };
        vk::DescriptorPoolCreateInfo pool_info{
            .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
            .maxSets = N_INFLIGHT,
            .poolSizeCount = (uint32_t)IM_COUNTOF(pool_sizes),
            .pPoolSizes = pool_sizes
        };
        pool = device.createDescriptorPool(pool_info);

        std::vector<vk::DescriptorSetLayout> layouts(N_INFLIGHT, layout);
        vk::DescriptorSetAllocateInfo alloc_info{
            .descriptorPool = pool, // The pool we just created
            .descriptorSetCount = N_INFLIGHT,
            .pSetLayouts = layouts.data() // Your predefined VkDescriptorSetLayout
        };
        auto sets = device.allocateDescriptorSets(alloc_info);

        vk::CommandPoolCreateInfo poolInfo = {
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = queueIndex
        };
        commandPool = device.createCommandPool(poolInfo);

        vk::CommandBufferAllocateInfo allocInfo = {
            .commandPool = commandPool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = N_INFLIGHT
        };
        auto commandBuffers = device.allocateCommandBuffers(allocInfo);

        vk::FenceCreateInfo fence_info = {
            .flags = vk::FenceCreateFlagBits::eSignaled,
        };

        for (auto i = 0; i < N_INFLIGHT; i++) {
            data.push_back({
                .copyFence = device.createFence(fence_info),
                .set = std::move(sets[i]),
                .commandBuffer = std::move(commandBuffers[i]),
            });
        }

        ass_library = ass_library_init();
    }

    bool init(int width, int height, AVCodecContext *subtitle_codec_ctx, AVFormatContext *format_ctx, SDL_Window *window) {
        if (ass_track) {
            ass_free_track(ass_track);
            ass_track = nullptr;
        }
        if (ass_renderer) {
            ass_renderer_done(ass_renderer);
            ass_renderer = nullptr;
        }
        SDL_GetWindowSizeInPixels(window, &wnd_w, &wnd_h);

        // 1. Initialize your standard libass environment
        load_embedded_fonts(format_ctx);
        ass_renderer = ass_renderer_init(ass_library);
        ass_set_fonts(ass_renderer, nullptr, "Sans", 1, nullptr, 0);
        ass_set_storage_size(ass_renderer, width, height);
        ass_set_frame_size(ass_renderer, wnd_w, wnd_h); // Match your window canvas size
//        ass_set_hinting(ass_renderer, ASS_HINTING_NATIVE);

        // 2. Create an EMPTY, blank track that you will feed packets manually
        ass_track = ass_new_track(ass_library);

        if (subtitle_codec_ctx->subtitle_header_size > 0) {
            ass_process_codec_private(
                ass_track, 
                (char*)subtitle_codec_ctx->subtitle_header, 
                subtitle_codec_ctx->subtitle_header_size
            );

            createGraphicsPipeline(device, swapChainSurfaceFormat.format);
            return true;
        }
        return false;
    }

    void load_embedded_fonts(AVFormatContext* format_ctx) {
        ass_clear_fonts(ass_library);

        // Loop through all tracks/streams in the container
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            AVStream* stream = format_ctx->streams[i];
            
            // Look specifically for file attachments
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT) {
                
                // Extract the font filename from metadata
                std::string font_name = "unknown_font";
                AVDictionaryEntry* tag = av_dict_get(stream->metadata, "filename", nullptr, 0);
                if (tag && tag->value) {
                    font_name = tag->value;
                }

                // Verify that the attachment contains data
                if (stream->codecpar->extradata && stream->codecpar->extradata_size > 0) {
                    
                    char* font_data = reinterpret_cast<char*>(stream->codecpar->extradata);
                    int font_data_size = stream->codecpar->extradata_size;

                    // Register the raw font buffer into libass's internal memory pool
                    ass_add_font(ass_library, font_name.c_str(), font_data, font_data_size);
                    
//                    std::cout << "Successfully matched and loaded font: " << font_name 
//                            << " (" << font_data_size << " bytes)" << std::endl;
                }
            }
        }
    }   

    void flush() {
        if (ass_track)
            ass_flush_events(ass_track);
    }

    void add_ass(const std::string& text, long long pts, long long duration) {
        ass_process_chunk(
                        ass_track, 
                        text.c_str(),          // The raw time-stripped ASS string payload
                        text.length(),  // String length
                        pts,             // Explicit start time (ms)
                        duration         // Explicit duration length (ms)
        );
    }

    void upload_data(DataSet& ds, const vk::raii::Queue& queue) {
        if (ds.n_images == 0)
            return;

        auto size = ds.vertices.size() * sizeof(Vertex);
        ds.alloc_buf(device, size);

        uint8_t *map = (uint8_t *) ds.memory.mapMemory(0, size);
        uint8_t *src = (uint8_t *) ds.vertices.data();
        memcpy(map, src, size);
        ds.memory.unmapMemory();

        auto textureCount = ds.n_images;
        std::vector<vk::DescriptorImageInfo> imageInfos(textureCount);
        std::vector<vk::WriteDescriptorSet> descriptorWrites(textureCount + 1);

        device.resetFences(*ds.copyFence);
        ds.commandBuffer.reset();
        vk::CommandBufferBeginInfo begin_info{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
        };
        ds.commandBuffer.begin(begin_info);

        uint32_t i = 0;
        for (; i < ds.n_images; i++) {
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
        vk::SubmitInfo end_info = {
            .commandBufferCount = 1,
            .pCommandBuffers = &*ds.commandBuffer
        };
        queue.submit(end_info, ds.copyFence);

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

    void prepare_draw(const vk::raii::Queue& queue, double play_time) {
        if (!ass_track)
            return;

        auto next_idx = (dataIdx + 1) % N_INFLIGHT;
        auto& ds = data[next_idx];
        auto err = device.waitForFences(*ds.copyFence, vk::True, 0);
        if (err != vk::Result::eSuccess)
            return;

        int changed = 0;
        // Ask libass to process the track at this specific millisecond frame marker
        ASS_Image* img = ass_render_frame(ass_renderer, ass_track, play_time * 1000, &changed);
        if (changed == 0)
            return;

        ds.vertices.clear();

        int idx = 0;
        int alloc_images = ds.images.size();
        // 3. Draw the active text lines over the frame canvas
        for (; img; img = img->next) {
//            printf("SUCCESS: libass generated image chunks! w=%d, h=%d at position x=%d, y=%d\n", img->w, img->h, img->dst_x, img->dst_y);
            if (img->w == 0 || img->h == 0)
                continue;

            if (idx >= alloc_images)
                ds.images.emplace_back();
            ImageData& id = ds.images[idx];
            id.init(device, img->w, img->h);

            uint8_t *map = (uint8_t *) id.up_memory.mapMemory(0, img->w * img->h);
            const uint8_t* src = img->bitmap;
            for (int y = 0; y < img->h; ++y) {
                memcpy(map + (y * img->w), src + (y * img->stride), img->w);
            }
            id.up_memory.unmapMemory();
            
            uint32_t c = img->color;
            float r = ((c >> 24) & 0xFF) / 255.0f;
            float g = ((c >> 16) & 0xFF) / 255.0f;
            float b = ((c >> 8)  & 0xFF) / 255.0f;
            float a = (255 - (c & 0xFF)) / 255.0f;

            ds.vertices.push_back({
                2.0f * img->dst_x / wnd_w - 1.0f, 1.0f - 2.0f * img->dst_y / wnd_h,
                2.0f * img->w / wnd_w, 2.0f * img->h / wnd_h,
                r, g, b, a,
                (float) img->w / id.alloc_w, (float) img->h / id.alloc_h
            });
            idx++;
        }
        ds.n_images = idx;

        upload_data(ds, queue);
        ds.status = DataSet::Upload;
        ds.play_time = play_time;
    }

    void draw(const vk::CommandBuffer commandBuffer) {
        auto& ds = data[dataIdx];
        if (ds.status != DataSet::Ready)
            return;

        if (ds.n_images == 0)
            return;

        auto err = device.waitForFences(*ds.copyFence, vk::True, 0);
        if (err != vk::Result::eSuccess)
            return;

        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, *ds.set, nullptr);
        commandBuffer.draw(ds.vertices.size() * 6, 1, 0, 0);
    }

    void window_size_changed(Sint32 w, Sint32 h) {
        wnd_w = w;
        wnd_h = h;
        if (ass_renderer)
            ass_set_frame_size(ass_renderer, wnd_w, wnd_h); // Match your window canvas size
    }

    bool check_next_frame(double play_time) {
        auto next_idx = (dataIdx + 1) % N_INFLIGHT;
        auto& ad = data[next_idx];
        if (ad.status == DataSet::Upload) {
            if (ad.play_time <= play_time) {
                auto err = device.waitForFences(*ad.copyFence, vk::True, 0);
                if (err == vk::Result::eSuccess) {
                    ad.status = DataSet::Ready;
                    dataIdx = next_idx;
                    return true;
                }
            }
            return false;
        } else if (ad.status == DataSet::Discard) {
            auto err = device.waitForFences(*ad.copyFence, vk::True, 0);
            if (err == vk::Result::eSuccess) {
                return true;
            }
            return false;
        }
        return true;
    }
};
