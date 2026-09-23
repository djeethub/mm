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

struct AvFrameDeleter {
    void operator()(AVFrame *frame) const {
        ff::frame_recycle(frame);
    }
};

struct AvFrameData {
    std::unique_ptr<AVFrame, AvFrameDeleter> frame;
    double play_time;
};

#ifdef _WIN32
struct D3D11Data {
    std::unique_ptr<ID3D11Texture2D, D3Deleter<ID3D11Texture2D>> tex;
    std::unique_ptr<ID3D11Texture2D, D3Deleter<ID3D11Texture2D>> shared_tex;
    std::unique_ptr<ID3D11Fence, D3Deleter<ID3D11Fence>> fence;
    vk::raii::Semaphore semaphore = nullptr;
    uint64_t counter;
};

void check_d3d_result(HRESULT hr) {
    if (hr == S_OK)
        return;

    LPTSTR errorText = nullptr;
    FormatMessage(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,
            hr,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPTSTR)&errorText,
            0,
            NULL
        );        
    SDL_Log("[D3D %x]: %s\n", hr, errorText);
//    throw std::runtime_error("D3 Failed.");
}
#endif

struct VkFrame {
    enum Status {
        None,
        New,
        Upload,
        Discard,
        Ready,
        Retry,
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
#ifdef __linux__
    std::unique_ptr<AVFrame, AvFrameDeleter> mapped;
#else
    D3D11Data mapped;
#endif
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

    void init_frame(VkFrame& vf, const AVFrame *frame, const vk::raii::Device& device) {
        if (frame->hw_frames_ctx) {
#ifdef _WIN32
            // 1. Get the texture handles from the incoming AVFrame
            ID3D11Texture2D* decoder_texture = (ID3D11Texture2D*)frame->data[0];
            ID3D11Texture2D* d3d11_texture = nullptr;
            D3D11_TEXTURE2D_DESC decoded_desc{};
            decoder_texture->GetDesc(&decoded_desc);

            D3D11_TEXTURE2D_DESC desc{
                .Width = (UINT) width,
                .Height = (UINT) height,
                .MipLevels = 1,
                .ArraySize = 1,
                .Format = decoded_desc.Format,
                .SampleDesc = {
                    .Count = 1,
                },
                .Usage = D3D11_USAGE_DEFAULT,
//                .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                .MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED,
            };
            HRESULT hr = d3d11_device->CreateTexture2D(&desc, nullptr, &d3d11_texture);
            if (hr != S_OK)
                throw std::runtime_error("CreateTexture2D failed.");

            vf.mapped.tex.reset(d3d11_texture);

            // Export the handle to Vulkan now!
            IDXGIResource1* dxgi_res = nullptr;
            d3d11_texture->QueryInterface(IID_PPV_ARGS(&dxgi_res));
            HANDLE shared_texture_handle = nullptr;
            hr = dxgi_res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &shared_texture_handle);
            dxgi_res->Release();

            vk::ExternalMemoryImageCreateInfo external_image_info{
                .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eD3D11Texture, // or KMT depending on allocation
            };
            vk::ImageCreateInfo image_info{
                .pNext = &external_image_info,
                .imageType = vk::ImageType::e2D,
                .format = format,
                .extent = { .width = (uint32_t) width, .height = (uint32_t) height, .depth = 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eSampled,
//                .sharingMode = vk::SharingMode::eExclusive,
            };
            vf.image = device.createImage(image_info);

            vk::MemoryDedicatedAllocateInfo dedicatedAllocInfo{
                .image = vf.image,
                .buffer = nullptr,
            };
            vk::ImportMemoryWin32HandleInfoKHR memory_import{
                .pNext = &dedicatedAllocInfo,
                .handleType = vk::ExternalMemoryHandleTypeFlagBits::eD3D11Texture,
                .handle = shared_texture_handle
            };
            auto req = vf.image.getMemoryRequirements();
            vk::MemoryAllocateInfo mem_alloc_info{
                .pNext = &memory_import,
                .allocationSize = req.size,
                .memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal)
            };
            vf.memory = device.allocateMemory(mem_alloc_info);
            vf.image.bindMemory(vf.memory, 0);
            CloseHandle(shared_texture_handle);

            vk::ImageViewCreateInfo viewInfo{
                .image = vf.image,     // The VkImage containing your uploaded AVFrame data
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

            if (!vf.mapped.fence) {
                vf.mapped.counter = 0;

                // 1. Query the 11.3 interface
                ID3D11Device5* d3d11_device5 = nullptr;
                d3d11_device->QueryInterface(IID_PPV_ARGS(&d3d11_device5));

                // 2. Create a shareable hardware fence
                ID3D11Fence* d3d11_fence = nullptr;
                HRESULT hr = d3d11_device5->CreateFence(
                    vf.mapped.counter, // Initial fence value
                    D3D11_FENCE_FLAG_SHARED,
                    IID_PPV_ARGS(&d3d11_fence)
                );
                vf.mapped.fence.reset(d3d11_fence);

                vk::SemaphoreTypeCreateInfo timelineInfo{
                    .semaphoreType = vk::SemaphoreType::eTimeline,
                    .initialValue = vf.mapped.counter
                };
                vk::SemaphoreCreateInfo semaphoreInfo{
                    .pNext = &timelineInfo
                };
                vf.mapped.semaphore = device.createSemaphore(semaphoreInfo);

                HANDLE shared_fence_handle = nullptr;
                hr = d3d11_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared_fence_handle);
                d3d11_device5->Release();
                vk::ImportSemaphoreWin32HandleInfoKHR importInfo{
                    .semaphore = vf.mapped.semaphore,
                    .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eD3D12Fence, // Same underlying kernel type as D3D11 Fence
                    .handle = shared_fence_handle,
                };
                device.importSemaphoreWin32HandleKHR(importInfo);
                CloseHandle(shared_fence_handle);
            }
#endif
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
            vf.image = device.createImage(info);
            auto req = vf.image.getMemoryRequirements();
            vk::MemoryAllocateInfo mem_alloc_info{
                .allocationSize = req.size,
                .memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal)
            };
            vf.memory = device.allocateMemory(mem_alloc_info);
            vf.image.bindMemory(vf.memory, 0);

