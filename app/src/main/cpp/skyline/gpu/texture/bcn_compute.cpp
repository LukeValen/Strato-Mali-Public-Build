// SPDX-License-Identifier: MPL-2.0
#include <gpu.h>
#include "bcn_compute.h"
#include "bcn_shaders.h"

namespace skyline::gpu {
    struct BcnComputePipeline {
        vk::raii::DescriptorSetLayout setLayout{nullptr};
        vk::raii::PipelineLayout layout{nullptr};
        vk::raii::Pipeline pipeline{nullptr};
        BcnComputePipeline(GPU &gpu, unsigned index) {
            const std::array bindings{
                vk::DescriptorSetLayoutBinding{.binding = 0, .descriptorType = vk::DescriptorType::eStorageBuffer,
                    .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
                vk::DescriptorSetLayoutBinding{.binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
                    .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute}};
            setLayout = gpu.vkDevice.createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo{
                .bindingCount = 2, .pBindings = bindings.data()});
            const vk::DescriptorSetLayout rawLayout{*setLayout};
            const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute, .size = 36};
            layout = gpu.vkDevice.createPipelineLayout(vk::PipelineLayoutCreateInfo{
                .setLayoutCount = 1, .pSetLayouts = &rawLayout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range});
            const u32 *code{index == 0 ? bcn_shaders::s3tc : index == 1 ? bcn_shaders::bc6 : bcn_shaders::bc7};
            const size_t codeSize{index == 0 ? sizeof(bcn_shaders::s3tc) : index == 1 ? sizeof(bcn_shaders::bc6) : sizeof(bcn_shaders::bc7)};
            auto module{gpu.vkDevice.createShaderModule(vk::ShaderModuleCreateInfo{.codeSize = codeSize, .pCode = code})};
            pipeline = gpu.vkDevice.createComputePipeline(nullptr, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"}, .layout = *layout});
        }
    };

    struct BcnComputeJob::Impl {
        struct Level {
            BcnComputeLevel dimensions;
            std::shared_ptr<memory::StagingBuffer> input, output;
        };
        std::shared_ptr<BcnComputePipeline> pipeline;
        vk::raii::DescriptorPool pool{nullptr};
        std::vector<vk::raii::DescriptorSet> sets;
        std::vector<Level> levels;
        vk::Format format;
    };

    bool BcnComputeJob::Supports(vk::Format format) {
        // RGTC signed/one/two-channel output needs a separate packing path.
        return (format >= vk::Format::eBc1RgbaUnormBlock && format <= vk::Format::eBc3SrgbBlock) ||
               (format >= vk::Format::eBc6HUfloatBlock && format <= vk::Format::eBc7SrgbBlock);
    }

