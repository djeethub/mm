#pragma once

#include <unistd.h>

#include "ffmpeg.hpp"

#include "vert.vert.h"
#include "nv12.frag.h"
#include "rgb.frag.h"
#include "gray.frag.h"
#include "yuyv.frag.h"

#define N_INFLIGHT_VIDEO    4

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
    vk::raii::Buffer upload_buffer = nullptr;
    vk::raii::DeviceMemory upload_buffer_memory = nullptr;
    vk::raii::Fence copyFence = nullptr;
    vk::DescriptorSet set = nullptr;
    vk::CommandBuffer commandBuffer = nullptr;

    AvFrameData frame_data;
    Status status = None;
};

class VkVideo {
private:
    vk::raii::SamplerYcbcrConversion ycbcrConversion = nullptr;
    vk::raii::Sampler sampler = nullptr;
    vk::raii::DescriptorSetLayout layout = nullptr;
    vk::raii::DescriptorPool pool = nullptr;
    vk::raii::CommandPool commandPool = nullptr;

    VkFrame frames[N_INFLIGHT_VIDEO];
    int frame_idx = 0;

    vk::Format format;
    vk::DeviceSize upload_size;
    const AVPixFmtDescriptor *fmt_desc;
    int n_planes;
    int bpp;
    bool native = true;
    SwsContext *sws_ctx = nullptr;

    void init_frame(int idx, const AVFrame *frame, const vk::raii::Device& device) {
        auto& x = frames[idx];

        if (frame->hw_frames_ctx) {
        } else {
            vk::ImageCreateInfo info{
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
            vk::MemoryAllocateInfo mem_alloc_info{
                .allocationSize = req.size,
                .memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal)
            };
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

            vk::ImageViewCreateInfo viewInfo{
                .image = x.image,     // The VkImage containing your uploaded AVFrame data
                .viewType = vk::ImageViewType::e2D,
                .format = format,
                .subresourceRange = {
                    .aspectMask = vk::ImageAspectFlagBits::eColor, // Vulkan handles sub-planes internally
                    .levelCount = 1,
                    .layerCount = 1
                },
            };
            vk::DescriptorImageInfo imageInfo{
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
            };
            vk::SamplerYcbcrConversionInfo viewConversionInfo{
            };
            if (*ycbcrConversion) {
                viewConversionInfo.conversion = ycbcrConversion;
                viewInfo.pNext = &viewConversionInfo; // <-- Crucial!
            } else {
                imageInfo.sampler = sampler;
            }
            x.imageView = device.createImageView(viewInfo);
            imageInfo.imageView = x.imageView;

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
        case RGB_FRAG:
			shader_info.pCode = (uint32_t *) rgb_frag;
			shader_info.codeSize = rgb_frag_len;
            break;
        case YUYV_FRAG:
			shader_info.pCode = (uint32_t *) yuyv_frag;
			shader_info.codeSize = yuyv_frag_len;
            break;
        case GRAY_FRAG:
			shader_info.pCode = (uint32_t *) gray_frag;
			shader_info.codeSize = gray_frag_len;
            break;
        }

        return device.createShaderModule(shader_info);
    }

public:
    AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
	int width;
	int height;
    vk::raii::PipelineLayout pipelineLayout = nullptr;
    vk::raii::Pipeline pipeline = nullptr;

    void init_once(const vk::raii::Device& device) {
        vk::CommandPoolCreateInfo poolInfo = {
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
            .queueFamilyIndex = queueIndex
        };
        commandPool = device.createCommandPool(poolInfo);

        vk::CommandBufferAllocateInfo allocInfo = {
            .commandPool = commandPool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = N_INFLIGHT_VIDEO
        };
        auto commandBuffers = (*device).allocateCommandBuffers(allocInfo);

        vk::DescriptorPoolSize pool_sizes[] =
        {
            { vk::DescriptorType::eCombinedImageSampler, N_INFLIGHT_VIDEO * 4 },
        };
        vk::DescriptorPoolCreateInfo pool_info{
//            .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
            .maxSets = N_INFLIGHT_VIDEO,
            .poolSizeCount = (uint32_t)IM_COUNTOF(pool_sizes),
            .pPoolSizes = pool_sizes
        };
        pool = device.createDescriptorPool(pool_info);

        vk::FenceCreateInfo fence_info = {
            .flags = vk::FenceCreateFlagBits::eSignaled,
        };

        for (auto i = 0; i < N_INFLIGHT_VIDEO; i++) {
            frames[i].commandBuffer = commandBuffers[i];
            frames[i].copyFence = device.createFence(fence_info);
        }
    }

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