            // Create the Upload Buffer:
            upload_size = get_upload_size();
            {
                vk::BufferCreateInfo buffer_info = {
                    .size = upload_size,
                    .usage = vk::BufferUsageFlagBits::eTransferSrc,
                    .sharingMode = vk::SharingMode::eExclusive
                };
                vf.upload_buffer = device.createBuffer(buffer_info);
                auto req = vf.upload_buffer.getMemoryRequirements();
                vk::MemoryAllocateInfo alloc_info = {
                    .allocationSize = req.size,
                    .memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)
                };
                vf.upload_buffer_memory = device.allocateMemory(alloc_info);
                vf.upload_buffer.bindMemory(vf.upload_buffer_memory, 0);
            }

            vk::ImageViewCreateInfo viewInfo{
                .image = vf.image,     // The VkImage containing your uploaded AVFrame data
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
        }

        vf.status = VkFrame::New;
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
#ifdef _WIN32
            init_d3d();
#endif
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
        bool canLinear = (bool) (features & vk::FormatFeatureFlagBits::eSampledImageFilterLinear);

        vk::SamplerCreateInfo samplerInfo{
            .magFilter = canLinear ? vk::Filter::eLinear : vk::Filter::eNearest,
            .minFilter = canLinear ? vk::Filter::eLinear : vk::Filter::eNearest,
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
            init_frame(frames[i], frame, device);
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
        auto frame = vf.frame_data.frame.get();

#ifdef __linux__
        AVFrame *drm_frame = ff::frame_alloc();
        // Map VAAPI surface to DRM PRIME
        drm_frame->format = AV_PIX_FMT_DRM_PRIME;
        int err = av_hwframe_map(drm_frame, frame, AV_HWFRAME_MAP_READ);
        if (err < 0) {
            ff::frame_recycle(drm_frame);
            std::println("av_hwframe_map failed.");
            return;
        }
        vf.mapped.reset(drm_frame);

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
#else
        HRESULT hr;
        ID3D11Texture2D* decoder_texture = (ID3D11Texture2D*)frame->data[0];
        UINT texture_slice_index = (UINT)(intptr_t)frame->data[1];

        IDXGIResource1* dxgi_res = nullptr;
        decoder_texture->QueryInterface(IID_PPV_ARGS(&dxgi_res));
        HANDLE shared_texture_handle = nullptr;
        hr = dxgi_res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &shared_texture_handle);
        dxgi_res->Release();

        ID3D11Texture2D* shared_tex = nullptr;
        ID3D11Device1 *d3d11_device1 = nullptr;
        hr = d3d11_device->QueryInterface(IID_PPV_ARGS(&d3d11_device1));
        d3d11_device1->OpenSharedResource1(shared_texture_handle, IID_PPV_ARGS(&shared_tex));
        vf.mapped.shared_tex.reset(shared_tex);
        d3d11_device1->Release();
        CloseHandle(shared_texture_handle);

        D3D11_BOX sourceBox{
            .right = (UINT) width,
            .bottom = (UINT) height,
            .back = 1
        };

        ID3D11DeviceContext4* d3d11_context4 = nullptr;
        hr = d3d11_context->QueryInterface(IID_PPV_ARGS(&d3d11_context4));

