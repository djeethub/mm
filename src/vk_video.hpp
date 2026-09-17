#pragma once

#include <unistd.h>

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

struct AvFrameData {
    AVFrame *frame = nullptr;
    AVFrame *mapped = nullptr;
    double play_time;

    // 1. Non-copyable (or implement deep copy if you really need it)
    AvFrameData() {}
    AvFrameData(const AvFrameData&)            = delete;
    AvFrameData& operator=(const AvFrameData&) = delete;

    // 2. Move constructor / move assignment that *transfer* ownership
    AvFrameData(AvFrameData&& other) noexcept
        : frame(std::exchange(other.frame, nullptr)),
          mapped(std::exchange(other.mapped, nullptr)),
          play_time(other.play_time)
    {}

    AvFrameData& operator=(AvFrameData&& other) noexcept {
        if (this != &other) {
            // release what we currently own
            ff::frame_recycle(mapped);
            ff::frame_recycle(frame);

            // steal
            frame     = std::exchange(other.frame, nullptr);
            mapped    = std::exchange(other.mapped, nullptr);
            play_time = other.play_time;
        }
        return *this;
    }

    ~AvFrameData() {
        ff::frame_recycle(mapped);
        ff::frame_recycle(frame);
    }
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
    vk::raii::Buffer upload_buffer = nullptr;
    vk::raii::DeviceMemory upload_buffer_memory = nullptr;
    vk::raii::Fence copyFence = nullptr;
    vk::raii::CommandPool commandPool = nullptr;
    vk::raii::CommandBuffer commandBuffer = nullptr;

    AvFrameData frame_data;
    Status status = None;
};

class VkVideo {
private:
    inline static const int N_FRAMES = 4;

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

