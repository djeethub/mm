#pragma once


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

struct FrameData {
    vk::raii::Image image = nullptr;
    vk::raii::DeviceMemory memory = nullptr;
    vk::raii::ImageView imageView = nullptr;
    vk::raii::DescriptorSet set = nullptr;
    vk::raii::DescriptorPool pool = nullptr;
    inline static vk::raii::SamplerYcbcrConversion ycbcrConversion = nullptr;
    inline static vk::raii::DescriptorSetLayout layout = nullptr;
    inline static vk::raii::Sampler sampler = nullptr;
    inline static vk::raii::PipelineLayout pipelineLayout = nullptr;
    inline static vk::raii::Pipeline pipeline = nullptr;
    vk::raii::DeviceMemory upload_buffer_memory = nullptr;
    vk::raii::Buffer upload_buffer = nullptr;
    vk::raii::Fence copyFence = nullptr;
    vk::raii::CommandPool commandPool = nullptr;
    vk::raii::CommandBuffer commandBuffer = nullptr;

    vk::DeviceSize upload_size;
    bool newly_created = true;
    AVFrame *frame;
    double play_time;

    inline static AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    static const AVPixFmtDescriptor *fmt_desc;
    static int n_planes;
    static int bpp;
	static int width;
	static int height;
    static vk::Format format;
    inline static vk::PhysicalDeviceMemoryProperties memProperties;

    static bool init_static(AVFrame *frame, const vk::raii::Device& device) {
        if (frame->format == pix_fmt && width == frame->width && height == frame->height)
            return false;

        pix_fmt = (AVPixelFormat) frame->format;
        width = frame->width;
        height = frame->height;
        fmt_desc = av_pix_fmt_desc_get(pix_fmt);
        n_planes = av_pix_fmt_count_planes(pix_fmt);
		if (n_planes == 1) {
			bpp = 0;
			for (auto i = 0; i < fmt_desc->nb_components; i++) {
				bpp += fmt_desc->comp[i].depth;
			}
			bpp /= 8;
			if (bpp == 3)
				bpp++;
		} else {
			bpp = (fmt_desc->comp[0].depth + 7) / 8;
		}

        switch (frame->format) {
            case AV_PIX_FMT_NV12:
                format = vk::Format::eG8B8R82Plane420Unorm;
                break;
            default:
                assert(false && "Unknown pix fmt");
        }

        //Create the VkSamplerYcbcrConversion
        vk::SamplerYcbcrConversionCreateInfo ycbcrInfo{};
        ycbcrInfo.format = format;
        ycbcrInfo.ycbcrModel = vk::SamplerYcbcrModelConversion::eYcbcr709; // or BT601 depending on video
        ycbcrInfo.ycbcrRange = vk::SamplerYcbcrRange::eItuNarrow; // Video levels (16-235) or FULL (0-255)
        ycbcrInfo.components = { vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity };
        ycbcrInfo.xChromaOffset = vk::ChromaLocation::eCositedEven;
        ycbcrInfo.yChromaOffset = vk::ChromaLocation::eCositedEven;
        ycbcrInfo.chromaFilter = vk::Filter::eLinear;
        ycbcrConversion = device.createSamplerYcbcrConversion(ycbcrInfo);

        //Create the Sampler pointing to the Conversion
        vk::SamplerYcbcrConversionInfo samplerConversionInfo = {
            .conversion = ycbcrConversion
        };
        vk::SamplerCreateInfo samplerInfo{};
        samplerInfo.pNext = &samplerConversionInfo; // <-- Bind the conversion rules here
        samplerInfo.magFilter = vk::Filter::eLinear;
        samplerInfo.minFilter = vk::Filter::eLinear;
        // Address modes must be CLAMP_TO_EDGE for YUV samplers
        samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        sampler = device.createSampler(samplerInfo);

        // Define the Descriptor Set Layout with an Immutable Sampler
        vk::DescriptorSetLayoutBinding bindings[] = {
            {
                .binding = 0,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eFragment,
                .pImmutableSamplers = &*sampler // <-- Baked directly into the layout binding!
            },
        };
        vk::DescriptorSetLayoutCreateInfo layoutInfo{
            .bindingCount = 1,
            .pBindings = bindings,
        };
        layout = device.createDescriptorSetLayout(layoutInfo);

        return true;
    }

