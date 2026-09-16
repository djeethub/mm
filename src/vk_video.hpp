#pragma once

#include "ffmpeg.hpp"

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

struct Vertform {
    float position[2]; // x, y (in NDC: -1.0 to 1.0)
    float size[2];     // width, height (in NDC: 0.0 to 2.0)
};

struct Uniforms
{
	float tex_size[2]; // width, height of Y plane
};

struct VkFrame {
    enum Status {
        None,
        New,
        Upload,
        Discard,
        Ready,
    };

    vk::raii::Image image = nullptr;
    vk::raii::DeviceMemory memory = nullptr;
    vk::raii::ImageView imageView = nullptr;
    vk::raii::DescriptorPool pool = nullptr;
    vk::raii::DescriptorSet set = nullptr;
    vk::raii::DeviceMemory upload_buffer_memory = nullptr;
    vk::raii::Buffer upload_buffer = nullptr;
    vk::raii::Fence copyFence = nullptr;
    vk::raii::CommandPool commandPool = nullptr;
    vk::raii::CommandBuffer commandBuffer = nullptr;

    Status status = None;
    AVFrame *frame = nullptr;
    double play_time;
};

class VkVideo {
private:
    inline static const int N_FRAMES = 2;

    vk::raii::SamplerYcbcrConversion ycbcrConversion = nullptr;
    vk::raii::DescriptorSetLayout layout = nullptr;
    vk::raii::Sampler sampler = nullptr;

    VkFrame frames[N_FRAMES];
    int frame_idx = 0;

    vk::DeviceSize upload_size;

    AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    const AVPixFmtDescriptor *fmt_desc;
    int n_planes;
    int bpp;
    vk::Format format;

    void init_frames(AVFrame *frame, const vk::raii::Device& device, uint32_t queueFamily) {
        for (auto& x : frames) {
            if (x.frame) {
                ff::frame_recycle(x.frame);
                x.frame = nullptr;
            }

            if (!*x.commandPool) {
                vk::CommandPoolCreateInfo poolInfo = {
                    .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                    .queueFamilyIndex = queueFamily
                };
                x.commandPool = device.createCommandPool(poolInfo);

                vk::CommandBufferAllocateInfo allocInfo = {
                    .commandPool = x.commandPool,
                    .level = vk::CommandBufferLevel::ePrimary,
                    .commandBufferCount = 1
                };
                x.commandBuffer = std::move(device.allocateCommandBuffers(allocInfo)[0]);

                vk::FenceCreateInfo fence_info = {
                    .flags = vk::FenceCreateFlagBits::eSignaled,
                };
                x.copyFence = device.createFence(fence_info);

                vk::DescriptorPoolSize pool_sizes[] =
                {
                    { vk::DescriptorType::eCombinedImageSampler, 3 },
                };
                vk::DescriptorPoolCreateInfo pool_info{
                    .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
                    .maxSets = 1,
                    .poolSizeCount = (uint32_t)IM_COUNTOF(pool_sizes),
                    .pPoolSizes = pool_sizes
                };
                x.pool = device.createDescriptorPool(pool_info);

                // Allocate a descriptor set from the pool
                vk::DescriptorSetAllocateInfo alloc_info{
                    .descriptorPool = x.pool, // The pool we just created
                    .descriptorSetCount = 1,
                    .pSetLayouts = &*layout // Your predefined VkDescriptorSetLayout
                };
                x.set = std::move(device.allocateDescriptorSets(alloc_info)[0]);
            } else {
                device.resetFences(*x.copyFence);
            }

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
            x.image = device.createImage(info);
            auto req = x.image.getMemoryRequirements();
            vk::MemoryAllocateInfo mem_alloc_info = {};
            mem_alloc_info.allocationSize = req.size;
            mem_alloc_info.memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
            x.memory = device.allocateMemory(mem_alloc_info);
            x.image.bindMemory(x.memory, 0);

            // Create the VkImageView with Conversion Info
            vk::SamplerYcbcrConversionInfo viewConversionInfo = {
                .conversion = ycbcrConversion
            };
            vk::ImageViewCreateInfo viewInfo{};
            viewInfo.pNext = &viewConversionInfo; // <-- Crucial!
            viewInfo.image = x.image;     // The VkImage containing your uploaded AVFrame data
            viewInfo.viewType = vk::ImageViewType::e2D;
            viewInfo.format = format;
            viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor; // Vulkan handles sub-planes internally
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            x.imageView = device.createImageView(viewInfo);

            // Create the Upload Buffer:
            upload_size = get_upload_size();
            {
                vk::BufferCreateInfo buffer_info = {
                    .size = upload_size,
                    .usage = vk::BufferUsageFlagBits::eTransferSrc,
                    .sharingMode = vk::SharingMode::eExclusive
                };
                x.upload_buffer = device.createBuffer(buffer_info);
                auto req = x.upload_buffer.getMemoryRequirements();
                vk::MemoryAllocateInfo alloc_info = {
                    .allocationSize = req.size,
                    .memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)
                };
                x.upload_buffer_memory = device.allocateMemory(alloc_info);
                x.upload_buffer.bindMemory(x.upload_buffer_memory, 0);
            }

            //Update the Descriptor Set
            vk::DescriptorImageInfo imageInfo{
                .imageView = x.imageView,
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
            };
            vk::WriteDescriptorSet descriptorWrites[] = {
                {
                    .dstSet = x.set,
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                    .pImageInfo = &imageInfo,
                },
            };
            device.updateDescriptorSets(descriptorWrites, nullptr);

            x.status = VkFrame::New;
        }
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

public:
	int width;
	int height;
    vk::raii::PipelineLayout pipelineLayout = nullptr;
    vk::raii::Pipeline pipeline = nullptr;
    vk::PhysicalDeviceMemoryProperties memProperties;

