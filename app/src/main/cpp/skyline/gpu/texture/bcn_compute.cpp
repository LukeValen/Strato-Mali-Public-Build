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
        std::shared_ptr<BcnComputePipeline> pipeline;
        vk::raii::DescriptorPool pool{nullptr};
        vk::raii::DescriptorSet set{nullptr};
        std::shared_ptr<memory::StagingBuffer> input;
        std::vector<BcnComputeLevel> levels;
        vk::Format format;
    };

    bool BcnComputeJob::Supports(vk::Format format) {
        // RGTC signed/one/two-channel output needs a separate packing path.
        return (format >= vk::Format::eBc1RgbaUnormBlock && format <= vk::Format::eBc3SrgbBlock) ||
               (format >= vk::Format::eBc6HUfloatBlock && format <= vk::Format::eBc7SrgbBlock);
    }

    BcnComputeJob::BcnComputeJob(GPU &gpu, vk::Format format, span<const u8> input,
                               const std::vector<BcnComputeLevel> &levels,
                               const std::shared_ptr<memory::StagingBuffer> &destination) : impl{std::make_unique<Impl>()} {
        if (!Supports(format) || levels.empty()) throw BcnComputeIneligible("Unsupported BCn compute upload");
        const auto limits{gpu.vkPhysicalDevice.getProperties().limits};
        const auto queues{gpu.vkPhysicalDevice.getQueueFamilyProperties()};
        if (!(queues[gpu.vkQueueFamilyIndex].queueFlags & vk::QueueFlagBits::eCompute))
            throw exception("BCn compute requires a compute-capable graphics queue");
        const u32 bytesPerPixel{format == vk::Format::eBc6HUfloatBlock || format == vk::Format::eBc6HSfloatBlock ? 8U : 4U};
        const u32 bytesPerBlock{format == vk::Format::eBc1RgbaUnormBlock || format == vk::Format::eBc1RgbaSrgbBlock ? 8U : 16U};
        if (!destination || !input.size() || input.size() > limits.maxStorageBufferRange ||
            destination->size() > limits.maxStorageBufferRange)
            throw BcnComputeIneligible("BCn upload exceeds a storage buffer range");
        // Validate every mip before allocating anything or recording commands.
        for (const auto &level : levels) {
            const u64 compressedSize{((u64(level.width) + 3) / 4) * ((u64(level.height) + 3) / 4) * level.depth * bytesPerBlock};
            const u64 decodedSize{u64(level.width) * level.height * level.depth * bytesPerPixel};
            if (!level.width || !level.height || !level.depth || level.inputSize != compressedSize || level.outputSize != decodedSize ||
                level.inputOffset > input.size() || level.inputSize > input.size() - level.inputOffset ||
                level.outputOffset > destination->size() || level.outputSize > destination->size() - level.outputOffset ||
                (level.inputOffset % 4) || (level.outputOffset % bytesPerPixel) ||
                (u64(level.width) + 7) / 8 > limits.maxComputeWorkGroupCount[0] ||
                (u64(level.height) + 7) / 8 > limits.maxComputeWorkGroupCount[1] || level.depth > limits.maxComputeWorkGroupCount[2] ||
                level.width > INT32_MAX || level.height > INT32_MAX || level.depth > INT32_MAX || decodedSize > INT32_MAX ||
                level.inputOffset / 4 > INT32_MAX || level.outputOffset / bytesPerPixel > INT32_MAX)
                throw BcnComputeIneligible("BCn texture exceeds compute decoder limits");
        }
        const unsigned index{format <= vk::Format::eBc3SrgbBlock ? 0U : format <= vk::Format::eBc6HSfloatBlock ? 1U : 2U};
        {
            std::scoped_lock lock{gpu.bcnComputeMutex};
            if (!gpu.bcnComputePipelines[index]) gpu.bcnComputePipelines[index] = std::make_shared<BcnComputePipeline>(gpu, index);
            impl->pipeline = gpu.bcnComputePipelines[index];
        }
        impl->format = format;
        const vk::DescriptorPoolSize poolSize{.type = vk::DescriptorType::eStorageBuffer, .descriptorCount = 2};
        impl->pool = gpu.vkDevice.createDescriptorPool(vk::DescriptorPoolCreateInfo{
            .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &poolSize});
        const vk::DescriptorSetLayout rawLayout{*impl->pipeline->setLayout};
        auto sets{gpu.vkDevice.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
            .descriptorPool = *impl->pool, .descriptorSetCount = 1, .pSetLayouts = &rawLayout})};
        impl->set = std::move(sets.front());
        impl->input = gpu.memory.AllocateStagingBuffer(input.size(), true);
        std::memcpy(impl->input->data(), input.data(), input.size());
        const auto flushed{vmaFlushAllocation(impl->input->vmaAllocator, impl->input->vmaAllocation, 0, VK_WHOLE_SIZE)};
        if (flushed != VK_SUCCESS) vk::throwResultException(vk::Result(flushed), "BCn input flush");
        const std::array infos{
            vk::DescriptorBufferInfo{.buffer = destination->vkBuffer, .range = destination->size()},
            vk::DescriptorBufferInfo{.buffer = impl->input->vkBuffer, .range = input.size()}};
        const std::array writes{
            vk::WriteDescriptorSet{.dstSet = *impl->set, .dstBinding = 0, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &infos[0]},
            vk::WriteDescriptorSet{.dstSet = *impl->set, .dstBinding = 1, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &infos[1]}};
        gpu.vkDevice.updateDescriptorSets(writes, {});
        impl->levels = levels;
    }
    BcnComputeJob::~BcnComputeJob() = default;
    void BcnComputeJob::Record(const vk::raii::CommandBuffer &commands) {
        commands.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eComputeShader, {},
            vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eHostWrite, .dstAccessMask = vk::AccessFlagBits::eShaderRead}, {}, {});
        commands.bindPipeline(vk::PipelineBindPoint::eCompute, *impl->pipeline->pipeline);
        commands.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *impl->pipeline->layout, 0, *impl->set, {});
        const u32 pixelSize{impl->format == vk::Format::eBc6HUfloatBlock || impl->format == vk::Format::eBc6HSfloatBlock ? 8U : 4U};
        for (const auto &d : impl->levels) {
            // Both offsets are element indices in uint32_t input/output buffers.
            // The output offset counts pixels; BC6 writes two words per pixel.
            const std::array<i32, 9> registers{static_cast<i32>(impl->format), i32(d.width), i32(d.height), i32(d.depth),
                i32(d.inputOffset / 4), 0, 0, i32(d.outputOffset / pixelSize), 0};
            commands.pushConstants<i32>(*impl->pipeline->layout, vk::ShaderStageFlagBits::eCompute, 0, registers);
            commands.dispatch((d.width + 7) / 8, (d.height + 7) / 8, d.depth);
        }
        commands.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer, {},
            vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eShaderWrite, .dstAccessMask = vk::AccessFlagBits::eTransferRead}, {}, {});
    }
}