    bool is_supported(vk::Format format, const vk::raii::PhysicalDevice& physicalDevice) {
        try {
            vk::PhysicalDeviceImageFormatInfo2 format_info{
                .format = format,
                .type = vk::ImageType::e2D,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eSampled,
//                .flags = vk::ImageCreateFlagBits::eMutableFormat,
            };
            auto imageProps = physicalDevice.getImageFormatProperties2(format_info);
        } catch (const vk::FormatNotSupportedError& err) {
            return false;
        }
        return true;
    }

    bool init(const AVFrame *frame, const vk::raii::PhysicalDevice& physicalDevice, vk::raii::Device& device) {
        if (frame->hw_frames_ctx) {
            // Access the frame context structural layer
            AVHWFramesContext *hwfc = (AVHWFramesContext*)frame->hw_frames_ctx->data;
            pix_fmt = hwfc->sw_format;
        } else {
            pix_fmt = (AVPixelFormat) frame->format;
        }
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
//			if (bpp == 3)
//				bpp++;
		} else {
			bpp = (fmt_desc->comp[0].depth + 7) / 8;
		}

        native = true;
        switch (pix_fmt) {
            case AV_PIX_FMT_NV12:
                format = vk::Format::eG8B8R82Plane420Unorm;
                break;
            case AV_PIX_FMT_P010:
                format = vk::Format::eG10X6B10X6R10X62Plane420Unorm3Pack16;
                break;
            case AV_PIX_FMT_YUV420P10:
                format = vk::Format::eG10X6B10X6R10X63Plane420Unorm3Pack16;
                break;
            case AV_PIX_FMT_RGB24:
                format = vk::Format::eR8G8B8Unorm;
                break;
            case AV_PIX_FMT_BGR24:
                format = vk::Format::eB8G8R8Unorm;
                break;
            case AV_PIX_FMT_YUV420P:
            case AV_PIX_FMT_YUVJ420P:
                format = vk::Format::eG8B8R83Plane420Unorm;
                break;
            case AV_PIX_FMT_YUV422P:
            case AV_PIX_FMT_YUVJ422P:
                format = vk::Format::eG8B8R83Plane422Unorm;
                break;
            case AV_PIX_FMT_YUV444P:
            case AV_PIX_FMT_YUVJ444P:
                format = vk::Format::eG8B8R83Plane444Unorm;
                break;
            case AV_PIX_FMT_YUYV422:
                format = vk::Format::eG8B8G8R8422Unorm;
                break;
            case AV_PIX_FMT_GRAY8:
                format = vk::Format::eR8Unorm;
                break;
            case AV_PIX_FMT_RGBA:
                format = vk::Format::eR8G8B8A8Unorm;
                break;
            case AV_PIX_FMT_BGRA:
                format = vk::Format::eB8G8R8A8Unorm;
                break;
//            case AV_PIX_FMT_YUVA420P:
//            case AV_PIX_FMT_PAL8:
//            case AV_PIX_FMT_YA8:
            default:
                if (frame->hw_frames_ctx) {
                    auto msg = std::format("Unsupported pix fmt: {}", av_get_pix_fmt_name(pix_fmt));
                    throw std::runtime_error(msg.c_str());
                }
                native = false;
        }

        if (native) {
            native = is_supported(format, physicalDevice);
        }
        if (!native) {
            if (frame->hw_frames_ctx) { // AV_PIX_FMT_YUYV422
                format = vk::Format::eR8G8B8A8Unorm;
                n_planes = 1;
                bpp = 4;
            } else {
                AVPixelFormat new_pix_fmt;
                switch (pix_fmt) {
                case AV_PIX_FMT_BGR24:
                case AV_PIX_FMT_PAL8:
                case AV_PIX_FMT_RGB32:
                    new_pix_fmt = AV_PIX_FMT_BGRA;
                    format = vk::Format::eB8G8R8A8Unorm;
                    n_planes = 1;
                    bpp = 4;
                    break;
                case AV_PIX_FMT_YUV420P10:
                    new_pix_fmt = AV_PIX_FMT_P010;
                    format = vk::Format::eG10X6B10X6R10X62Plane420Unorm3Pack16;
                    n_planes = 2;
                    bpp = 2;
                    break;
                case AV_PIX_FMT_RGB24:
                case AV_PIX_FMT_BGR32:
                default:
                    new_pix_fmt = AV_PIX_FMT_RGBA;
                    format = vk::Format::eR8G8B8A8Unorm;
                    n_planes = 1;
                    bpp = 4;
                }
                if (!is_supported(format, physicalDevice)) {
                    new_pix_fmt = AV_PIX_FMT_RGBA;
                    format = vk::Format::eR8G8B8A8Unorm;
                    n_planes = 1;
                    bpp = 4;
                }
                setup_sws_context(pix_fmt, new_pix_fmt);
            }
        }

