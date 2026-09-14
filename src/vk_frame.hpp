#pragma once

void check_vk_result(VkResult err);

struct FrameData {
    vk::raii::Image image = nullptr;
    vk::raii::DeviceMemory memory = nullptr;
    vk::raii::ImageView imageView = nullptr;
    vk::raii::DescriptorSet set = nullptr;
    vk::raii::DescriptorPool pool = nullptr;
    vk::raii::SamplerYcbcrConversion ycbcrConversion = nullptr;
    vk::raii::DescriptorSetLayout layout = nullptr;
    vk::raii::Sampler sampler = nullptr;
    vk::raii::DeviceMemory upload_buffer_memory = nullptr;
    vk::raii::Buffer upload_buffer = nullptr;
    vk::raii::Fence copyFence = nullptr;
    vk::raii::CommandPool commandPool = nullptr;
    vk::raii::CommandBuffer commandBuffer = nullptr;

    vk::DeviceSize upload_size;
    bool just_created = true;
    AVFrame *frame;

    inline static AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    static const AVPixFmtDescriptor *fmt_desc;
    static int n_planes;
    static int bpp;
	static int width;
	static int height;

    static bool init_static(AVFrame *frame) {
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

        return true;
    }

    int get_upload_size() {
        if (fmt_desc->flags & AV_PIX_FMT_FLAG_RGB) {
            return width * height * bpp;
        } else {
            return (width * height * bpp * (fmt_desc->flags & AV_PIX_FMT_FLAG_ALPHA ? 2 : 1)) + ((width >> fmt_desc->log2_chroma_w) * (height >> fmt_desc->log2_chroma_h) * bpp * 2);
        }
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

        vk::DescriptorPoolSize pool_sizes[] =
        {
            { vk::DescriptorType::eCombinedImageSampler, 1 },
            { vk::DescriptorType::eUniformBuffer, 1 },
        };
        vk::DescriptorPoolCreateInfo pool_info{};
//        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = (uint32_t)IM_COUNTOF(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        pool = device.createDescriptorPool(pool_info);

        //Create the VkSamplerYcbcrConversion
        vk::SamplerYcbcrConversionCreateInfo ycbcrInfo{};
        ycbcrInfo.format = vk::Format::eG8B8R82Plane420Unorm; // Match your NV12 layout
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
        vk::DescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        binding.descriptorCount = 1;
        binding.stageFlags = vk::ShaderStageFlagBits::eFragment;
        binding.pImmutableSamplers = &*sampler; // <-- Baked directly into the layout binding!

        vk::DescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        layout = device.createDescriptorSetLayout(layoutInfo);

        // Allocate a descriptor set from the pool
        vk::DescriptorSetAllocateInfo alloc_info{};
        alloc_info.descriptorPool = pool; // The pool we just created
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &*layout; // Your predefined VkDescriptorSetLayout
        set = std::move(device.allocateDescriptorSets(alloc_info)[0]);

        // Create the Image
        vk::Format format;
        switch (frame->format) {
            case AV_PIX_FMT_NV12:
                format = vk::Format::eG8B8R82Plane420Unorm;
                break;
            default:
                return;
        }

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
        vk::SamplerYcbcrConversionInfo viewConversionInfo{};
        viewConversionInfo.conversion = ycbcrConversion;

        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.pNext = &viewConversionInfo; // <-- Crucial!
        viewInfo.image = image;     // The VkImage containing your uploaded AVFrame data
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor; // Vulkan handles sub-planes internally
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        imageView = device.createImageView(viewInfo);

        //Update the Descriptor Set
        vk::DescriptorImageInfo imageInfo{};
        imageInfo.imageView = imageView;
        imageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
//        imageInfo.sampler = VK_NULL_HANDLE; // Ignored because we used an immutable sampler!

        vk::WriteDescriptorSet descriptorWrite{};
        descriptorWrite.dstSet = set;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;
        device.updateDescriptorSets(descriptorWrite, nullptr);

        // Create the Upload Buffer:
        upload_size = get_upload_size();
        {
            vk::BufferCreateInfo buffer_info = {};
            buffer_info.size = upload_size;
            buffer_info.usage = vk::BufferUsageFlagBits::eTransferSrc;
            buffer_info.sharingMode = vk::SharingMode::eExclusive;
            upload_buffer = device.createBuffer(buffer_info);
            auto req = upload_buffer.getMemoryRequirements();
            vk::MemoryAllocateInfo alloc_info = {};
            alloc_info.allocationSize = req.size;
            alloc_info.memoryTypeIndex = req.memoryTypeBits;// | vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
            upload_buffer_memory = device.allocateMemory(alloc_info);
            upload_buffer.bindMemory(upload_buffer_memory, 0);
        }

        just_created = true;
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
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .subresourceRange = {
                    .aspectMask = vk::ImageAspectFlags::BitsType::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
            };
            commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, bufferBarrier, imageBarrier);

            vk::BufferImageCopy2 region = {};
            region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = width;
            region.imageExtent.height = height;
            region.imageExtent.depth = 1;
            region.imageOffset.x = 0;
            region.imageOffset.y = 0;
            vk::CopyBufferToImageInfo2 copy_info = {
                .srcBuffer = upload_buffer,
                .dstImage = image,
                .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
                .regionCount = 1,
                .pRegions = &region,
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
            queue.submit(end_info);
        }

        just_created = false;
        queue.waitIdle();
    }
};
