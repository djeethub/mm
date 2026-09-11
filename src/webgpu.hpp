#pragma once

#include <dawn/webgpu_cpp.h>
#include <iostream>
#include <unistd.h> // close

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
}
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#include "imgui_impl_wgpu.h"

#include "colorspace.hpp"
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
#include "nv12_ext.frag.h"
#include "test.vert.h"
#include "test.frag.h"
#include "wgsl.hpp"

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
	YUVA_FRAG,
    NV12_EXT_FRAG,
    TEST_VERT,
    TEST_FRAG
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
    float scale;
};

enum UniformUpdate {
    Clean,
    All,
    Transform
};

void UncapturedErrorCallback(const wgpu::Device& device, wgpu::ErrorType type, wgpu::StringView message) {
    std::string_view msgStr(message.data, message.length);
    std::cerr << "[Dawn Error " << static_cast<int>(type) << "]: " 
            << msgStr << std::endl;
}

class AppWgpu {
private:
    wgpu::Instance instance = nullptr;
    wgpu::Device device = nullptr;
    wgpu::Surface surface = nullptr;
    wgpu::Queue queue = nullptr;
    wgpu::SurfaceConfiguration config;
    wgpu::RenderPipeline pipeline = nullptr;
    wgpu::BindGroup bind_group_0 = nullptr;
    wgpu::BindGroup bind_group_1 = nullptr;
    wgpu::Sampler sampler_linear = nullptr;
    wgpu::Sampler sampler_nearest = nullptr;
    wgpu::Buffer buffer_uniform_media = nullptr;
    wgpu::Buffer buffer_uniform_transform = nullptr;
    wgpu::SharedTextureMemory sharedMemory = nullptr;
    wgpu::Texture texture[4]{};
    wgpu::TextureView textureView[4]{};
    wgpu::ExternalTexture extTexture = nullptr;

    SDL_Window *window;
	AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
	const AVPixFmtDescriptor *fmt_desc;
	int width = 0;
	int height = 0;
	int de_width = 2;
	int de_height = 2;
	int wnd_w = 0;
	int wnd_h = 0;
	float base_scale = 0.0;
	AVFrame *frame = nullptr;
	int n_bindings = 0;
	int bpp;
    UniformUpdate uniform_update = All;
    bool is_hwframe() {
        return frame->hw_frames_ctx != nullptr;
    }
	SwsContext *sws_ctx = nullptr;

	bool setup_sws_context(AVPixelFormat src_fmt, AVPixelFormat dst_fmt) {
		sws_free_context(&sws_ctx);
		sws_ctx = sws_getContext(
			width, height, src_fmt,       // Source video specs
			width, height, dst_fmt,        // Destination specs (GPU friendly)
			SWS_BILINEAR,                          // Fast filter (since size is identical)
			NULL, NULL, NULL
		);
		return sws_ctx != nullptr;
	}

    void destroy_textures() {
        sharedMemory = nullptr;
        for (auto& e : texture) {
            e = nullptr;
        }
        for (auto& e : textureView) {
            e = nullptr;
        }
    }

	void create_texture(AVFrame *frame)
	{
		if (frame->format == pix_fmt && frame->width == width && frame->height == height)
			return;

        destroy_textures();
		pix_fmt = static_cast<AVPixelFormat>(frame->format);
		fmt_desc = av_pix_fmt_desc_get(pix_fmt);
//		SDL_Log("out_pix_fmt: %s\n", av_get_pix_fmt_name(pix_fmt));
		init_pipeline();
		width = frame->width;
		height = frame->height;
		reset_scale();

        if (!is_hwframe()) {
            if (n_bindings == 1) {
                if (fmt_desc->nb_components == 1) {
                    create_plane_texture(0, width, height, wgpu::TextureFormat::R8Unorm);
                    if (fmt_desc->flags & AV_PIX_FMT_FLAG_PAL)
                        create_plane_texture(1, 256, 1, wgpu::TextureFormat::RGBA8Unorm);
                } else if (fmt_desc->nb_components == 2) {
                    create_plane_texture(0, width, height, wgpu::TextureFormat::RG8Unorm);
                } else {
                    switch (pix_fmt)
                    {
                    case AV_PIX_FMT_BGR24:
                        setup_sws_context(pix_fmt, AV_PIX_FMT_BGRA);
                    case AV_PIX_FMT_BGRA:
                        create_plane_texture(0, width, height, wgpu::TextureFormat::BGRA8Unorm);
                        break;

                    case AV_PIX_FMT_RGB24:
                        setup_sws_context(pix_fmt, AV_PIX_FMT_RGBA);
                    default:
                        create_plane_texture(0, width, height, wgpu::TextureFormat::RGBA8Unorm);
                    }
                }
            } else if (n_bindings == 2) {
                if (bpp > 1) {
                    create_plane_texture(0, width, height, wgpu::TextureFormat::R16Unorm);
                    create_plane_texture(1, width / de_width, height / de_height, wgpu::TextureFormat::RG16Unorm);
                } else {
                    create_plane_texture(0, width, height, wgpu::TextureFormat::R8Unorm);
                    create_plane_texture(1, width / de_width, height / de_height, wgpu::TextureFormat::RG8Unorm);
                }
            } else {
                if (bpp > 1) {
                    create_plane_texture(0, width, height, wgpu::TextureFormat::R16Unorm);
                    create_plane_texture(1, width / de_width, height / de_height, wgpu::TextureFormat::R16Unorm);
                    create_plane_texture(2, width / de_width, height / de_height, wgpu::TextureFormat::R16Unorm);
                    if (n_bindings > 3)
                        create_plane_texture(3, width, height, wgpu::TextureFormat::R16Unorm);
                } else {
                    create_plane_texture(0, width, height, wgpu::TextureFormat::R8Unorm);
                    create_plane_texture(1, width / de_width, height / de_height, wgpu::TextureFormat::R8Unorm);
                    create_plane_texture(2, width / de_width, height / de_height, wgpu::TextureFormat::R8Unorm);
                    if (n_bindings > 3)
                        create_plane_texture(3, width, height, wgpu::TextureFormat::R8Unorm);
                }
            }
        }
	}