        hr = d3d11_context4->Wait(vf.mapped.fence.get(), vf.mapped.counter);
        d3d11_context4->CopySubresourceRegion(
            vf.mapped.tex.get(), 0,        // Destination texture and subresource slice index
            0, 0, 0,                         // Destination coordinates (X, Y, Z)
            shared_tex, texture_slice_index, // Source texture array pointer and matching active frame slice
            &sourceBox                          // Source box wrapper pointer (nullptr = Copy entire plane layout)
        );
        hr = d3d11_context4->Signal(vf.mapped.fence.get(), ++vf.mapped.counter);
        d3d11_context4->Release();

        device.resetFences(*vf.copyFence);
        vk::CommandBufferBeginInfo begin_info{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
        };        
        vf.commandBuffer.begin(begin_info);
        
        vk::ImageMemoryBarrier2 imageBarrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eUndefined,
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
        vk::DependencyInfo dep_info = {
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &imageBarrier
        };
        vf.commandBuffer.pipelineBarrier2(dep_info);

        vf.commandBuffer.end();
/*
        uint64_t acquireKey = 1;
        uint64_t releaseKey = 0;
        uint32_t timeoutMs  = INFINITE;
        vk::Win32KeyedMutexAcquireReleaseInfoKHR keyedMutexInfo{
            .acquireCount = 1,
            .pAcquireSyncs = &*vf.memory,
            .pAcquireKeys = &acquireKey,
            .pAcquireTimeouts = &timeoutMs,
            .releaseCount = 1,
            .pReleaseSyncs = &*vf.memory,
            .pReleaseKeys = &releaseKey,
        };*/
        vk::SemaphoreSubmitInfo wait_info{
            .semaphore = vf.mapped.semaphore,
            .value = vf.mapped.counter,
            .stageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        };
        vk::SemaphoreSubmitInfo signal_info{
            .semaphore = vf.mapped.semaphore,
            .value = ++vf.mapped.counter,
        };
        vk::CommandBufferSubmitInfo cmd_info{
            .commandBuffer = vf.commandBuffer,
        };
        vk::SubmitInfo2 submit_info{
//            .pNext = &keyedMutexInfo,
            .waitSemaphoreInfoCount = 1,
            .pWaitSemaphoreInfos = &wait_info,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &cmd_info,
            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos = &signal_info
        };
        queue.submit2(submit_info, vf.copyFence);
#endif
        
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
        AVFrame *frame = vf.frame_data.frame.get();
        if (frame->hw_frames_ctx) {
            while (true) {
                upmap(vf, device, queue);
                if (vf.status == VkFrame::Retry) {
                    init_frame(vf, vf.frame_data.frame.get(), device);
                    continue;
                }
                break;
            }
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
    
#ifdef __WIN32
private:
    std::unique_ptr<ID3D11Device, D3Deleter<ID3D11Device>> d3d11_device;
    std::unique_ptr<ID3D11DeviceContext, D3Deleter<ID3D11DeviceContext>> d3d11_context;

    void init_d3d() {
        ID3D11Device *d3d11_device = this->d3d11_device.get();
        ID3D11DeviceContext *d3d11_context = this->d3d11_context.get();
        if (!d3d11_device) {
            D3D_FEATURE_LEVEL featureLevels[] = {
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0
            };
            D3D_FEATURE_LEVEL selectedFeatureLevel;

            // 2. Ensure BGRA/Sharing support is active via creation flags
            UINT creationFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
            creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif                
            HRESULT hr = D3D11CreateDevice(
                nullptr,                    // Use default adapter (or your matched Vulkan GPU LUID adapter)
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                creationFlags,
                featureLevels,
                2,                          // Array size
                D3D11_SDK_VERSION,
                &d3d11_device,
                &selectedFeatureLevel,
                &d3d11_context
            );
            if (selectedFeatureLevel < D3D_FEATURE_LEVEL_11_1) {
                // NT Handles and 10-bit AV1 zero-copy configurations may fail on this hardware/driver level
            }
#ifndef NDEBUG
            ID3D11Debug *d3dDebug;
            if (SUCCEEDED(d3d11_device->QueryInterface(IID_PPV_ARGS(&d3dDebug)))) {
                ID3D11InfoQueue *infoQueue;
                if (SUCCEEDED(d3dDebug->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
                    // Optional: break on serious problems
                    infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
                    infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);

                    // Optional: push an empty filter so nothing is filtered out
                    infoQueue->PushEmptyStorageFilter();
                    infoQueue->Release();
                }
                d3dDebug->Release();
            } else {
            }
#endif
            this->d3d11_device.reset(d3d11_device);
            this->d3d11_context.reset(d3d11_context);
        }
    }
#endif
};