        commandPool.reset();
        pool.reset();

        vk::FormatProperties2 props = physicalDevice.getFormatProperties2(format);
        auto features = props.formatProperties.optimalTilingFeatures;
        bool canUseYcbcr = (features & vk::FormatFeatureFlagBits::eSampledImageYcbcrConversionLinearFilter) &&
                        ((features & vk::FormatFeatureFlagBits::eMidpointChromaSamples) || 
                            (features & vk::FormatFeatureFlagBits::eCositedChromaSamples));

        vk::SamplerCreateInfo samplerInfo{
            .magFilter = vk::Filter::eLinear,
            .minFilter = vk::Filter::eLinear,
        // Address modes must be CLAMP_TO_EDGE for YUV samplers
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        };
        vk::SamplerYcbcrConversionInfo samplerConversionInfo = {
        };
        vk::DescriptorSetLayoutBinding bindings[] = {
            {
                .binding = 0,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eFragment,
            },
        };
        if (canUseYcbcr) {
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
                case AVCOL_SPC_UNSPECIFIED:
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
                case AVCOL_SPC_SMPTE240M:
                default:
                    ycbcrInfo.ycbcrModel = vk::SamplerYcbcrModelConversion::eYcbcr601;
            }
            ycbcrConversion = device.createSamplerYcbcrConversion(ycbcrInfo);
            //Create the Sampler pointing to the Conversion
            samplerConversionInfo.conversion = ycbcrConversion;
            samplerInfo.pNext = &samplerConversionInfo;
            bindings[0].pImmutableSamplers = &*sampler; // <-- Baked directly into the layout binding!
        } else {
            ycbcrConversion = nullptr;
        }

        sampler = device.createSampler(samplerInfo);

        vk::DescriptorSetLayoutCreateInfo layoutInfo{
            .bindingCount = 1,
            .pBindings = bindings,
        };
        layout = device.createDescriptorSetLayout(layoutInfo);

        std::vector<vk::DescriptorSetLayout> layouts(N_INFLIGHT_VIDEO, layout);
        // Allocate a descriptor set from the pool
        vk::DescriptorSetAllocateInfo alloc_info{
            .descriptorPool = pool, // The pool we just created
            .descriptorSetCount = N_INFLIGHT_VIDEO,
            .pSetLayouts = layouts.data() // Your predefined VkDescriptorSetLayout
        };
        auto sets = (*device).allocateDescriptorSets(alloc_info);

        for (auto i = 0; i < N_INFLIGHT_VIDEO; i++) {
            frames[i].set = sets[i];
            init_frame(i, frame, device);
        }

        createGraphicsPipeline(device);
        return true;
    }

	void createGraphicsPipeline(const vk::raii::Device& device)
	{
        auto vert_shader = load_shader(device, VERT);
        ShaderType shader_type;
        switch (format) {
        case vk::Format::eR8G8B8A8Unorm:
            if (pix_fmt == AV_PIX_FMT_YUYV422)
                shader_type = YUYV_FRAG;
            else
                shader_type = RGB_FRAG;
            break;
        case vk::Format::eR8Unorm:
            shader_type = GRAY_FRAG;
            break;
        default:
            shader_type = NV12_FRAG;
        }
        auto frag_shader = load_shader(device, shader_type);

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

    void shutdown() {
		sws_free_context(&sws_ctx);
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
        uint32_t width = frame->width;
        uint32_t height = frame->height;
        if (!native) {
            width >>= fmt_desc->log2_chroma_w;
            width >>= fmt_desc->log2_chroma_h;
        }
        // 3. Create the Image
        vk::ImageCreateInfo img_info = {
            .pNext = &external_memory_img_info,
            .imageType = vk::ImageType::e2D,
            .format = format, // NV12 matching Vulkan layout
            .extent = { .width = (uint32_t) width, .height = (uint32_t) height, .depth = 1 },
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

        vk::ImageViewCreateInfo viewInfo{
            .image = vf.image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor, // Vulkan handles sub-planes internally
                .levelCount = 1,
                .layerCount = 1,
            }
        };
        vk::DescriptorImageInfo imageInfo{
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
        };
        vk::SamplerYcbcrConversionInfo viewConversionInfo = {
        };
        if (*ycbcrConversion) {
            viewConversionInfo.conversion = ycbcrConversion;
            viewInfo.pNext = &viewConversionInfo; // <-- Crucial!
        } else {
            imageInfo.sampler = sampler;
        }
        vf.imageView = device.createImageView(viewInfo);
        imageInfo.imageView = vf.imageView;

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
        auto next_idx = (frame_idx + 1) % N_INFLIGHT_VIDEO;
        auto& vf = frames[next_idx];
        auto err = device.waitForFences(*vf.copyFence, vk::True, 0);
        if (err != vk::Result::eSuccess) {
            return;
        }

        vf.frame_data = std::move(frame_data);
        AVFrame *frame = vf.frame_data.frame;
        if (frame->hw_frames_ctx) {
            upmap(vf, device, queue);
            return;
        }

        int offset[4]{};
        // Upload to Buffer:
        uint8_t *map = (uint8_t *) vf.upload_buffer_memory.mapMemory(0, upload_size);
        if (native) {
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
                    throw std::runtime_error("(planes > 3) not supported.");
                }
            }
        } else {
			uint8_t* dst_data[4];
			int dst_linesize[4];
            int inc = 0;
            for (auto i = 0; i < n_planes; i++) {
                offset[i] = inc;
                dst_data[i] = map + inc;
                if (i == 0)
                    dst_linesize[i] = width * bpp;
                else if (n_planes == 2)
                    dst_linesize[i] = (width >> fmt_desc->log2_chroma_w) * bpp * 2;
                else
                    dst_linesize[i] = (width >> fmt_desc->log2_chroma_w) * bpp;
                inc += dst_linesize[i] * (i == 0 ? height : height >> fmt_desc->log2_chroma_h);
            }
			sws_scale(sws_ctx, frame->data, frame->linesize, 0, height, dst_data, dst_linesize);
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
            vk::CommandBufferSubmitInfo cmd_info{
                .commandBuffer = vf.commandBuffer,
            };
            vk::SubmitInfo2 submit_info{
                .commandBufferInfoCount = 1,
                .pCommandBufferInfos = &cmd_info,
            };
            queue.submit2(submit_info, vf.copyFence);
        }

        vf.status = VkFrame::Upload;
    }

    bool check_next_frame(double play_time, const vk::Device& device) {
        auto next_idx = (frame_idx + 1) % N_INFLIGHT_VIDEO;
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
            return false;
        }
        return true;
    }

    VkFrame& get_current_frame() {
        return frames[frame_idx];
    }

    void discard_pending() {
        auto next_idx = (frame_idx + 1) % N_INFLIGHT_VIDEO;
        auto& vf = frames[next_idx];
        if (vf.status == VkFrame::Upload) {
            vf.status = VkFrame::Discard;
        }
    }

  	bool setup_sws_context(AVPixelFormat src_fmt, AVPixelFormat dst_fmt) {
		sws_free_context(&sws_ctx);
		sws_ctx = sws_getContext(
			width, height, src_fmt,       // Source video specs
			width, height, dst_fmt,        // Destination specs (GPU friendly)
			SWS_FAST_BILINEAR,                          // Fast filter (since size is identical)
			nullptr, nullptr, nullptr
		);
/*        const int *inv_table, *table;
        int srcRange, dstRange, brightness, contrast, saturation;

        // 1 = Full range (0-255), 0 = Limited range (16-235)
        srcRange = 0; // Set based on your input metadata
        dstRange = 1; // RGBA is almost always Full Range (1)

        inv_table = sws_getCoefficients(SWS_CS_ITU709); // Or SWS_CS_ITU601
        table     = sws_getCoefficients(SWS_CS_DEFAULT);

        sws_setColorspaceDetails(sws_ctx, inv_table, srcRange, table, dstRange, 0, 1 << 16, 1 << 16);*/

		return sws_ctx != nullptr;
	}
};