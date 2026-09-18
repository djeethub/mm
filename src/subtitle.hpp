#pragma once

#include "vk_util.hpp"

#define N_INFLIGHT 3
#define N_MAX_SUBS 999

struct Vertex {
    float x, y;
    float w, h;
    float r, g, b, a;
    float u, v; // Bottom-Right UV
    float u1, v1; // padding
};

struct ImageData {
    vk::raii::Image image = nullptr;
    vk::raii::DeviceMemory memory = nullptr;
    vk::raii::ImageView imageView = nullptr;
    vk::raii::Buffer up_buffer = nullptr;
    vk::raii::DeviceMemory up_memory = nullptr;

    uint32_t w;
    uint32_t h;
    uint32_t alloc_w = 0;
    uint32_t alloc_h = 0;

    void init(const vk::raii::Device& device, uint32_t w, uint32_t h) {
        this->w = w;
        this->h = h;
        if (alloc_w >= w && alloc_h >= h)
            return;
        alloc_w = std::max(w, alloc_w) + 32;
        alloc_h = std::max(h, alloc_h) + 32;

        vk::ImageCreateInfo info = {
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8Unorm,
            .extent = { .width = (uint32_t) alloc_w, .height = (uint32_t) alloc_h, .depth = 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        image = device.createImage(info);
        auto req = image.getMemoryRequirements();
        vk::MemoryAllocateInfo mem_alloc_info = {};
        mem_alloc_info.allocationSize = req.size;
        mem_alloc_info.memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        memory = device.allocateMemory(mem_alloc_info);
        image.bindMemory(memory, 0);

        vk::ImageViewCreateInfo viewInfo{
            .image = image,     // The VkImage containing your uploaded AVFrame data
            .viewType = vk::ImageViewType::e2D,
            .format = info.format,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor, // Vulkan handles sub-planes internally
                .levelCount = 1,
                .layerCount = 1
            }
        };
        imageView = device.createImageView(viewInfo);
        
        vk::BufferCreateInfo buffer_info = {
            .size = alloc_w * alloc_h,
            .usage = vk::BufferUsageFlagBits::eTransferSrc,
            .sharingMode = vk::SharingMode::eExclusive
        };
        up_buffer = device.createBuffer(buffer_info);
        req = up_buffer.getMemoryRequirements();
        vk::MemoryAllocateInfo alloc_info = {
            .allocationSize = req.size,
            .memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)
        };
        up_memory = device.allocateMemory(alloc_info);
        up_buffer.bindMemory(up_memory, 0);
    }
};

struct DataSet {
    enum Status {
        None,
        Init,
        Upload,
        Ready,
        Discard
    };

    std::vector<ImageData> images;
    uint32_t n_images = 0;
    //vertex
    std::vector<Vertex> vertices;
    vk::raii::Buffer buffer = nullptr;
    vk::raii::DeviceMemory memory = nullptr;
    Uint32 alloc_size = 0;

    vk::raii::Fence copyFence = nullptr;
    vk::raii::DescriptorSet set = nullptr;
    vk::raii::CommandBuffer commandBuffer = nullptr;

    Status status = None;
    double play_time;

    void alloc_buf(const vk::raii::Device& device, uint32_t size) {
        if (alloc_size >= size)
            return;
        alloc_size = size * 2;
        vk::BufferCreateInfo buffer_info = {
            .size = alloc_size,
            .usage = vk::BufferUsageFlagBits::eStorageBuffer,
            .sharingMode = vk::SharingMode::eExclusive
        };
        buffer = device.createBuffer(buffer_info);
        auto req = buffer.getMemoryRequirements();
        vk::MemoryAllocateInfo alloc_info = {
            .allocationSize = req.size,
            .memoryTypeIndex = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)
        };
        memory = device.allocateMemory(alloc_info);
        buffer.bindMemory(memory, 0);
    }
};

class AppSubtitle {
protected:
    const vk::raii::Device& device;
    vk::raii::Sampler sampler = nullptr;
    vk::raii::DescriptorSetLayout layout = nullptr;
    vk::raii::DescriptorPool pool = nullptr;
    vk::raii::PipelineLayout pipelineLayout = nullptr;
    vk::raii::Pipeline pipeline = nullptr;
    vk::raii::CommandPool commandPool = nullptr;

    int wnd_w = 0;
    int wnd_h = 0;

public:
    AppSubtitle(const vk::raii::Device& gpu) : device(gpu) {}
};

class SubAss;
class SubBitmap;
using AppSub = std::variant<SubAss *>;//, SubBitmap *>;
