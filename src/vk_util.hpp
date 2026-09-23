#pragma once

#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#include <vulkan/vulkan_raii.hpp>

#ifdef __linux__
extern "C" {
#include <libavutil/hwcontext_drm.h>
}
#else
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#endif

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

#ifdef _WIN32
template <typename T>
struct D3Deleter {
    void operator()(T *t) const {
        t->Release();
    }
};

struct HandleDeleter {
    void operator()(HANDLE t) const {
        CloseHandle(t);
    }
};
#endif