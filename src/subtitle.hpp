#pragma once

#include "vk_util.hpp"

#define N_INFLIGHT 4

template <typename T>
class GPUPool {
public:
    std::vector<T *> in_use_list;
    std::vector<T *> list;

    void recycle(T *buf) {
        buf->reset();
        list.push_back(buf);
    }

    void in_use(T *buf) {
        in_use_list.push_back(buf);
    }

    void recycle() {
        while (!in_use_list.empty()) {
            auto data = in_use_list.back();
            data->reset();
            list.push_back(data);
            in_use_list.pop_back();
        }
    }

    void clear() {
        recycle();
        for (auto data : list) {
            delete data;
        }
        list.clear();
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
