#pragma once

#include <ass/ass.h>

#include "ffmpeg.hpp"
#include "subtitle.hpp"
#include "ass.vert.h"
#include "ass.frag.h"

#define N_DATA 2

struct AtlasRegion {
    float u0, v0; // Top-Left UV
    float u1, v1; // Bottom-Right UV
};

struct Vertex {
    float x, y;
    float w, h;
    AtlasRegion uv;
    float r, g, b, a;
};

struct Atlas {
    vk::raii::Image image = nullptr;
    vk::raii::DeviceMemory memory = nullptr;
    vk::raii::ImageView imageView = nullptr;
    vk::raii::Buffer up_buffer = nullptr;
    vk::raii::DeviceMemory up_memory = nullptr;
    vk::raii::Buffer *vert_buf = nullptr;
    int vert_size;
    Uint32 w;
    Uint32 h;
    std::vector<Vertex> vertices;
    bool newly_created = true;

// Shelf packer state
    uint32_t current_x = 0;
    uint32_t current_y = 0;
    uint32_t current_shelf_height = 0;
    const uint32_t padding = 1; // 1px padding to avoid bilinear filtering artifacts
    Uint32 offset;

    // Allocate space for a new glyph inside the atlas
    bool alloc_region(uint32_t glyph_w, uint32_t glyph_h, uint32_t& out_x, uint32_t& out_y, AtlasRegion& out_uv) {
        uint32_t alloc_w = glyph_w + padding;
        uint32_t alloc_h = glyph_h + padding;

        // Check if glyph fits on the current shelf
        if (current_x + alloc_w > w) {
            // Move down to next shelf
            current_y += current_shelf_height;
            current_x = 0;
            current_shelf_height = 0;
        }

        // Check if atlas is completely full
        if (current_y + alloc_h > h) {
            return false; // Atlas full! Needs flush or clear.
        }

        out_x = current_x;
        out_y = current_y;

        // Calculate normalized UV coordinates
        out_uv.u0 = (float)out_x / (float)w;
        out_uv.v0 = (float)out_y / (float)h;
        out_uv.u1 = (float)glyph_w / (float)w;
        out_uv.v1 = (float)glyph_h / (float)h;

        // Update shelf trackers
        current_x += alloc_w;
        if (alloc_h > current_shelf_height) {
            current_shelf_height = alloc_h;
        }

        return true;
    }

    // Reset atlas state when cleared (or when seeking video)
    void reset() {
        vertices.clear();
        current_x = 0;
        current_y = 0;
        current_shelf_height = 0;
        offset = 0;
        vert_buf = nullptr;
    }
};

class AtlasPool : public GPUPool<Atlas> {
public:
    using GPUPool<Atlas>::GPUPool;

    Atlas *alloc(const vk::raii::Device&device, Uint32 w, Uint32 h) {
        if (!list.empty()) {
            auto data = list.back();
            list.pop_back();
            return data;
        }

        auto atlas = new Atlas{.w = w, .h = h};

        vk::ImageCreateInfo info = {
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8Unorm,
            .extent = { .width = (uint32_t) w, .height = (uint32_t) h, .depth = 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        atlas->image = device.createImage(info);
        auto req = atlas->image.getMemoryRequirements();
        vk::MemoryAllocateInfo mem_alloc_info = {};
        mem_alloc_info.allocationSize = req.size;
        mem_alloc_info.memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        atlas->memory = device.allocateMemory(mem_alloc_info);
        atlas->image.bindMemory(atlas->memory, 0);

        vk::ImageViewCreateInfo viewInfo{
            .image = atlas->image,     // The VkImage containing your uploaded AVFrame data
            .viewType = vk::ImageViewType::e2D,
            .format = info.format,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor, // Vulkan handles sub-planes internally
                .levelCount = 1,
                .layerCount = 1
            }
        };
        atlas->imageView = device.createImageView(viewInfo);
        
        auto upload_size = w * h;
        {
            vk::BufferCreateInfo buffer_info = {
                .size = upload_size,
                .usage = vk::BufferUsageFlagBits::eTransferSrc,
                .sharingMode = vk::SharingMode::eExclusive
            };
            atlas->up_buffer = device.createBuffer(buffer_info);
            auto req = atlas->up_buffer.getMemoryRequirements();
            vk::MemoryAllocateInfo alloc_info = {
                .allocationSize = req.size,
                .memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)
            };
            atlas->up_memory = device.allocateMemory(alloc_info);
            atlas->up_buffer.bindMemory(atlas->up_memory, 0);
        }

        return atlas;
    }
};

struct VertexBuf {
    vk::raii::Buffer buf = nullptr;
    vk::raii::DeviceMemory memory = nullptr;
    Uint32 size;

    void reset() {}
};

class VertexPool : public GPUPool<VertexBuf> {
public:
    using GPUPool<VertexBuf>::GPUPool;

