#pragma once
#include <common.h>
#include <filesystem>
#include <gpu/memory_manager.h>
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
namespace skyline::gpu {
    struct BcnComputePipeline;
    struct GPU {
        vk::raii::Context context;
        vk::raii::Instance instance{nullptr};
        vk::raii::PhysicalDevice vkPhysicalDevice{nullptr};
        u32 vkQueueFamilyIndex{};
        vk::raii::Device vkDevice{nullptr};
        bool bcnShaderCacheEnabled{};
        std::mutex bcnComputeMutex;
        std::array<std::shared_ptr<BcnComputePipeline>, 3> bcnComputePipelines;
        vk::raii::PipelineCache bcnPipelineCache{nullptr};
        std::filesystem::path bcnPipelineCacheDir;
        bool bcnPipelineCacheInitialized{};
        memory::MemoryManager memory{*this};
        GPU();
    };
}
