#pragma once

void check_vk_result(VkResult err);

class FrameData {
public:
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSamplerYcbcrConversion ycbcrConversion = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDeviceMemory upload_buffer_memory = VK_NULL_HANDLE;
    VkBuffer upload_buffer = VK_NULL_HANDLE;
    VkFence copyFence = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    VkDeviceSize upload_size;
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

    void init(AVFrame *frame, VkDevice device, VkAllocationCallbacks *allocator, uint32_t queueFamily) {
        reset(device, allocator);

        if (commandPool == VK_NULL_HANDLE) {
            // No special flags needed if we reset the entire pool
            VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            poolInfo.queueFamilyIndex = queueFamily;
            vkCreateCommandPool(device, &poolInfo, allocator, &commandPool);

            VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            allocInfo.commandPool = commandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);
        }

        VkDescriptorPoolSize pool_sizes[] =
        {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
//        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = (uint32_t)IM_COUNTOF(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        auto err = vkCreateDescriptorPool(device, &pool_info, allocator, &pool);
        check_vk_result(err);

        //Create the VkSamplerYcbcrConversion
        VkSamplerYcbcrConversionCreateInfo ycbcrInfo{};
        ycbcrInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
        ycbcrInfo.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM; // Match your NV12 layout
        ycbcrInfo.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709; // or BT601 depending on video
        ycbcrInfo.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW; // Video levels (16-235) or FULL (0-255)
        ycbcrInfo.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        ycbcrInfo.xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
        ycbcrInfo.yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
        ycbcrInfo.chromaFilter = VK_FILTER_LINEAR;

        err = vkCreateSamplerYcbcrConversion(device, &ycbcrInfo, nullptr, &ycbcrConversion);
        check_vk_result(err);

        //Create the Sampler pointing to the Conversion
        VkSamplerYcbcrConversionInfo samplerConversionInfo{};
        samplerConversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
        samplerConversionInfo.conversion = ycbcrConversion;

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.pNext = &samplerConversionInfo; // <-- Bind the conversion rules here
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        // Address modes must be CLAMP_TO_EDGE for YUV samplers
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        err = vkCreateSampler(device, &samplerInfo, nullptr, &sampler);
        check_vk_result(err);

        // Define the Descriptor Set Layout with an Immutable Sampler
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binding.pImmutableSamplers = &sampler; // <-- Baked directly into the layout binding!

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;

        err = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout);

        // Allocate a descriptor set from the pool
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = pool; // The pool we just created
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout; // Your predefined VkDescriptorSetLayout

        err = vkAllocateDescriptorSets(device, &allocInfo, &set);
        check_vk_result(err);

        // Create the Image
        VkFormat format;
        switch (frame->format) {
            case AV_PIX_FMT_NV12:
                format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
                break;
            default:
                return;
        }

        VkImageCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent.width = frame->width;
        info.extent.height = frame->height;
        info.extent.depth = 1;
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        err = vkCreateImage(device, &info, allocator, &image);
        check_vk_result(err);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, image, &req);
        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = req.size;
        alloc_info.memoryTypeIndex = req.memoryTypeBits;
        err = vkAllocateMemory(device, &alloc_info, allocator, &memory);
        check_vk_result(err);
        err = vkBindImageMemory(device, image, memory, 0);
        check_vk_result(err);

        // Create the VkImageView with Conversion Info
        VkSamplerYcbcrConversionInfo viewConversionInfo{};
        viewConversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
        viewConversionInfo.conversion = ycbcrConversion;

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.pNext = &viewConversionInfo; // <-- Crucial!
        viewInfo.image = image;     // The VkImage containing your uploaded AVFrame data
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; // Vulkan handles sub-planes internally
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        vkCreateImageView(device, &viewInfo, nullptr, &imageView);

        //Update the Descriptor Set
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = imageView;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.sampler = VK_NULL_HANDLE; // Ignored because we used an immutable sampler!

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = set;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);

        // Create the Upload Buffer:
        upload_size = get_upload_size();
        {
            VkBufferCreateInfo buffer_info = {};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size = upload_size;
            buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            err = vkCreateBuffer(device, &buffer_info, allocator, &upload_buffer);
            check_vk_result(err);
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(device, upload_buffer, &req);
            VkMemoryAllocateInfo alloc_info = {};
            alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc_info.allocationSize = req.size;
            alloc_info.memoryTypeIndex = req.memoryTypeBits | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            err = vkAllocateMemory(device, &alloc_info, allocator, &upload_buffer_memory);
            check_vk_result(err);
            err = vkBindBufferMemory(device, upload_buffer, upload_buffer_memory, 0);
            check_vk_result(err);
        }

        just_created = true;
    }

    void upload(AVFrame *frame, VkQueue queue, VkDevice device, VkAllocationCallbacks *allocator) {
		if (this->frame)
			ff::frame_recycle(this->frame);
		this->frame = frame;

        VkResult err;

        int offset[4]{};
        // Upload to Buffer:
        char* map = nullptr;
        err = vkMapMemory(device, upload_buffer_memory, 0, upload_size, 0, (void**)(&map));
        check_vk_result(err);
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
        vkUnmapMemory(device, upload_buffer_memory);

        // Start command buffer
        {
            err = vkResetCommandPool(device, commandPool, 0);
            check_vk_result(err);
            VkCommandBufferBeginInfo begin_info = {};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            err = vkBeginCommandBuffer(commandBuffer, &begin_info);
            check_vk_result(err);
        }

        // Copy to Image:
        {
            VkBufferMemoryBarrier upload_barrier[1] = {};
            upload_barrier[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            upload_barrier[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            upload_barrier[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            upload_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            upload_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            upload_barrier[0].buffer = upload_buffer;
            upload_barrier[0].offset = 0;
            upload_barrier[0].size = upload_size;

            VkImageMemoryBarrier copy_barrier[1] = {};
            copy_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            copy_barrier[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            copy_barrier[0].oldLayout = just_created ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            copy_barrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            copy_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            copy_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            copy_barrier[0].image = image;
            copy_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy_barrier[0].subresourceRange.levelCount = 1;
            copy_barrier[0].subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, upload_barrier, 1, copy_barrier);

            VkBufferImageCopy region = {};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = width;
            region.imageExtent.height = height;
            region.imageExtent.depth = 1;
            region.imageOffset.x = 0;
            region.imageOffset.y = 0;
            vkCmdCopyBufferToImage(commandBuffer, upload_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            VkImageMemoryBarrier use_barrier[1] = {};
            use_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            use_barrier[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            use_barrier[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            use_barrier[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            use_barrier[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            use_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            use_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            use_barrier[0].image = image;
            use_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            use_barrier[0].subresourceRange.levelCount = 1;
            use_barrier[0].subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, use_barrier);
        }

        // End command buffer
        {
            VkSubmitInfo end_info = {};
            end_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            end_info.commandBufferCount = 1;
            end_info.pCommandBuffers = &commandBuffer;
            auto err = vkEndCommandBuffer(commandBuffer);
            check_vk_result(err);
            err = vkQueueSubmit(queue, 1, &end_info, VK_NULL_HANDLE);
            check_vk_result(err);
        }

        err = vkQueueWaitIdle(queue); // FIXME-OPT: Suboptimal!
        check_vk_result(err);

        just_created = false;
    }

    void reset(VkDevice device, VkAllocationCallbacks *allocator) {
        if (pool) {
            vkDestroyDescriptorPool(device, pool, allocator);
            set = VK_NULL_HANDLE;
            pool = VK_NULL_HANDLE;
        }
        if (imageView) {
            vkDestroyImageView(device, imageView, allocator);
            imageView = VK_NULL_HANDLE;
        }
        if (image) {
            vkDestroyImage(device, image, allocator);
            image = VK_NULL_HANDLE;
        }
        if (memory) {
            vkFreeMemory(device, memory, allocator);
            memory = VK_NULL_HANDLE;
        }
        if (upload_buffer_memory) {
            vkFreeMemory(device, upload_buffer_memory, allocator);
            memory = VK_NULL_HANDLE;
        }
        if (upload_buffer) {
            vkDestroyBuffer(device, upload_buffer, allocator);
            upload_buffer = VK_NULL_HANDLE;
        }
        if (sampler) {
            vkDestroySampler(device, sampler, allocator);
            sampler = VK_NULL_HANDLE;
        }
        if (layout) {
            vkDestroyDescriptorSetLayout(device, layout, allocator);
            layout = VK_NULL_HANDLE;
        }
        if (ycbcrConversion) {
            vkDestroySamplerYcbcrConversion(device, ycbcrConversion, allocator);
            ycbcrConversion = VK_NULL_HANDLE;
        }
    }

    void destroy(VkDevice device, VkAllocationCallbacks *allocator) {
        reset(device, allocator);
        if (commandPool) {
            vkDestroyCommandPool(device, commandPool, allocator);
            commandPool = VK_NULL_HANDLE;
            commandBuffer = VK_NULL_HANDLE;
        }
    }
};
