// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <memory>
#include <stdexcept>
#include <vector>
#include <gpu/memory_manager.h>

namespace skyline::gpu {
    struct BcnComputePipeline;
    struct BcnComputeLevel {
        u32 width, height, depth;
        size_t inputOffset, inputSize, outputOffset, outputSize;
    };
    // A single texture cannot use compute; other textures may still qualify.
    class BcnComputeIneligible : public std::runtime_error {
      public:
        using std::runtime_error::runtime_error;
    };
    // Lives with the upload staging buffer, which is retained by its FenceCycle.
    class BcnComputeJob {
        struct Impl;
        std::unique_ptr<Impl> impl;
      public:
        BcnComputeJob(GPU &gpu, vk::Format format, span<const u8> input,
                      const std::vector<BcnComputeLevel> &levels,
                      const std::shared_ptr<memory::StagingBuffer> &destination);
        ~BcnComputeJob();
        void Record(const vk::raii::CommandBuffer &commands);
        static bool Supports(vk::Format format);
    };
}