    void init_frames(const AVFrame *frame, const vk::raii::Device& device) {
        for (auto& x : frames) {
            if (!*x.commandPool) {
                vk::CommandPoolCreateInfo poolInfo = {
                    .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                    .queueFamilyIndex = queueIndex
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
                vk::FenceCreateInfo fence_info = {
                    .flags = vk::FenceCreateFlagBits::eSignaled,
                };
                x.copyFence = device.createFence(fence_info);
            }

            // Create the Image
            if (frame->hw_frames_ctx) {
            } else {
                vk::ImageCreateInfo info = {
                    .imageType = vk::ImageType::e2D,
                    .format = format,
                    .extent = { .width = (uint32_t) frame->width, .height = (uint32_t) frame->height, .depth = 1 },
                    .mipLevels = 1,
                    .arrayLayers = 1,
                    .samples = vk::SampleCountFlagBits::e1,
                    .tiling = vk::ImageTiling::eOptimal,
                    .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                    .sharingMode = vk::SharingMode::eExclusive,
                    .initialLayout = vk::ImageLayout::eUndefined,
                };
                x.image = device.createImage(info);
                auto req = x.image.getMemoryRequirements();
                vk::MemoryAllocateInfo mem_alloc_info = {};
                mem_alloc_info.allocationSize = req.size;
                mem_alloc_info.memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
                x.memory = device.allocateMemory(mem_alloc_info);
                x.image.bindMemory(x.memory, 0);

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

                // Create the VkImageView with Conversion Info
                vk::SamplerYcbcrConversionInfo viewConversionInfo = {
                    .conversion = ycbcrConversion
                };
                vk::ImageViewCreateInfo viewInfo{
                    .pNext = &viewConversionInfo, // <-- Crucial!
                    .image = x.image,     // The VkImage containing your uploaded AVFrame data
                    .viewType = vk::ImageViewType::e2D,
                    .format = format,
                    .subresourceRange = {
                        .aspectMask = vk::ImageAspectFlagBits::eColor, // Vulkan handles sub-planes internally
                        .levelCount = 1,
                        .layerCount = 1
                    }
                };
                x.imageView = device.createImageView(viewInfo);

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
            }

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

    bool check_frame(const AVFrame *frame) {
        AVPixelFormat format;
        if (frame->hw_frames_ctx) {
            // Access the frame context structural layer
            AVHWFramesContext *hwfc = (AVHWFramesContext*)frame->hw_frames_ctx->data;
            format = hwfc->sw_format;
        } else
            format = (AVPixelFormat) frame->format;

        if (format == pix_fmt && width == frame->width && height == frame->height)
            return true;
        return false;
    }

    bool init(const AVFrame *frame, const vk::raii::Device& device) {
        if (frame->hw_frames_ctx) {
            // Access the frame context structural layer
            AVHWFramesContext *hwfc = (AVHWFramesContext*)frame->hw_frames_ctx->data;
            pix_fmt = hwfc->sw_format;
        } else
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

        switch (pix_fmt) {
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

        init_frames(frame, device);
        createGraphicsPipeline(device);
        return true;
    }

	void createGraphicsPipeline(const vk::raii::Device& device)
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
		    {.colorAttachmentCount = 1, .pColorAttachmentFormats = &swapChainSurfaceFormat.format}
        };

		pipeline = vk::raii::Pipeline(device, nullptr, pipelineCreateInfoChain.get<vk::GraphicsPipelineCreateInfo>());
	}

    void upmap(VkFrame& vf, const vk::raii::Device& device, const vk::raii::Queue& queue) {
        auto frame = vf.frame_data.frame;

        AVFrame *drm_frame = ff::frame_alloc();
        // Map VAAPI surface to DRM PRIME
        drm_frame->format = AV_PIX_FMT_DRM_PRIME;
        int err = av_hwframe_map(drm_frame, frame, AV_HWFRAME_MAP_READ);
        if (err < 0) {
            ff::frame_recycle(drm_frame);
            std::println("av_hwframe_map failed.");
            return;
        }
        vf.frame_data.mapped = drm_frame;

        vf.imageView = nullptr;

        // drm_frame->data[0] now contains a pointer to an AVDRMFrameDescriptor struct
        AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)drm_frame->data[0];

        // Example assumes an NV12 frame (1 Layer, 2 Planes: Y and UV)
        AVDRMObjectDescriptor *obj = &desc->objects[0]; // The underlying memory chunk
//        AVDRMLayerDescriptor *layer = &desc->layers[0];

        int dma_buf_fd = obj->fd;
        uint64_t drm_modifier = obj->format_modifier;

        vk::SubresourceLayout plane_layouts[desc->nb_layers]{};
        for (auto i = 0; i < desc->nb_layers; i++) {
            plane_layouts[i].rowPitch = desc->layers[i].planes[0].pitch;
            plane_layouts[i].offset = desc->layers[i].planes[0].offset;
        }
        vk::ImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
            .drmFormatModifier = drm_modifier,
            .drmFormatModifierPlaneCount = (uint32_t) desc->nb_layers,
            .pPlaneLayouts = plane_layouts
        };
        // 2. Declare external memory capabilities
        vk::ExternalMemoryImageCreateInfo external_memory_img_info = {
            .pNext = &modifier_info,
            .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT
        };
        // 3. Create the Image
        vk::ImageCreateInfo img_info = {
            .pNext = &external_memory_img_info,
            .imageType = vk::ImageType::e2D,
            .format = format, // NV12 matching Vulkan layout
            .extent = { .width = (uint32_t) frame->width, .height = (uint32_t) frame->height, .depth = 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eDrmFormatModifierEXT, // Required for DRM modifiers
            .usage = vk::ImageUsageFlagBits::eSampled,
            .sharingMode = vk::SharingMode::eExclusive
        };
        vf.image = device.createImage(img_info);

        // Dup the file descriptor because Vulkan takes ownership and closes it upon import
        int imported_fd = dup(dma_buf_fd);

        vk::MemoryDedicatedAllocateInfo dedicatedAllocInfo{
            .image = vf.image
        };
        vk::ImportMemoryFdInfoKHR import_fd_info = {
            .pNext = &dedicatedAllocInfo,
            .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
            .fd = imported_fd
        };
        // Query memory requirements for your image
        auto req = vf.image.getMemoryRequirements();
        vk::MemoryAllocateInfo alloc_info = {
            .pNext = &import_fd_info,
            .allocationSize = req.size,
            .memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal),
        };
        vf.memory = device.allocateMemory(alloc_info);
        vf.image.bindMemory(vf.memory, 0);

        // Create the VkImageView with Conversion Info
        vk::SamplerYcbcrConversionInfo viewConversionInfo = {
            .conversion = ycbcrConversion
        };
        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.pNext = &viewConversionInfo; // <-- Crucial!
        viewInfo.image = vf.image;     // The VkImage containing your uploaded AVFrame data
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor; // Vulkan handles sub-planes internally
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        vf.imageView = device.createImageView(viewInfo);

        //Update the Descriptor Set
        vk::DescriptorImageInfo imageInfo{
            .imageView = vf.imageView,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
        };
        vk::WriteDescriptorSet descriptorWrites[] = {
            {
                .dstSet = vf.set,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .pImageInfo = &imageInfo,
            },
        };
        device.updateDescriptorSets(descriptorWrites, nullptr);
        
        vf.status = VkFrame::Upload;
    }