	wgpu::ShaderModule load_shader(ShaderType type)
	{
        wgpu::ShaderSourceSPIRV spirv_desc = {};
        spirv_desc.sType = wgpu::SType::ShaderSourceSPIRV;

		switch (type)
		{
		case VERT:
            spirv_desc.code = (const uint32_t *)vert_vert;
            spirv_desc.codeSize = vert_vert_len / sizeof(uint32_t);
			break;

		case NV12_FRAG:
            spirv_desc.code = (const uint32_t *)nv12_frag;
            spirv_desc.codeSize = nv12_frag_len / sizeof(uint32_t);
			break;

		case RGB_FRAG:
            spirv_desc.code = (const uint32_t *)rgb_frag;
            spirv_desc.codeSize = rgb_frag_len / sizeof(uint32_t);
			break;

		case YUV_FRAG:
            spirv_desc.code = (const uint32_t *)yuv_frag;
            spirv_desc.codeSize = yuv_frag_len / sizeof(uint32_t);
			break;

		case YUV_10_FRAG:
            spirv_desc.code = (const uint32_t *)yuv_10_frag;
            spirv_desc.codeSize = yuv_10_frag_len / sizeof(uint32_t);
			break;

		case GRAY_FRAG:
            spirv_desc.code = (const uint32_t *)gray_frag;
            spirv_desc.codeSize = gray_frag_len / sizeof(uint32_t);
			break;

		case PAL8_FRAG:
            spirv_desc.code = (const uint32_t *)pal8_frag;
            spirv_desc.codeSize = pal8_frag_len / sizeof(uint32_t);
			break;

		case YUYV_FRAG:
            spirv_desc.code = (const uint32_t *)yuyv_frag;
            spirv_desc.codeSize = yuyv_frag_len / sizeof(uint32_t);
			break;

		case YA_FRAG:
            spirv_desc.code = (const uint32_t *)ya_frag;
            spirv_desc.codeSize = ya_frag_len / sizeof(uint32_t);
			break;

		case YUVA_FRAG:
            spirv_desc.code = (const uint32_t *)yuva_frag;
            spirv_desc.codeSize = yuva_frag_len / sizeof(uint32_t);
			break;

        case NV12_EXT_FRAG:
            spirv_desc.code = (const uint32_t *)nv12_ext_frag;
            spirv_desc.codeSize = nv12_ext_frag_len / sizeof(uint32_t);
            break;

        case TEST_VERT:
            spirv_desc.code = (const uint32_t *)test_vert;
            spirv_desc.codeSize = test_vert_len / sizeof(uint32_t);
			break;

        case TEST_FRAG:
            spirv_desc.code = (const uint32_t *)test_frag;
            spirv_desc.codeSize = test_frag_len / sizeof(uint32_t);
			break;
		}

        wgpu::ShaderModuleDescriptor desc = {};
        desc.nextInChain = &spirv_desc;

        int validation_error = 0;
        device.PushErrorScope(wgpu::ErrorFilter::Validation);
        auto module = device.CreateShaderModule(&desc);
        device.PopErrorScope(wgpu::CallbackMode::AllowSpontaneous, [](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView message, void* userdata1) {
            if (type == wgpu::ErrorType::Validation) {
                *static_cast<int*>(userdata1) = 1;
                // Print the detailed error message
                if (message.data != nullptr && message.length > 0) {
                    std::string errorStr(message.data, message.length);
                    std::printf("Dawn Validation Error:\n%s\n", errorStr.c_str());
                } else {
                    std::printf("Dawn Validation Error occurred, but no message was provided.\n");
                }                
            }
        }, &validation_error);

        wgpu::ComputeState stage_desc = {};
        if (module && !validation_error)
        {
            return module;
        }
        return nullptr;
	}