    bool check_frame(AVFrame *frame) {
        if (frame->format == pix_fmt && width == frame->width && height == frame->height)
            return true;
        return false;
    }

    bool init(AVFrame *frame, const vk::raii::Device& device, uint32_t queueFamily, vk::Format swap_format) {
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
            case AV_PIX_FMT_YUV420P:
                format = vk::Format::eG8B8R83Plane420Unorm;
                break;
            case AV_PIX_FMT_NV12:
                format = vk::Format::eG8B8R82Plane420Unorm;
                break;
            case AV_PIX_FMT_P010:
                format = vk::Format::eG10X6B10X6R10X62Plane420Unorm3Pack16;
                break;
            case AV_PIX_FMT_YUV420P10:
                format = vk::Format::eG10X6B10X6R10X63Plane420Unorm3Pack16;
                break;
            default:
                auto msg = std::format("Unsupported pix fmt: {}", av_get_pix_fmt_name(pix_fmt));
                throw std::runtime_error(msg.c_str());
        }

        auto colorspace = frame->colorspace == AVCOL_SPC_UNSPECIFIED ? AVCOL_SPC_BT709 : frame->colorspace;

        //Create the VkSamplerYcbcrConversion
        vk::SamplerYcbcrConversionCreateInfo ycbcrInfo{
            .format = format,
            .components = { vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity },
            .xChromaOffset = vk::ChromaLocation::eCositedEven,
            .yChromaOffset = vk::ChromaLocation::eCositedEven,
            .chromaFilter = vk::Filter::eLinear
        };
        switch (frame->color_range) {
            case AVCOL_RANGE_JPEG:
                ycbcrInfo.ycbcrRange = vk::SamplerYcbcrRange::eItuFull;
                break;
            default:
                ycbcrInfo.ycbcrRange = vk::SamplerYcbcrRange::eItuNarrow;
        }
        switch (frame->colorspace) {
            case AVCOL_SPC_BT709:
                ycbcrInfo.ycbcrModel = vk::SamplerYcbcrModelConversion::eYcbcr709;
                break;
            case AVCOL_SPC_BT2020_CL:
            case AVCOL_SPC_BT2020_NCL:
                ycbcrInfo.ycbcrModel = vk::SamplerYcbcrModelConversion::eYcbcr2020;
                break;
            case AVCOL_SPC_RGB:
                ycbcrInfo.ycbcrModel = vk::SamplerYcbcrModelConversion::eRgbIdentity;
                break;
            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M:
                ycbcrInfo.ycbcrModel = vk::SamplerYcbcrModelConversion::eYcbcr601;
                break;
            default:
                ycbcrInfo.ycbcrModel = vk::SamplerYcbcrModelConversion::eYcbcrIdentity;
        }
        ycbcrConversion = device.createSamplerYcbcrConversion(ycbcrInfo);

        //Create the Sampler pointing to the Conversion
        vk::SamplerYcbcrConversionInfo samplerConversionInfo = {
            .conversion = ycbcrConversion
        };
        vk::SamplerCreateInfo samplerInfo{
            .pNext = &samplerConversionInfo, // <-- Bind the conversion rules here
            .magFilter = vk::Filter::eLinear,
            .minFilter = vk::Filter::eLinear,
        // Address modes must be CLAMP_TO_EDGE for YUV samplers
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        };
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