    void upload(AvFrameData frame_data, const vk::raii::Device& device, const vk::raii::Queue& queue) {
        auto next_idx = (frame_idx + 1) % N_FRAMES;
        auto& vf = frames[next_idx];
        auto err = device.waitForFences(*vf.copyFence, vk::True, UINT64_MAX);
        if (err != vk::Result::eSuccess)
            return;

        vf.frame_data = std::move(frame_data);
        AVFrame *frame = vf.frame_data.frame;
        if (frame->hw_frames_ctx) {
            upmap(vf, device, queue);
            return;
        }

        int offset[4]{};
        // Upload to Buffer:
        uint8_t *map = (uint8_t *) vf.upload_buffer_memory.mapMemory(0, upload_size);
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
        vf.upload_buffer_memory.unmapMemory();

        // Start command buffer
        {
            device.resetFences(*vf.copyFence);
            vf.commandPool.reset();
            vk::CommandBufferBeginInfo begin_info{
                .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
            };
            vf.commandBuffer.begin(begin_info);
        }

        // Copy to Image:
        {
            vk::ImageMemoryBarrier2 imageBarrier = {
                .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eHost,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vf.status == VkFrame::New ? vk::ImageLayout::eUndefined : vk::ImageLayout::eShaderReadOnlyOptimal,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .image = *vf.image,
                .subresourceRange = {
                    .aspectMask = vk::ImageAspectFlags::BitsType::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
            };
            vk::DependencyInfo dep_info{
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &imageBarrier
            };
            vf.commandBuffer.pipelineBarrier2(dep_info);

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
                .srcBuffer = vf.upload_buffer,
                .dstImage = vf.image,
                .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
                .regionCount = (uint32_t) planeCopies.size(),
                .pRegions = planeCopies.data(),
            };
            vf.commandBuffer.copyBufferToImage2(copy_info);

            imageBarrier = {
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .image = *vf.image,
                .subresourceRange = {
                    .aspectMask = vk::ImageAspectFlags::BitsType::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
            };
            dep_info = {
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &imageBarrier
            };
            vf.commandBuffer.pipelineBarrier2(dep_info);
        }

        // End command buffer
        {
            vf.commandBuffer.end();
            vk::SubmitInfo end_info = {};
            end_info.commandBufferCount = 1;
            end_info.pCommandBuffers = &*vf.commandBuffer;
            queue.submit(end_info, vf.copyFence);
        }

        vf.status = VkFrame::Upload;
//        queue.waitIdle();
    }

    bool check_next_frame(double play_time, const vk::Device& device) {
        auto next_idx = (frame_idx + 1) % N_FRAMES;
        auto& vf = frames[next_idx];
        if (vf.status == VkFrame::Upload) {
            if (vf.frame_data.play_time <= play_time) {
                auto err = device.waitForFences(*vf.copyFence, vk::True, 0);
                if (err == vk::Result::eSuccess) {
                    vf.status = VkFrame::Ready;
                    frame_idx = next_idx;
                    return true;
                }
            }
            return false;
        } else if (vf.status == VkFrame::Discard) {
            auto err = device.waitForFences(*vf.copyFence, vk::True, 0);
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
        auto& vf = frames[next_idx];
        if (vf.status == VkFrame::Upload) {
            vf.status = VkFrame::Discard;
        }
    }
};