    wgpu::ShaderModule load_shader(const char* wgsl_source)
    {
        wgpu::ShaderSourceWGSL wgsl = {};
        wgsl.code = wgsl_source;

        wgpu::ShaderModuleDescriptor desc = {
            .nextInChain = &wgsl
        };

        // Detect shader compilation errors by using an error scope.
        int validation_error = 0;
        device.PushErrorScope(wgpu::ErrorFilter::Validation);
        auto module = device.CreateShaderModule(&desc);
        device.PopErrorScope(wgpu::CallbackMode::AllowSpontaneous, [](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView message, void* userdata1) {
            if (type == wgpu::ErrorType::Validation) {
                *static_cast<int*>(userdata1) = 1;
                if (message.data != nullptr && message.length > 0) {
                    std::string errorStr(message.data, message.length);
                    std::printf("Dawn Validation Error:\n%s\n", errorStr.c_str());
                } else {
                    std::printf("Dawn Validation Error occurred, but no message was provided.\n");
                }                
            }
        }, &validation_error);

        if (module && !validation_error) {
            return module;
        }
        return nullptr;
    }

    bool init_pipeline()
    {
        // Create the vertex shader
        auto vertex_shader_module = load_shader(VERT);
        wgpu::ShaderModule frag_shader_module;

        std::vector<wgpu::BindGroupLayoutEntry> layout_entries_0 = {
            {
                .binding = 0,
                .visibility = wgpu::ShaderStage::Vertex,
                .buffer = {
                    .type = wgpu::BufferBindingType::Uniform,
                }
            },
            {
                .binding = 1,
                .visibility = wgpu::ShaderStage::Fragment,
                .buffer = {
                    .type = wgpu::BufferBindingType::Uniform,
                }
            },
            {
                .binding = 2,
                .visibility = wgpu::ShaderStage::Fragment,
                .sampler = {
                    .type = wgpu::SamplerBindingType::Filtering
                }
            }            
        };
        wgpu::BindGroupLayoutDescriptor bglDesc = {
            .entryCount = layout_entries_0.size(),
            .entries = layout_entries_0.data()
        };
        std::vector<wgpu::BindGroupLayout> bindGroupLayout;
        bindGroupLayout.push_back(device.CreateBindGroupLayout(&bglDesc));

        std::vector<wgpu::BindGroupLayoutEntry> layout_entries_1;
        if (is_hwframe()) {
            frag_shader_module = load_shader(__nv12_ext_frag_wgsl);

            wgpu::ExternalTextureBindingLayout externalLayout = {};
            layout_entries_1.push_back({
                .nextInChain = &externalLayout,
                .binding = 0,
                .visibility = wgpu::ShaderStage::Fragment,
            });
        } else {
            de_width = 1 << fmt_desc->log2_chroma_w;
            de_height = 1 << fmt_desc->log2_chroma_h;
            n_bindings = av_pix_fmt_count_planes(pix_fmt);

            ShaderType frag;
            if (n_bindings == 1) {
                bpp = 0;
                for (auto i = 0; i < fmt_desc->nb_components; i++) {
                    bpp += fmt_desc->comp[i].depth;
                }
                bpp /= 8;
                if (bpp == 3)
                    bpp++;
                switch (fmt_desc->nb_components) {
                    case 1:
                        if (fmt_desc->flags & AV_PIX_FMT_FLAG_PAL)
                            frag = PAL8_FRAG;
                        else
                            frag = GRAY_FRAG;
                        break;
                    case 2:
                        frag = YA_FRAG;
                        break;
                    default:
                        if (fmt_desc->flags & AV_PIX_FMT_FLAG_RGB)
                            frag = RGB_FRAG;
                        else
                            frag = YUYV_FRAG;
                }
            } else {
                bpp = (fmt_desc->comp[0].depth + 7) / 8;
                switch (n_bindings) {
                case 2:
                    frag = NV12_FRAG;
                    break;
                case 4:
                    frag = YUVA_FRAG;
                    break;
                default:
                    if (bpp > 1)
                        frag = YUV_10_FRAG;
                    else 
                        frag = YUV_FRAG;
                }
            }

            frag_shader_module = load_shader(__nv12_frag_wgsl);

            layout_entries_1.resize(n_bindings);
            for (uint32_t i = 0; i < n_bindings; i++) {
                layout_entries_1[i] = {
                    .binding = i,
                    .visibility = wgpu::ShaderStage::Fragment,
                    .texture = {
                        .sampleType = wgpu::TextureSampleType::Float,
                        .viewDimension = wgpu::TextureViewDimension::e2D
                    }
                };
            }
        }
        bglDesc = {
            .entryCount = layout_entries_1.size(),
            .entries = layout_entries_1.data()
        };
        bindGroupLayout.push_back(device.CreateBindGroupLayout(&bglDesc));

        wgpu::PipelineLayoutDescriptor layout_desc = {
            .bindGroupLayoutCount = bindGroupLayout.size(),
            .bindGroupLayouts = bindGroupLayout.data()
        };

        wgpu::ColorTargetState color_state = {
            .format = config.format,
        };
        if (fmt_desc->flags & AV_PIX_FMT_FLAG_ALPHA) {
            wgpu::BlendState blend_state = {
                .color = {
                    .operation = wgpu::BlendOperation::Add,
                    .srcFactor = wgpu::BlendFactor::SrcAlpha,
                    .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha,
                },
                .alpha = {
                    .operation = wgpu::BlendOperation::Add,
                    .srcFactor = wgpu::BlendFactor::One,
                    .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha,
                },
            };
            color_state.blend = &blend_state;
        }
        wgpu::FragmentState fragment_state = {
            .module = frag_shader_module,
            .targetCount = 1,
            .targets = &color_state
        };

        wgpu::RenderPipelineDescriptor texture_pipeline_desc = {
            .layout = device.CreatePipelineLayout(&layout_desc),
            .vertex = {
                .module = vertex_shader_module,
            },
            .primitive = {
                .topology = wgpu::PrimitiveTopology::TriangleStrip,
            },
//            .multisample = bd->initInfo.PipelineMultisampleState
            .fragment = &fragment_state
        };

        pipeline = device.CreateRenderPipeline(&texture_pipeline_desc);

        wgpu::BufferDescriptor ub_desc =
        {
            .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::Uniform,
            .size = sizeof(Vertform)
        };
        buffer_uniform_transform = device.CreateBuffer(&ub_desc);
        ub_desc =
        {
            .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::Uniform,
            .size = sizeof(Uniforms)
        };
        buffer_uniform_media = device.CreateBuffer(&ub_desc);

        if (!sampler_linear) {
            wgpu::SamplerDescriptor sampler_desc = {
                .addressModeU = wgpu::AddressMode::ClampToEdge,
                .addressModeV = wgpu::AddressMode::ClampToEdge,
                .addressModeW = wgpu::AddressMode::ClampToEdge,
                .magFilter = wgpu::FilterMode::Linear,
                .minFilter = wgpu::FilterMode::Linear,
                .mipmapFilter = wgpu::MipmapFilterMode::Linear,
                .maxAnisotropy = 1,
            };
            sampler_linear = device.CreateSampler(&sampler_desc);

            sampler_desc.minFilter = wgpu::FilterMode::Nearest;
            sampler_desc.magFilter = wgpu::FilterMode::Nearest;
            sampler_desc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
            sampler_nearest = device.CreateSampler(&sampler_desc);
        }

        wgpu::BindGroupEntry bgEntries[] = {
            {
                .binding = 0,
                .buffer = buffer_uniform_transform,
            },
            {
                .binding = 1,
                .buffer = buffer_uniform_media,
            },
            {
                .binding = 2,
                .sampler = (fmt_desc->flags & AV_PIX_FMT_FLAG_PAL) ? sampler_nearest : sampler_linear,
            }
        };
        wgpu::BindGroupDescriptor bindGroupDesc{
            .layout = bindGroupLayout[0],
            .entryCount = layout_entries_0.size(),
            .entries = bgEntries
        };
        bind_group_0 = device.CreateBindGroup(&bindGroupDesc);

		uniform_update = All;

        return true;
    }    

