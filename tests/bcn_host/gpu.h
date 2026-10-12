#pragma once
#include <common.h>
#include <gpu/memory_manager.h>
namespace skyline::gpu {
    struct BcnComputePipeline;
    struct GPU {
        vk::raii::Context context;
        vk::raii::Instance instance{nullptr};
        vk::raii::PhysicalDevice vkPhysicalDevice{nullptr};
        u32 vkQueueFamilyIndex{};
        vk::raii::Device vkDevice{nullptr};
        std::mutex bcnComputeMutex;
        std::array<std::shared_ptr<BcnComputePipeline>, 3> bcnComputePipelines;
        memory::MemoryManager memory{*this};
        GPU();
    };
}