    VertexBuf *alloc(const vk::raii::Device& device, Uint32 size) {
        while (!list.empty()) {
            auto data = list.back();
            list.pop_back();
            if (data->size >= size)
                return data;
            delete data;
        }

        size *= 3;
        auto vert_buf = new VertexBuf{.size = size};

        vk::BufferCreateInfo buffer_info = {
            .size = size,
            .usage = vk::BufferUsageFlagBits::eStorageBuffer,
            .sharingMode = vk::SharingMode::eExclusive
        };
        vert_buf->buf = device.createBuffer(buffer_info);
        auto req = vert_buf->buf.getMemoryRequirements();
        vk::MemoryAllocateInfo alloc_info = {
            .allocationSize = req.size,
            .memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)
        };
        vert_buf->memory = device.allocateMemory(alloc_info);
        vert_buf->buf.bindMemory(vert_buf->memory, 0);

        return vert_buf;
    }
};

struct AssData {
    vk::raii::CommandBuffer commandBuffer = nullptr;
    vk::raii::Fence copyFence = nullptr;

    AtlasPool atlas_pool;
    VertexPool vertex_pool;
};

class SubAss : public AppSubtitle {
private:
    enum Status {
        Idle,
        Init,
    };

    std::vector<AssData> data;
    int frameIdx = 0;
    int dataIdx = 0;

    ASS_Library *ass_library = nullptr;
    ASS_Renderer *ass_renderer = nullptr;
    ASS_Track *ass_track = nullptr;
    
    std::vector<vk::BufferImageCopy2> regions;
    Status status_upload = Idle;

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
        for (auto& ad : data) {
            ad.atlas_pool.clear();
            ad.vertex_pool.clear();
        }

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
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eFragment,
                .pImmutableSamplers = &*sampler // <-- Baked directly into the layout binding!
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
            { vk::DescriptorType::eCombinedImageSampler, N_INFLIGHT },
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
        // Allocate a descriptor set from the pool
        vk::DescriptorSetAllocateInfo alloc_info{
            .descriptorPool = pool, // The pool we just created
            .descriptorSetCount = N_INFLIGHT,
            .pSetLayouts = layouts.data() // Your predefined VkDescriptorSetLayout
        };
        sets = device.allocateDescriptorSets(alloc_info);

        vk::CommandPoolCreateInfo poolInfo = {
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = queueIndex
        };
        commandPool = device.createCommandPool(poolInfo);

        vk::CommandBufferAllocateInfo allocInfo = {
            .commandPool = commandPool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = N_DATA
        };
        auto commandBuffers = device.allocateCommandBuffers(allocInfo);

        vk::FenceCreateInfo fence_info = {
            .flags = vk::FenceCreateFlagBits::eSignaled,
        };

        for (auto i = 0; i < N_DATA; i++) {
            data.emplace_back(AssData{
                .commandBuffer = std::move(commandBuffers[i]),
                .copyFence = device.createFence(fence_info),
            });
        }

        queue = device.getQueue(queueIndex, 0);

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

    void upload_vertices(Atlas *atlas) {
        auto& ad = data[dataIdx];

        Uint32 size = atlas->vertices.size() * sizeof(Vertex);
        auto vert = ad.vertex_pool.alloc(device, size);

        uint8_t *map = (uint8_t *) vert->memory.mapMemory(0, size);
        uint8_t *src = (uint8_t *) atlas->vertices.data();
        memcpy(map, src, size);
        vert->memory.unmapMemory();
        atlas->vert_buf = &vert->buf;
        atlas->vert_size = size;
        ad.vertex_pool.in_use(vert);
    }

    void upload_atlas(Atlas *atlas, std::vector<vk::BufferImageCopy2> regions) {
        auto& ad = data[dataIdx];

        if (status_upload == Idle) {
            auto err = device.waitForFences(*ad.copyFence, vk::True, UINT64_MAX);
            if (err != vk::Result::eSuccess)
                return;

            device.resetFences(*ad.copyFence);
            ad.commandBuffer.reset();
            vk::CommandBufferBeginInfo begin_info{
                .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
            };
            ad.commandBuffer.begin(begin_info);
            status_upload = Init;
        }

        vk::ImageMemoryBarrier2 imageBarrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eHost,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = atlas->newly_created ? vk::ImageLayout::eUndefined : vk::ImageLayout::eShaderReadOnlyOptimal,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = *atlas->image,
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
        ad.commandBuffer.pipelineBarrier2(dep_info);

        vk::CopyBufferToImageInfo2 copy_info = {
            .srcBuffer = atlas->up_buffer,
            .dstImage = atlas->image,
            .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
            .regionCount = (uint32_t)regions.size(),
            .pRegions = regions.data(),
        };
        ad.commandBuffer.copyBufferToImage2(copy_info);

        imageBarrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = *atlas->image,
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
        ad.commandBuffer.pipelineBarrier2(dep_info);
        atlas->newly_created = false;
    }