    // This function takes your decoded hardware AVFrame and extracts its GPU file descriptors
    VADRMPRIMESurfaceDescriptor ExtractDmaBufFromFrame(AVFrame* hw_frame) {
        AVHWFramesContext* hw_ctx = (AVHWFramesContext*)hw_frame->hw_frames_ctx->data;
        AVVAAPIDeviceContext* va_dev_ctx = (AVVAAPIDeviceContext*)hw_ctx->device_ctx->hwctx;
        VADisplay va_display = va_dev_ctx->display;
//        VASurfaceID va_surface = (VASurfaceID)(uintptr_t)hw_frame->data;
        VASurfaceID va_surface = (VASurfaceID)(uintptr_t)hw_frame->data[3];
        // Explicitly sync the surface execution thread to resolve asynchronous race blocks
        vaSyncSurface(va_display, va_surface);

        // Use the correct structure here
        VADRMPRIMESurfaceDescriptor va_desc{};
        VAStatus status = vaExportSurfaceHandle(
            va_display,
            va_surface,
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 
            VA_EXPORT_SURFACE_READ_ONLY,
            &va_desc
        );

        if (status != VA_STATUS_SUCCESS) {
            throw std::runtime_error("Failed to export VA-API surface to DMA-BUF!");
        }

        return va_desc;
    }