    int get_upload_size() {
        if (fmt_desc->flags & AV_PIX_FMT_FLAG_RGB) {
            return width * height * bpp;
        } else {
            return (width * height * bpp * (fmt_desc->flags & AV_PIX_FMT_FLAG_ALPHA ? 2 : 1)) + ((width >> fmt_desc->log2_chroma_w) * (height >> fmt_desc->log2_chroma_h) * bpp * 2);
        }
    }

    uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) {
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }

        throw std::runtime_error("failed to find suitable memory type!");
    }   

    void init(AVFrame *frame, const vk::raii::Device& device, uint32_t queueFamily) {
        vk::CommandPoolCreateInfo poolInfo = {
                .queueFamilyIndex = queueFamily
        };
        commandPool = device.createCommandPool(poolInfo);

        vk::CommandBufferAllocateInfo allocInfo = {
            .commandPool = commandPool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1
        };
        commandBuffer = std::move(device.allocateCommandBuffers(allocInfo)[0]);

        vk::FenceCreateInfo fence_info = {
//            .flags = vk::FenceCreateFlagBits::eSignaled,
        };
        copyFence = device.createFence(fence_info);

        vk::DescriptorPoolSize pool_sizes[] =
        {
            { vk::DescriptorType::eCombinedImageSampler, 1 },
        };
        vk::DescriptorPoolCreateInfo pool_info{};
//        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = (uint32_t)IM_COUNTOF(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        pool = device.createDescriptorPool(pool_info);

        // Allocate a descriptor set from the pool
        vk::DescriptorSetAllocateInfo alloc_info{};
        alloc_info.descriptorPool = pool; // The pool we just created
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &*layout; // Your predefined VkDescriptorSetLayout
        set = std::move(device.allocateDescriptorSets(alloc_info)[0]);

        // Create the Image
        vk::ImageCreateInfo info = {};
        info.imageType = vk::ImageType::e2D;
        info.format = format;
        info.extent.width = frame->width;
        info.extent.height = frame->height;
        info.extent.depth = 1;
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = vk::SampleCountFlagBits::e1;
        info.tiling = vk::ImageTiling::eOptimal;
        info.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
        info.sharingMode = vk::SharingMode::eExclusive;
        info.initialLayout = vk::ImageLayout::eUndefined;
        image = device.createImage(info);
        auto req = image.getMemoryRequirements();
        vk::MemoryAllocateInfo mem_alloc_info = {};
        mem_alloc_info.allocationSize = req.size;
        mem_alloc_info.memoryTypeIndex = req.memoryTypeBits;
        memory = device.allocateMemory(mem_alloc_info);
        image.bindMemory(memory, 0);

        // Create the VkImageView with Conversion Info
        vk::SamplerYcbcrConversionInfo viewConversionInfo = {
            .conversion = ycbcrConversion
        };
        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.pNext = &viewConversionInfo; // <-- Crucial!
        viewInfo.image = image;     // The VkImage containing your uploaded AVFrame data
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor; // Vulkan handles sub-planes internally
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        imageView = device.createImageView(viewInfo);

        // Create the Upload Buffer:
        upload_size = get_upload_size();
        {
            vk::BufferCreateInfo buffer_info = {
                .size = upload_size,
                .usage = vk::BufferUsageFlagBits::eTransferSrc,
                .sharingMode = vk::SharingMode::eExclusive
            };
            upload_buffer = device.createBuffer(buffer_info);
            auto req = upload_buffer.getMemoryRequirements();
            vk::MemoryAllocateInfo alloc_info = {
                .allocationSize = req.size,
                .memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)
            };
            upload_buffer_memory = device.allocateMemory(alloc_info);
            upload_buffer.bindMemory(upload_buffer_memory, 0);
        }

        //Update the Descriptor Set
        vk::DescriptorImageInfo imageInfo{
            .imageView = imageView,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
        };
        vk::WriteDescriptorSet descriptorWrites[] = {
            {
                .dstSet = set,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .pImageInfo = &imageInfo,
            },
        };
        device.updateDescriptorSets(descriptorWrites, nullptr);

        newly_created = true;
    }

    static vk::raii::ShaderModule load_shader(const vk::raii::Device& device, ShaderType type) {
        vk::ShaderModuleCreateInfo shader_info{};

        switch (type) {
		case VERT:
			shader_info.pCode = (uint32_t *) vert_vert;
			shader_info.codeSize = vert_vert_len;
			break;
		case NV12_FRAG:
			shader_info.pCode = (uint32_t *) nv12_frag;
			shader_info.codeSize = nv12_frag_len;
			break;
        }

        return device.createShaderModule(shader_info);
    }

    static void init_pipeline(const vk::raii::Device& device, const vk::Format swap_format) {
        auto vert_shader = load_shader(device, VERT);
        auto frag_shader = load_shader(device, NV12_FRAG);

		vk::PipelineShaderStageCreateInfo vertShaderStageInfo{.stage = vk::ShaderStageFlagBits::eVertex, .module = vert_shader};
		vk::PipelineShaderStageCreateInfo fragShaderStageInfo{.stage = vk::ShaderStageFlagBits::eFragment, .module = frag_shader};
		vk::PipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

		vk::PipelineInputAssemblyStateCreateInfo inputAssembly{.topology = vk::PrimitiveTopology::eTriangleStrip};
/*
		vk::PipelineViewportStateCreateInfo      viewportState{.viewportCount = 1, .scissorCount = 1};

		vk::PipelineRasterizationStateCreateInfo rasterizer{.depthClampEnable        = vk::False,
		                                                    .rasterizerDiscardEnable = vk::False,
		                                                    .polygonMode             = vk::PolygonMode::eFill,
		                                                    .cullMode                = vk::CullModeFlagBits::eBack,
		                                                    .frontFace               = vk::FrontFace::eCounterClockwise,
		                                                    .depthBiasEnable         = vk::False,
		                                                    .lineWidth               = 1.0f};

		vk::PipelineMultisampleStateCreateInfo multisampling{.rasterizationSamples = vk::SampleCountFlagBits::e1, .sampleShadingEnable = vk::False};
*/
		vk::PipelineColorBlendAttachmentState colorBlendAttachment{
		    .blendEnable    = vk::False,
		    .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};

		vk::PipelineColorBlendStateCreateInfo colorBlending{
		    .logicOpEnable = vk::False, .logicOp = vk::LogicOp::eCopy, .attachmentCount = 1, .pAttachments = &colorBlendAttachment};

//		std::vector<vk::DynamicState>      dynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
//		vk::PipelineDynamicStateCreateInfo dynamicState{.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()), .pDynamicStates = dynamicStates.data()};

        vk::PushConstantRange push_constants[]{
            {
                .stageFlags = vk::ShaderStageFlagBits::eVertex,
                .offset = 0,
                .size = sizeof(Vertex),
            },
            {
                .stageFlags = vk::ShaderStageFlagBits::eFragment,
                .offset = sizeof(Vertex),
                .size = sizeof(Uniforms),
            }
        };
		vk::PipelineLayoutCreateInfo pipelineLayoutInfo{.setLayoutCount = 1, .pSetLayouts = &*layout, .pushConstantRangeCount = 2, .pPushConstantRanges = push_constants};
		pipelineLayout = vk::raii::PipelineLayout(device, pipelineLayoutInfo);

		vk::StructureChain<vk::GraphicsPipelineCreateInfo, vk::PipelineRenderingCreateInfo> pipelineCreateInfoChain = {
		    {
                .stageCount          = 2,
                .pStages             = shaderStages,
    //		     .pVertexInputState   = &vertexInputInfo,
                .pInputAssemblyState = &inputAssembly,
//                .pViewportState      = &viewportState,
//                .pRasterizationState = &rasterizer,
//                .pMultisampleState   = &multisampling,
                .pColorBlendState    = &colorBlending,
//                .pDynamicState       = &dynamicState,
                .layout              = pipelineLayout,
            },
		    {.colorAttachmentCount = 1, .pColorAttachmentFormats = &swap_format}
        };

		pipeline = vk::raii::Pipeline(device, nullptr, pipelineCreateInfoChain.get<vk::GraphicsPipelineCreateInfo>());
	}

    void upload(AVFrame *frame, const vk::raii::Queue& queue, const vk::raii::Device& device) {
		if (this->frame)
			ff::frame_recycle(this->frame);
		this->frame = frame;

        int offset[4]{};
        // Upload to Buffer:
        uint8_t *map = (uint8_t *) upload_buffer_memory.mapMemory(0, upload_size);
        uint8_t *src = frame->data[0];
        auto map_save = map;
        auto bytes_per_line = width * bpp;
        for (int y = 0; y < height; y++) {
            memcpy(map, src, bytes_per_line);
            map += bytes_per_line;
            src += frame->linesize[0];
        }
        if (n_planes > 1) {
            bytes_per_line = (width >> fmt_desc->log2_chroma_w) * bpp * (n_planes == 2 ? 2 : 1);
            auto new_height = height >> fmt_desc->log2_chroma_h;
            for (auto idx = 1; idx < n_planes && idx < 3; idx++) {
                src = frame->data[idx];
                offset[idx] = map - map_save;
                for (int y = 0; y < new_height; y++) {
                    memcpy(map, src, bytes_per_line);
                    map += bytes_per_line;
                    src += frame->linesize[idx];
                }
            }

            if (n_planes > 3) {
                assert(false);
//                    bytes_per_line = width * bpp;
            }
        }

/*        VkMappedMemoryRange range[1] = {};
        range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range[0].memory = upload_buffer_memory;
        range[0].size = upload_size;
        err = vkFlushMappedMemoryRanges(v->Device, 1, range);
        check_vk_result(err);*/
        upload_buffer_memory.unmapMemory();

        // Start command buffer
        {
            commandPool.reset();
            vk::CommandBufferBeginInfo begin_info = {};
            begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
            commandBuffer.begin(begin_info);
        }

        // Copy to Image:
        {
            vk::BufferMemoryBarrier bufferBarrier = {
                .srcAccessMask = vk::AccessFlagBits::eHostWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .buffer = *upload_buffer,
                .size = upload_size,
            };
            vk::ImageMemoryBarrier imageBarrier = {
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = newly_created ? vk::ImageLayout::eUndefined : vk::ImageLayout::eShaderReadOnlyOptimal,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .image = *image,
                .subresourceRange = {
                    .aspectMask = vk::ImageAspectFlags::BitsType::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
            };
            commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, bufferBarrier, imageBarrier);

            std::vector<vk::BufferImageCopy2> planeCopies(n_planes);
            if (n_planes > 1) {
                for (auto i = 0; i < n_planes; i++) {
                    planeCopies[i] = {
                        .bufferOffset = (vk::DeviceSize) offset[i],
                        .imageSubresource = {
                            .aspectMask = i == 0 ? vk::ImageAspectFlagBits::ePlane0 : (i == 1 ? vk::ImageAspectFlagBits::ePlane1 : vk::ImageAspectFlagBits::ePlane2),
                            .layerCount = 1,
                        },
                        .imageExtent = i == 0 ? vk::Extent3D{ (uint32_t)width, (uint32_t)height, 1 } : vk::Extent3D{ (uint32_t)width >> fmt_desc->log2_chroma_w, (uint32_t)height >> fmt_desc->log2_chroma_h, 1 }
                    };
                }
            } else {
                planeCopies[0] = {
                    .imageSubresource = {
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .layerCount = 1,
                    },
                    .imageExtent = { (uint32_t)width, (uint32_t)height, 1 }
                };
            }

            vk::CopyBufferToImageInfo2 copy_info = {
                .srcBuffer = upload_buffer,
                .dstImage = image,
                .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
                .regionCount = (uint32_t) planeCopies.size(),
                .pRegions = planeCopies.data(),
            };
            commandBuffer.copyBufferToImage2(copy_info);

            imageBarrier = {
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .image = *image,
                .subresourceRange = {
                    .aspectMask = vk::ImageAspectFlags::BitsType::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
            };
            commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr, imageBarrier);
        }

        // End command buffer
        {
            commandBuffer.end();
            vk::SubmitInfo end_info = {};
            end_info.commandBufferCount = 1;
            end_info.pCommandBuffers = &*commandBuffer;
            queue.submit(end_info, copyFence);
        }

        newly_created = false;
//        queue.waitIdle();
    }

    void render(const vk::CommandBuffer cmd) {

    }
};