    void prepare_draw(double play_time) {
        if (!ass_track)
            return;

        dataIdx = (dataIdx + 1) % N_DATA;
        auto& ad = data[dataIdx];
        ad.atlas_pool.recycle();
        ad.vertex_pool.recycle();

        int changed = 0;
        // Ask libass to process the track at this specific millisecond frame marker
        ASS_Image* img = ass_render_frame(ass_renderer, ass_track, play_time * 1000, &changed);
        // 3. Draw the active text lines over the frame canvas
        Atlas *atlas = nullptr;
        for (; img; img = img->next) {
//            printf("SUCCESS: libass generated image chunks! w=%d, h=%d at position x=%d, y=%d\n", img->w, img->h, img->dst_x, img->dst_y);
            if (img->w == 0 || img->h == 0)
                continue;

            uint32_t out_x, out_y;
            AtlasRegion out_uv;
            while (true) {
                if (!atlas) {
                    atlas = ad.atlas_pool.alloc(device, wnd_w, wnd_h);
                    regions.clear();
                }
                if (atlas->alloc_region(img->w, img->h, out_x, out_y, out_uv))
                    break;
                if (atlas->vertices.empty()) {
                    delete atlas;
                    return; // fatal: too large img?
                } else {
                    upload_vertices(atlas);
                    upload_atlas(atlas, regions);
                    ad.atlas_pool.in_use(atlas);
                }
                atlas = nullptr;
            }

            uint8_t *map = (uint8_t *) atlas->up_memory.mapMemory(atlas->offset, img->w * img->h);
            const uint8_t* src = img->bitmap;
            for (int y = 0; y < img->h; ++y) {
                SDL_memcpy(map + (y * img->w), src + (y * img->stride), img->w);
            }
            atlas->up_memory.unmapMemory();

            regions.push_back({
                .bufferOffset = atlas->offset,
                .imageSubresource = {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .layerCount = 1,
                },
                .imageOffset = { (int32_t)out_x, (int32_t)out_y},
                .imageExtent = { (uint32_t)img->w, (uint32_t)img->h, 1 }
            });
            atlas->offset += img->w * img->h;

            uint32_t c = img->color;
            float r = ((c >> 24) & 0xFF) / 255.0f;
            float g = ((c >> 16) & 0xFF) / 255.0f;
            float b = ((c >> 8)  & 0xFF) / 255.0f;
            float a = (255 - (c & 0xFF)) / 255.0f;

            atlas->vertices.push_back({
                2.0f * img->dst_x / wnd_w - 1.0f, 1.0f - 2.0f * img->dst_y / wnd_h,
                2.0f * img->w / wnd_w, 2.0f * img->h / wnd_h,
                out_uv,
                r, g, b, a
            });
        }
        if (atlas) {
            upload_vertices(atlas);
            upload_atlas(atlas, regions);
            ad.atlas_pool.in_use(atlas);
        }

        if (status_upload != Idle) {
            ad.commandBuffer.end();
            vk::SubmitInfo end_info = {
                .commandBufferCount = 1,
                .pCommandBuffers = &*ad.commandBuffer
            };
            queue.submit(end_info, ad.copyFence);
            status_upload = Idle;
        }
    }

    void draw(const vk::CommandBuffer commandBuffer) {
        auto& ad = data[dataIdx];
        auto list = ad.atlas_pool.get_in_use();
        if (list.empty())
            return;

        auto err = device.waitForFences(*ad.copyFence, vk::True, UINT64_MAX);
        if (err != vk::Result::eSuccess)
            return;

        auto& set = sets[frameIdx];

        for (auto data : list) {
            //Update the Descriptor Set
            vk::DescriptorImageInfo imageInfo{
                .imageView = data->imageView,
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
            };
            vk::DescriptorBufferInfo bufInfo{
                .buffer = *data->vert_buf,
                .range = (vk::DeviceSize)data->vert_size
            };
            vk::WriteDescriptorSet descriptorWrites[] = {
                {
                    .dstSet = set,
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                    .pImageInfo = &imageInfo,
                },
                {
                    .dstSet = set,
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eStorageBuffer,
                    .pBufferInfo = &bufInfo,
                },
            };
            device.updateDescriptorSets(descriptorWrites, nullptr);
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
            commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, *set, nullptr);
            commandBuffer.draw(data->vertices.size() * 6, 1, 0, 0);
        }

        frameIdx = (frameIdx + 1) % N_INFLIGHT;
    }

    void window_size_changed(Sint32 w, Sint32 h) {
        wnd_w = w;
        wnd_h = h;
        if (ass_renderer)
            ass_set_frame_size(ass_renderer, wnd_w, wnd_h); // Match your window canvas size
    }
};