    wgpu::ExternalTexture CreateVideoTextureFromDmaBuf(
        const VADRMPRIMESurfaceDescriptor& va_desc, 
        uint32_t width, 
        uint32_t height
    ) {
        // 1. Describe the raw Linux DMA memory block mapping to Dawn
        wgpu::SharedTextureMemoryDmaBufDescriptor dmaBufDesc{};
        dmaBufDesc.drmFormat = va_desc.fourcc;
        dmaBufDesc.drmModifier = va_desc.objects[0].drm_format_modifier;
        dmaBufDesc.planeCount = va_desc.num_layers;
        dmaBufDesc.size = { width, height, 1 };
        
        std::vector<wgpu::SharedTextureMemoryDmaBufPlane> planes(va_desc.num_layers);
        for (uint32_t i = 0; i < va_desc.num_layers; ++i) {
            // VAAPI maps layers to objects using an internal object_index array
            uint32_t obj_idx = va_desc.layers[i].object_index[0];
            planes[i].fd = va_desc.objects[obj_idx].fd;
            planes[i].offset = va_desc.layers[i].offset[0];
            planes[i].stride = va_desc.layers[i].pitch[0];
        }
        dmaBufDesc.planes = planes.data();

        wgpu::SharedTextureMemoryDescriptor sharedDesc{};
        sharedDesc.nextInChain = &dmaBufDesc;

        // 2. Import into hardware-managed memory state
        sharedMemory = device.ImportSharedTextureMemory(&sharedDesc);

        wgpu::SharedTextureMemoryProperties properties;
        sharedMemory.GetProperties(&properties);

        // 3. Create the multi-planar master texture (Dawn automatically detects NV12/YUV)
        wgpu::TextureDescriptor textureDesc{};
        textureDesc.size = { width, height, 1 };
        textureDesc.usage = wgpu::TextureUsage::TextureBinding;
//        textureDesc.usage = properties.usage;
        textureDesc.format = properties.format;
        
        texture[0] = sharedMemory.CreateTexture(&textureDesc);

        // Separate the Y (Plane0) and UV (Plane1) texture components via native Aspect Views
        wgpu::TextureViewDescriptor plane0Desc{};
        plane0Desc.aspect = wgpu::TextureAspect::Plane0Only; // Focus on Luminance
//        plane0Desc.format = wgpu::TextureFormat::R8Unorm;
        textureView[0] = texture[0].CreateView(&plane0Desc);

        if (va_desc.num_layers > 1) {
            wgpu::TextureViewDescriptor plane1Desc{};
            plane1Desc.aspect = wgpu::TextureAspect::Plane1Only; // Focus on Chrominance
//            plane1Desc.format = wgpu::TextureFormat::RG8Unorm;
            textureView[1] = texture[0].CreateView(&plane1Desc);
        }

        static const float kLinearTransferFn[] = {
            1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f
        };

        const float* chosenMatrix = kMatrix_BT709_Limited; // Default fallback
        if (frame->color_range == AVCOL_RANGE_JPEG) {
            if (frame->colorspace == AVCOL_SPC_BT709) {
                chosenMatrix = kMatrix_BT709_Full;
            } else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M) {
                chosenMatrix = kMatrix_BT601_Full;
            }
        } else { // Limited Range
            if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M) {
                chosenMatrix = kMatrix_BT601_Limited;
            } else if (frame->colorspace == AVCOL_SPC_BT2020_NCL) {
                chosenMatrix = kMatrix_BT2020_Limited;
            }
        }

        // 4. Wrap the planes into an actionable ExternalTexture object for your shaders
        wgpu::ExternalTextureDescriptor externalDesc{
            .plane0 = textureView[0],
            .plane1 = textureView[1],
            .cropSize = { (uint32_t) frame->width, (uint32_t) frame->height },
            .yuvToRgbConversionMatrix = chosenMatrix,
            .srcTransferFunctionParameters = kLinearTransferFn,
            .dstTransferFunctionParameters = kLinearTransferFn,        
            .gamutConversionMatrix = kIdentityGamut,
        };
        externalDesc.apparentSize = externalDesc.cropSize;

        // Modern Dawn uses a dedicated factory function to instantiate the wrapper asset
        extTexture = device.CreateExternalTexture(&externalDesc);
        return extTexture;
    }

	void create_plane_texture(int idx, int w, int h, wgpu::TextureFormat format)
	{
        wgpu::TextureDescriptor tex_desc = {
            .usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::TextureBinding,
            .dimension = wgpu::TextureDimension::e2D,
            .size = { (uint32_t) w, (uint32_t) h, 1},
            .format = format,
        };
        texture[idx] = device.CreateTexture(&tex_desc);
        wgpu::TextureViewDescriptor view_desc = {
            .format = format,
            .dimension = wgpu::TextureViewDimension::e2D,
            .baseMipLevel = 0,
            .mipLevelCount = 1,
            .baseArrayLayer = 0,
            .arrayLayerCount = 1,
            .aspect = wgpu::TextureAspect::All,
        };
        textureView[idx] = texture[idx].CreateView(&view_desc);
	}

	void upload_plane(int idx, const uint8_t *data, int linesize, Uint32 width, Uint32 height, int bytes_per_pixel)
	{
        wgpu::TexelCopyTextureInfo dest_info = {
            .texture = texture[idx],
            .aspect = wgpu::TextureAspect::All
        };
        wgpu::TexelCopyBufferLayout layout = {
            .bytesPerRow = (uint32_t) linesize,
            .rowsPerImage = height,
        };
        wgpu::Extent3D write_size = { width, height, 1 };
        queue.WriteTexture(&dest_info, data, linesize * height, &layout, &write_size);
	}

	void push_uniforms() {
        switch (uniform_update) {
            case All:
            {
                Uniforms u = {0};
                u.tex_size[0] = (float)frame->width;
                u.tex_size[1] = (float)frame->height;
                // Range (simplified – you can improve this with frame->color_range)
                u.color_range = frame->color_range == AVCOL_RANGE_UNSPECIFIED ? AVCOL_RANGE_MPEG : frame->color_range;
                // Color matrix
                u.colorspace = frame->colorspace == AVCOL_SPC_UNSPECIFIED ? AVCOL_SPC_BT709 : frame->colorspace;
                u.scale = video_scale;
                queue.WriteBuffer(buffer_uniform_media, 0, &u, sizeof(u));
            }
            case Transform:
            {
                auto scale = base_scale * video_scale;
                float w = 2.0f * scale * frame->width / wnd_w;
                float h = 2.0f * scale * frame->height / wnd_h;
                Vertform tf = {
                    .position = { video_pan_x / wnd_w, video_pan_y / wnd_h },
                    .size = { w, h }
                };
                queue.WriteBuffer(buffer_uniform_transform, 0, &tf, sizeof(tf));
            }
        }
        uniform_update = Clean;
    }

    void prepare_texture_draw() {
        if (is_hwframe()) {
            auto va_desc = ExtractDmaBufFromFrame(frame);
            wgpu::ExternalTexture videoFrame = CreateVideoTextureFromDmaBuf(va_desc, frame->width, frame->height);
            for (uint32_t i = 0; i < va_desc.num_objects; ++i) {
                if (va_desc.objects[i].fd >= 0) {
                    close(va_desc.objects[i].fd);
                }
            }

            // 1. Instantiate the specialized WebGPU extension entry for external textures
            wgpu::ExternalTextureBindingEntry externalBinding{};
            externalBinding.externalTexture = videoFrame; // <--- Pass your video texture here

            wgpu::BindGroupEntry bgEntries[] = {
                {
                    .nextInChain = &externalBinding,
                    .binding = 0,
                }
            };

            wgpu::BindGroupDescriptor bindGroupDesc{};
            bindGroupDesc.layout = pipeline.GetBindGroupLayout(1),
            bindGroupDesc.entryCount = 1;
            bindGroupDesc.entries = bgEntries;
            bind_group_1 = device.CreateBindGroup(&bindGroupDesc);
        } else {
            switch (n_bindings)
            {
            case 1:
                upload_plane(0, frame->data[0], frame->linesize[0], width, height, bpp);
                if (fmt_desc->flags & AV_PIX_FMT_FLAG_PAL)
                    upload_plane(1, frame->data[1], frame->linesize[1], 256, 1, 4);
                break;

            case 2:
                upload_plane(0, frame->data[0], frame->linesize[0], width, height, bpp);
                upload_plane(1, frame->data[1], frame->linesize[1], width / de_width, height / de_height, bpp * 2);
                break;

            case 4:
                upload_plane(3, frame->data[3], frame->linesize[3], width, height, bpp);
            default:
                upload_plane(0, frame->data[0], frame->linesize[0], width, height, bpp);
                upload_plane(1, frame->data[1], frame->linesize[1], width / de_width, height / de_height, bpp);
                upload_plane(2, frame->data[2], frame->linesize[2], width / de_width, height / de_height, bpp);
            }

            wgpu::BindGroupEntry bgEntries[n_bindings];
            for (uint32_t i = 0; i < n_bindings; i++) {
                bgEntries[i] = {
                    .binding = i,
                    .textureView = textureView[i]
                };
            }

            wgpu::BindGroupDescriptor bindGroupDesc{};
            bindGroupDesc.layout = pipeline.GetBindGroupLayout(1),
            bindGroupDesc.entryCount = n_bindings;
            bindGroupDesc.entries = bgEntries;
            bind_group_1 = device.CreateBindGroup(&bindGroupDesc);
        }
    }

