#pragma once
#include <common.h>
#include <memory>
namespace skyline::gpu { class GPU; class BcnComputeJob; }
inline VkResult vmaFlushAllocation(void*, void*, VkDeviceSize, VkDeviceSize) { return VK_SUCCESS; }
namespace skyline::gpu::memory {
    struct StagingBuffer {
        vk::raii::Buffer buffer{nullptr};
        vk::raii::DeviceMemory memory{nullptr};
        vk::Buffer vkBuffer;
        void *vmaAllocator{}, *vmaAllocation{};
        std::shared_ptr<BcnComputeJob> bcnComputeJob;
        u8 *mapped{};
        size_t length;
        u8 *data() { return mapped; }
        size_t size() { return length; }
        ~StagingBuffer() { if (mapped) memory.unmapMemory(); }
    };
    struct MemoryManager {
        GPU &gpu;
        std::shared_ptr<StagingBuffer> AllocateStagingBuffer(vk::DeviceSize size, bool storage = false);
    };
}
