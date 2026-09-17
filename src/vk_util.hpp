#pragma once

#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#include <vulkan/vulkan_raii.hpp>

vk::PhysicalDeviceMemoryProperties memProperties;
vk::SurfaceFormatKHR swapChainSurfaceFormat;
vk::Extent2D swapChainExtent;
uint32_t queueIndex     = ~0;

inline uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) {
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("failed to find suitable memory type!");
}