public:
    float video_scale = 1.0;
    float video_pan_x = 0.0;
    float video_pan_y = 0.0;
	auto get_pix_fmt() { return pix_fmt; }
    auto get_device() { return device; }
    auto get_format() { return config.format; }
    bool zero_p010 = false;

    bool init(SDL_Window *window) {
        this->window = window;
        wgpu::InstanceFeatureName features[] = {
            wgpu::InstanceFeatureName::TimedWaitAny,
            wgpu::InstanceFeatureName::ShaderSourceSPIRV,
        };
        wgpu::InstanceDescriptor instanceDesc {
            .requiredFeatureCount = 2,
            .requiredFeatures = features
        };
        instance = wgpu::CreateInstance(&instanceDesc);

        SDL_PropertiesID props = SDL_GetWindowProperties(window);
#ifdef _WIN32
        // --- WINDOWS (Win32) SURFACE CREATION ---
        auto hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        auto hinstance = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);

        if (hwnd && hinstance) {
            wgpu::SurfaceSourceWindowsHWND winSource{};
            winSource.hwnd = hwnd;
            winSource.hinstance = hinstance;

            wgpu::SurfaceDescriptor surfaceDesc{};
            surfaceDesc.nextInChain = &winSource;
            surface = instance.CreateSurface(&surfaceDesc);
        }
