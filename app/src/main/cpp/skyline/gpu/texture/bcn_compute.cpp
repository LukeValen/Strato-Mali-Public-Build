// SPDX-License-Identifier: MPL-2.0
#include <gpu.h>
#include <filesystem>
#include <fstream>
#include "bcn_compute.h"
#include "bcn_shaders.h"

namespace skyline::gpu {
    namespace {
        // A private cache: never mix guest graphics pipelines or another shader revision.
        constexpr u32 BcnCacheMagic{0x314e4342}; // "BCN1"
        constexpr size_t MaxBcnCacheBytes{16 * 1024 * 1024};
        struct CacheEnvelope {
            u32 magic;
            u32 size;
            u64 hash;
        };

        u64 CacheHash(span<const u8> bytes) {
            u64 hash{14695981039346656037ULL};
            for (const auto byte : bytes) hash = (hash ^ byte) * 1099511628211ULL;
            return hash;
        }

        std::filesystem::path CachePath(GPU &gpu) {
            const auto properties{gpu.vkPhysicalDevice.getProperties()};
            // The Vulkan UUID changes with incompatible driver pipeline binaries.
            std::string uuid;
            constexpr char hex[] = "0123456789abcdef";
            for (const auto byte : properties.pipelineCacheUUID) {
                uuid += hex[byte >> 4];
                uuid += hex[byte & 15];
            }
            return gpu.bcnPipelineCacheDir / ("bcn-v1-" + std::to_string(properties.vendorID) + "-" +
                std::to_string(properties.deviceID) + "-" + std::to_string(properties.driverVersion) + "-" + uuid);
        }

        void InitialiseCache(GPU &gpu) {
            if (gpu.bcnPipelineCacheInitialized || !gpu.bcnShaderCacheEnabled) return;
            gpu.bcnPipelineCacheInitialized = true; // Called while holding bcnComputeMutex.
            try {
                const auto path{CachePath(gpu)};
                std::vector<u8> bytes;
                std::error_code ec;
                const auto fileSize{std::filesystem::file_size(path, ec)};
                if (!ec && fileSize >= sizeof(CacheEnvelope) + sizeof(VkPipelineCacheHeaderVersionOne) &&
                    fileSize <= MaxBcnCacheBytes + sizeof(CacheEnvelope)) {
                    std::ifstream stream{path, std::ios::binary};
                    CacheEnvelope envelope{};
                    stream.read(reinterpret_cast<char *>(&envelope), sizeof(envelope));
                    if (stream && envelope.magic == BcnCacheMagic && envelope.size == fileSize - sizeof(envelope)) {
                        bytes.resize(envelope.size);
                        stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
                        if (!stream || CacheHash(bytes) != envelope.hash) bytes.clear();
                    }
                }
                if (!bytes.empty()) {
                    VkPipelineCacheHeaderVersionOne header{};
                    std::memcpy(&header, bytes.data(), sizeof(header));
                    const auto properties{gpu.vkPhysicalDevice.getProperties()};
                    if (header.headerSize != sizeof(header) || header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
                        header.vendorID != properties.vendorID || header.deviceID != properties.deviceID ||
                        std::memcmp(header.pipelineCacheUUID, properties.pipelineCacheUUID.data(), VK_UUID_SIZE))
                        bytes.clear();
                }
                gpu.bcnPipelineCache = gpu.vkDevice.createPipelineCache(vk::PipelineCacheCreateInfo{
                    .initialDataSize = bytes.size(), .pInitialData = bytes.data()});
                if (!bytes.empty()) LOGI("Loaded BCn compute pipeline cache ({} bytes)", bytes.size());
            } catch (const std::exception &error) {
                LOGW("BCn pipeline cache unavailable; compiling shaders normally: {}", error.what());
                gpu.bcnPipelineCache = vk::raii::PipelineCache{nullptr};
            }
        }

        void SaveCache(GPU &gpu) {
            if (*gpu.bcnPipelineCache == vk::PipelineCache{}) return;
            try {
                const auto bytes{gpu.bcnPipelineCache.getData()};
                if (bytes.empty() || bytes.size() > MaxBcnCacheBytes) return;
                std::filesystem::create_directories(gpu.bcnPipelineCacheDir);
                const auto path{CachePath(gpu)};
                auto temporary{path};
                temporary += ".tmp";
                const CacheEnvelope envelope{BcnCacheMagic, static_cast<u32>(bytes.size()), CacheHash(bytes)};
                {
                    std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
                    stream.write(reinterpret_cast<const char *>(&envelope), sizeof(envelope));
                    stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
                    stream.flush();
                    if (!stream) throw std::runtime_error("Failed to write BCn pipeline cache");
                }
                std::filesystem::rename(temporary, path);
            } catch (const std::exception &error) {
                LOGW("Could not save BCn pipeline cache: {}", error.what());
            }
        }
    }

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
            InitialiseCache(gpu);
            const vk::ComputePipelineCreateInfo info{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"}, .layout = *layout};
            if (*gpu.bcnPipelineCache != vk::PipelineCache{}) {
                try {
                    pipeline = gpu.vkDevice.createComputePipeline(gpu.bcnPipelineCache, info);
                } catch (const vk::DeviceLostError &) {
                    throw;
                } catch (const std::exception &error) {
                    LOGW("BCn cached pipeline rejected; retrying without cache: {}", error.what());
                    gpu.bcnPipelineCache = vk::raii::PipelineCache{nullptr};
                }
            }
            if (*pipeline == vk::Pipeline{}) pipeline = gpu.vkDevice.createComputePipeline(nullptr, info);
            SaveCache(gpu);
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