    BcnComputeJob::BcnComputeJob(GPU &gpu, vk::Format format, span<const u8> input,
                               const std::vector<BcnComputeLevel> &levels) : impl{std::make_unique<Impl>()} {
        if (!Supports(format) || levels.empty()) throw exception("Unsupported BCn compute upload");
        const auto limits{gpu.vkPhysicalDevice.getProperties().limits};
        const auto queues{gpu.vkPhysicalDevice.getQueueFamilyProperties()};
        if (!(queues[gpu.vkQueueFamilyIndex].queueFlags & vk::QueueFlagBits::eCompute))
            throw exception("BCn compute requires a compute-capable graphics queue");
        const u32 bytesPerPixel{format == vk::Format::eBc6HUfloatBlock || format == vk::Format::eBc6HSfloatBlock ? 8U : 4U};
        const u32 bytesPerBlock{format == vk::Format::eBc1RgbaUnormBlock || format == vk::Format::eBc1RgbaSrgbBlock ? 8U : 16U};
        // Validate every mip before allocating anything or recording commands.
        for (const auto &level : levels) {
            const u64 compressedSize{((u64(level.width) + 3) / 4) * ((u64(level.height) + 3) / 4) * level.depth * bytesPerBlock};
            const u64 decodedSize{u64(level.width) * level.height * level.depth * bytesPerPixel};
            if (!level.width || !level.height || !level.depth || level.inputSize != compressedSize || level.outputSize != decodedSize ||
                level.inputOffset > input.size() || level.inputSize > input.size() - level.inputOffset ||
                level.inputSize > limits.maxStorageBufferRange || level.outputSize > limits.maxStorageBufferRange ||
                (u64(level.width) + 7) / 8 > limits.maxComputeWorkGroupCount[0] ||
                (u64(level.height) + 7) / 8 > limits.maxComputeWorkGroupCount[1] || level.depth > limits.maxComputeWorkGroupCount[2] ||
                level.width > INT32_MAX || level.height > INT32_MAX || level.depth > INT32_MAX || decodedSize > INT32_MAX)
                throw exception("BCn texture exceeds compute decoder limits");
        }
        const unsigned index{format <= vk::Format::eBc3SrgbBlock ? 0U : format <= vk::Format::eBc6HSfloatBlock ? 1U : 2U};
        {
            std::scoped_lock lock{gpu.bcnComputeMutex};
            if (!gpu.bcnComputePipelines[index]) gpu.bcnComputePipelines[index] = std::make_shared<BcnComputePipeline>(gpu, index);
            impl->pipeline = gpu.bcnComputePipelines[index];
        }
        impl->format = format;
        const vk::DescriptorPoolSize poolSize{.type = vk::DescriptorType::eStorageBuffer, .descriptorCount = u32(levels.size() * 2)};
        impl->pool = gpu.vkDevice.createDescriptorPool(vk::DescriptorPoolCreateInfo{
            .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, .maxSets = u32(levels.size()), .poolSizeCount = 1, .pPoolSizes = &poolSize});
        std::vector<vk::DescriptorSetLayout> layouts(levels.size(), *impl->pipeline->setLayout);
        impl->sets = gpu.vkDevice.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
            .descriptorPool = *impl->pool, .descriptorSetCount = u32(layouts.size()), .pSetLayouts = layouts.data()});
        for (size_t i{}; i < levels.size(); ++i) {
            const auto &level{levels[i]};
            Impl::Level resources{level, gpu.memory.AllocateStagingBuffer(level.inputSize, true), gpu.memory.AllocateStagingBuffer(level.outputSize, true)};
            std::memcpy(resources.input->data(), input.data() + level.inputOffset, level.inputSize);
            const auto flushed{vmaFlushAllocation(resources.input->vmaAllocator, resources.input->vmaAllocation, 0, VK_WHOLE_SIZE)};
            if (flushed != VK_SUCCESS) vk::throwResultException(vk::Result(flushed), "BCn input flush");
            const std::array infos{
                vk::DescriptorBufferInfo{.buffer = resources.output->vkBuffer, .range = level.outputSize},
                vk::DescriptorBufferInfo{.buffer = resources.input->vkBuffer, .range = level.inputSize}};
            const std::array writes{
                vk::WriteDescriptorSet{.dstSet = *impl->sets[i], .dstBinding = 0, .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &infos[0]},
                vk::WriteDescriptorSet{.dstSet = *impl->sets[i], .dstBinding = 1, .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &infos[1]}};
            gpu.vkDevice.updateDescriptorSets(writes, {});
            impl->levels.push_back(std::move(resources));
        }
    }
    BcnComputeJob::~BcnComputeJob() = default;
    void BcnComputeJob::Record(const vk::raii::CommandBuffer &commands, vk::Buffer destination) {
        commands.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eComputeShader, {},
            vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eHostWrite, .dstAccessMask = vk::AccessFlagBits::eShaderRead}, {}, {});
        commands.bindPipeline(vk::PipelineBindPoint::eCompute, *impl->pipeline->pipeline);
        for (size_t i{}; i < impl->levels.size(); ++i) {
            const auto &level{impl->levels[i]};
            const auto &d{level.dimensions};
            const std::array<i32, 9> registers{static_cast<i32>(impl->format), i32(d.width), i32(d.height), i32(d.depth), 0, 0, 0, 0, 0};
            commands.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *impl->pipeline->layout, 0, *impl->sets[i], {});
            commands.pushConstants<i32>(*impl->pipeline->layout, vk::ShaderStageFlagBits::eCompute, 0, registers);
            commands.dispatch((d.width + 7) / 8, (d.height + 7) / 8, d.depth);
        }
        commands.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer, {},
            vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eShaderWrite, .dstAccessMask = vk::AccessFlagBits::eTransferRead}, {}, {});
        for (const auto &level : impl->levels)
            commands.copyBuffer(level.output->vkBuffer, destination, vk::BufferCopy{.dstOffset = level.dimensions.outputOffset, .size = level.dimensions.outputSize});
        commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {},
            vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite, .dstAccessMask = vk::AccessFlagBits::eTransferRead}, {}, {});
    }
}