        init_frames(frame, device, queueFamily);
        createGraphicsPipeline(device, swap_format);
        return true;
    }

	void createGraphicsPipeline(const vk::raii::Device& device, vk::Format swap_format)
	{
        auto vert_shader = load_shader(device, VERT);
        auto frag_shader = load_shader(device, NV12_FRAG);

		vk::PipelineShaderStageCreateInfo vertShaderStageInfo{.stage = vk::ShaderStageFlagBits::eVertex, .module = vert_shader, .pName = "main"};
		vk::PipelineShaderStageCreateInfo fragShaderStageInfo{.stage = vk::ShaderStageFlagBits::eFragment, .module = frag_shader, .pName = "main"};
		vk::PipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

		vk::PipelineVertexInputStateCreateInfo   vertexInputInfo{.vertexBindingDescriptionCount   = 0,
		                                                         .pVertexBindingDescriptions      = nullptr,
		                                                         .vertexAttributeDescriptionCount = 0,
		                                                         .pVertexAttributeDescriptions    = nullptr};
		vk::PipelineInputAssemblyStateCreateInfo inputAssembly{.topology = vk::PrimitiveTopology::eTriangleStrip};

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
		    .blendEnable    = vk::False,
		    .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};

		vk::PipelineColorBlendStateCreateInfo colorBlending{
		    .logicOpEnable = vk::False, .logicOp = vk::LogicOp::eCopy, .attachmentCount = 1, .pAttachments = &colorBlendAttachment};

		std::vector<vk::DynamicState>      dynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
		vk::PipelineDynamicStateCreateInfo dynamicState{.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()), .pDynamicStates = dynamicStates.data()};

        vk::PushConstantRange push_constants[]{
            {
                .stageFlags = vk::ShaderStageFlagBits::eVertex,
                .offset = 0,
                .size = sizeof(Vertform),
            },
            {
                .stageFlags = vk::ShaderStageFlagBits::eFragment,
                .offset = sizeof(Vertform),
                .size = sizeof(Uniforms),
            }
        };
		vk::PipelineLayoutCreateInfo pipelineLayoutInfo{.setLayoutCount = 1, .pSetLayouts = &*layout, .pushConstantRangeCount = 2, .pPushConstantRanges = push_constants};
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

    void upload(AVFrame *frame, const vk::raii::Device& device, const vk::raii::Queue& queue, double play_time) {
        auto next_idx = (frame_idx + 1) % N_FRAMES;
        auto& fd = frames[next_idx];
        auto err = device.waitForFences(*fd.copyFence, vk::True, 0);
        if (err != vk::Result::eSuccess)
            return;

		if (fd.frame)
			ff::frame_recycle(fd.frame);
		fd.frame = frame;

        int offset[4]{};
        // Upload to Buffer:
        uint8_t *map = (uint8_t *) fd.upload_buffer_memory.mapMemory(0, upload_size);
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
        fd.upload_buffer_memory.unmapMemory();

        // Start command buffer
        {
            device.resetFences(*fd.copyFence);
            fd.commandBuffer.reset();
            vk::CommandBufferBeginInfo begin_info{
                .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
            };
            fd.commandBuffer.begin(begin_info);
        }

        // Copy to Image:
        {
            vk::BufferMemoryBarrier bufferBarrier = {
                .srcAccessMask = vk::AccessFlagBits::eHostWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .buffer = *fd.upload_buffer,
                .size = upload_size,
            };
            vk::ImageMemoryBarrier imageBarrier = {
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = fd.status == VkFrame::New ? vk::ImageLayout::eUndefined : vk::ImageLayout::eShaderReadOnlyOptimal,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .image = *fd.image,
                .subresourceRange = {
                    .aspectMask = vk::ImageAspectFlags::BitsType::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
            };
            fd.commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, bufferBarrier, imageBarrier);

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
                .srcBuffer = fd.upload_buffer,
                .dstImage = fd.image,
                .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
                .regionCount = (uint32_t) planeCopies.size(),
                .pRegions = planeCopies.data(),
            };
            fd.commandBuffer.copyBufferToImage2(copy_info);

            imageBarrier = {
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .image = *fd.image,
                .subresourceRange = {
                    .aspectMask = vk::ImageAspectFlags::BitsType::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
            };
            fd.commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr, imageBarrier);
        }

        // End command buffer
        {
            fd.commandBuffer.end();
            vk::SubmitInfo end_info = {};
            end_info.commandBufferCount = 1;
            end_info.pCommandBuffers = &*fd.commandBuffer;
            queue.submit(end_info, fd.copyFence);
        }

        fd.status = VkFrame::Upload;
        fd.play_time = play_time;
//        queue.waitIdle();
    }

    bool check_next_frame(double play_time, const vk::Device& device) {
        auto next_idx = (frame_idx + 1) % N_FRAMES;
        auto& fd = frames[next_idx];
        if (fd.status == VkFrame::Upload) {
            if (fd.play_time <= play_time) {
                auto err = device.waitForFences(*fd.copyFence, vk::True, 0);
                if (err == vk::Result::eSuccess) {
                    fd.status = VkFrame::Ready;
                    frame_idx = next_idx;
                    return true;
                }
            }
            return false;
        } else if (fd.status == VkFrame::Discard) {
            auto err = device.waitForFences(*fd.copyFence, vk::True, 0);
            if (err == vk::Result::eSuccess) {
                return true;
            }
        }
        return true;
    }

    VkFrame& get_current_frame() {
        return frames[frame_idx];
    }

    void discard_pending() {
        auto next_idx = (frame_idx + 1) % N_FRAMES;
        auto& fd = frames[next_idx];
        if (fd.status == VkFrame::Upload) {
            fd.status = VkFrame::Discard;
        }
    }
};