#else
        // --- LINUX (Wayland / X11) SURFACE CREATION ---
        // 1. Try Wayland first (Modern Linux Desktop default)
        auto wl_display = static_cast<struct wl_display*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
        auto wl_surface = static_cast<struct wl_surface*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));

        if (wl_display && wl_surface) {
            wgpu::SurfaceSourceWaylandSurface waylandSource{};
            waylandSource.display = wl_display;
            waylandSource.surface = wl_surface;

            wgpu::SurfaceDescriptor surfaceDesc{};
            surfaceDesc.nextInChain = &waylandSource;
            surface = instance.CreateSurface(&surfaceDesc);
        } 
        // 2. Fall back to X11 (If running under Xorg or XWayland)
        else {
            auto x11_display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
            uint64_t x11_window = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);

            if (x11_display && x11_window != 0) {
                wgpu::SurfaceSourceXlibWindow x11Source{};
                x11Source.display = x11_display;
                x11Source.window = x11_window;

                wgpu::SurfaceDescriptor surfaceDesc{};
                surfaceDesc.nextInChain = &x11Source;
                surface = instance.CreateSurface(&surfaceDesc);
            }
        }
#endif

        if (!surface) {
            std::cerr << "Failed to extract native window handles from SDL3 for WebGPU surface!" << std::endl;
            return false;
        }

        // 5. Request a physical graphics Adapter (GPU)
        wgpu::RequestAdapterOptions adapterOptions{};
        adapterOptions.compatibleSurface = surface;
        adapterOptions.powerPreference = wgpu::PowerPreference::HighPerformance;

        // Modern callback expects wgpu:: C++ types and wgpu::StringView instead of raw char*
        auto adapterCallback = [](wgpu::RequestAdapterStatus status, wgpu::Adapter cAdapter, wgpu::StringView message, void* userdata) {
            if (status == wgpu::RequestAdapterStatus::Success) {
                *static_cast<wgpu::Adapter*>(userdata) = cAdapter;
            } else {
                std::cerr << "Failed to acquire Adapter: " << message.data << std::endl;
            }
        };

        wgpu::Adapter adapter = nullptr;

        // Modern Dawn requires a CallbackMode parameter (e.g., wgpu::CallbackMode::WaitAnyOnly)
        wgpu::Future adapterFuture = instance.RequestAdapter(
            &adapterOptions, 
            wgpu::CallbackMode::WaitAnyOnly, 
            adapterCallback, 
            &adapter
        );

        // Wait for the asynchronous request to complete
        instance.WaitAny(adapterFuture, UINT64_MAX);

        if (!adapter) {
            std::cerr << "Failed to create WebGPU Adapter!" << std::endl;
            return false;
        }

/*
        wgpu::AdapterInfo info;
        adapter.GetInfo(&info);

        std::cout << "Backend Type: ";
        switch (info.backendType) {
            case wgpu::BackendType::Vulkan: std::cout << "Vulkan\n"; break;
            case wgpu::BackendType::OpenGL: std::cout << "OpenGL\n"; break;
            case wgpu::BackendType::OpenGLES: std::cout << "OpenGLES\n"; break;
            case wgpu::BackendType::Null:   std::cout << "Null (Software/Mock)\n"; break;
            default:                        std::cout << "Other Backend\n"; break;
        }
        std::cout << "Device Name: " << info.device.data << "\n";        
*/
        if (adapter.HasFeature(wgpu::FeatureName::MultiPlanarFormatP010)) {
            zero_p010 = true;
        }

        // 6. Request the logical Device
/*        const char* allowUnsafeApiToggle = "allow_unsafe_apis";
        wgpu::DawnTogglesDescriptor togglesDesc{};
        togglesDesc.enabledToggles = &allowUnsafeApiToggle;
        togglesDesc.enabledToggleCount = 1;*/

        std::vector<wgpu::FeatureName> features_dev = {
//            wgpu::FeatureName::DawnDrmFormatCapabilities,
            wgpu::FeatureName::SharedTextureMemoryDmaBuf,
            wgpu::FeatureName::DawnMultiPlanarFormats,  // important for NV12
//            wgpu::FeatureName::MultiPlanarFormatP010,
//            wgpu::FeatureName::StaticSamplers,
//            wgpu::FeatureName::DawnInternalUsages,
            wgpu::FeatureName::SharedFenceSyncFD,
            // optionally SharedFenceSyncFD / SharedFenceVkSemaphoreOpaqueFD for sync
            wgpu::FeatureName::TextureFormatsTier1,
            wgpu::FeatureName::TextureFormatsTier2,
        };
        if (zero_p010) {
            features_dev.push_back(wgpu::FeatureName::MultiPlanarFormatP010);
        }
        wgpu::DeviceDescriptor deviceDesc{};
        deviceDesc.label = "Main Dawn Device";
