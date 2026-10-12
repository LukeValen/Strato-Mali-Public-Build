// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <memory>
#include <vector>
#include <gpu/memory_manager.h>

namespace skyline::gpu {
    struct BcnComputePipeline;
    struct BcnComputeLevel {
        u32 width, height, depth;
        size_t inputOffset, inputSize, outputOffset, outputSize;
    };
    // Lives with the upload staging buffer, which is retained by its FenceCycle.
    class BcnComputeJob {
        struct Impl;
        std::unique_ptr<Impl> impl;
      public:
        BcnComputeJob(GPU &gpu, vk::Format format, span<const u8> input,
                      const std::vector<BcnComputeLevel> &levels);
        ~BcnComputeJob();
        void Record(const vk::raii::CommandBuffer &commands, vk::Buffer destination);
        static bool Supports(vk::Format format);
    };
}