//        deviceDesc.nextInChain = &togglesDesc;
        deviceDesc.requiredFeatures = features_dev.data();
        deviceDesc.requiredFeatureCount = features_dev.size();
        deviceDesc.SetUncapturedErrorCallback(UncapturedErrorCallback);
        
        auto deviceCallback = [](wgpu::RequestDeviceStatus status, wgpu::Device cDevice, wgpu::StringView message, void* userdata) {
            if (status == wgpu::RequestDeviceStatus::Success) {
                *static_cast<wgpu::Device*>(userdata) = cDevice;
            } else {
                std::cerr << "Failed to acquire Device: " << message.data << std::endl;
            }
        };

        wgpu::Future deviceFuture = adapter.RequestDevice(
            &deviceDesc,
            wgpu::CallbackMode::WaitAnyOnly,
            deviceCallback,
            &device
        );

        // Wait for the device request to resolve
        instance.WaitAny(deviceFuture, UINT64_MAX);

        if (!device) {
            std::cerr << "Failed to create WebGPU Device!" << std::endl;
            return false;
        }

        device.SetLoggingCallback(
            [](wgpu::LoggingType type, wgpu::StringView message, void* userdata) {
                // Access wgpu::StringView data (or message.data if StringView is a struct)
                std::string_view msgStr(message.data, message.length);
                std::cerr << "[Dawn Log " << static_cast<int>(type) << "]: " 
                        << msgStr << std::endl;
            },
            static_cast<void*>(nullptr) // Cast explicit void* to satisfy template
        );

        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        // 7. Configure the Surface (Tells Dawn how big the swapchain buffer should be)
        wgpu::SurfaceCapabilities capabilities{};
        surface.GetCapabilities(adapter, &capabilities);

        config = {
            .device = device,
            .format = capabilities.formats[0],
            .usage = wgpu::TextureUsage::RenderAttachment,
            .width = (uint32_t) w,
            .height = (uint32_t) h,
            .presentMode = wgpu::PresentMode::Fifo // Standard V-Sync
        };
        surface.Configure(&config);
        queue = device.GetQueue();

        return true;
    }

    void shutdown() {
    }

	void set_frame(AVFrame *frame, double play_time, AppSub sub) {
		if (this->frame)
			ff::frame_recycle(this->frame);
        this->frame = frame;
        create_texture(frame);
        prepare_texture_draw();
    }

    void render(AppSub sub) {
        if (!pipeline)
            return;

        push_uniforms();

        wgpu::SurfaceTexture surface_texture;
        surface.GetCurrentTexture(&surface_texture);

        wgpu::TextureViewDescriptor view_desc = {
            .format = config.format,
            .dimension = wgpu::TextureViewDimension::e2D,
            .mipLevelCount = WGPU_MIP_LEVEL_COUNT_UNDEFINED,
            .arrayLayerCount = WGPU_ARRAY_LAYER_COUNT_UNDEFINED,
            .aspect = wgpu::TextureAspect::All
        };
        auto texture_view = surface_texture.texture.CreateView(&view_desc);

        wgpu::RenderPassColorAttachment color_attachments = {
            .view = texture_view,
            .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
            .loadOp = wgpu::LoadOp::Clear,
            .storeOp = wgpu::StoreOp::Store,
//            .clearValue = clear_color,
        };
        wgpu::RenderPassDescriptor render_pass_desc = {
            .colorAttachmentCount = 1,
            .colorAttachments = &color_attachments,
            .depthStencilAttachment = nullptr
        };
        wgpu::CommandEncoderDescriptor enc_desc = {};

        if (is_hwframe()) {
            wgpu::SharedTextureMemoryVkImageLayoutBeginState vkLayoutBegin{};
            wgpu::SharedTextureMemoryBeginAccessDescriptor beginDesc{
                .nextInChain = &vkLayoutBegin,
                .initialized = true,
            };
            sharedMemory.BeginAccess(texture[0], &beginDesc);
        }

        auto encoder = device.CreateCommandEncoder(&enc_desc);
        auto pass = encoder.BeginRenderPass(&render_pass_desc);

        pass.SetPipeline(pipeline);
        pass.SetBindGroup(0, bind_group_0);
        pass.SetBindGroup(1, bind_group_1);
        pass.Draw(4);

        ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.Get());

        pass.End();

        wgpu::CommandBufferDescriptor cmd_buffer_desc = {};
        auto cmd_buffer = encoder.Finish(&cmd_buffer_desc);
        queue.Submit(1, &cmd_buffer);

        if (is_hwframe()) {
            wgpu::SharedTextureMemoryVkImageLayoutEndState vkLayoutEnd{};
            wgpu::SharedTextureMemoryEndAccessState endState{};
            endState.nextInChain = &vkLayoutEnd,
            sharedMemory.EndAccess(texture[0], &endState);
        }

        surface.Present();
        instance.ProcessEvents();
    }

	void window_size_changed(Sint32 w, Sint32 h) {
		wnd_w = w;
		wnd_h = h;
        config.width = (uint32_t) w;
        config.height = (uint32_t) h;
        surface.Configure(&config);
	}

	void reset_scale() {
		SDL_GetWindowSizeInPixels(window, &wnd_w, &wnd_h);
		base_scale = SDL_max((float) wnd_w / width, (float) wnd_h / height);
	}

    void transform_changed() {
        if (uniform_update == Clean)
            uniform_update = Transform;
    }

    void pan_x(float x) {
        video_pan_x += x;
        transform_changed();
    }

    void pan_y(float y) {
        video_pan_y += y;
        transform_changed();
    }

    void scale(float x) {
        video_scale += x;
        transform_changed();
    }

    void reset_transform() {
        video_pan_x = 0;
        video_pan_y = 0;
        video_scale = 1;
        transform_changed();
    }
};