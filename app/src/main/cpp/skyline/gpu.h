// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <adrenotools/driver.h>
#include <atomic>
#include <unordered_map>
#include "gpu/trait_manager.h"
#include "gpu/resolution_scaling.h"
#include "gpu/memory_manager.h"
#include "gpu/command_scheduler.h"
#include "gpu/presentation_engine.h"
#include "gpu/texture_manager.h"
#include "gpu/buffer_manager.h"
#include "gpu/megabuffer.h"
#include "gpu/descriptor_allocator.h"
#include "gpu/shader_manager.h"
#include "gpu/pipeline_cache_manager.h"
#include "gpu/graphics_pipeline_assembler.h"
#include "gpu/shaders/helper_shaders.h"
#include "gpu/cache/renderpass_cache.h"
#include "gpu/cache/framebuffer_cache.h"
#include "gpu/interconnect/maxwell_3d/pipeline_manager.h"
#include "gpu/interconnect/kepler_compute/pipeline_manager.h"

namespace skyline::gpu {
    static constexpr u32 VkApiVersion{VK_API_VERSION_1_1}; //!< The version of core Vulkan that we require

    /**
     * @brief An interface to host GPU structures, anything concerning host GPU/Presentation APIs is encapsulated by this
     */
    class GPU {
      private:
        const DeviceState &state; // We access the device state inside Texture (and Buffers) for setting up NCE memory tracking
        friend Texture;
        friend Buffer;
        friend BufferManager;

      public:
        adrenotools_gpu_mapping adrenotoolsImportMapping{}; //!< Persistent struct to store active adrenotools mapping import info
        vk::raii::Context vkContext;
        vk::raii::Instance vkInstance;
        vk::raii::DebugReportCallbackEXT vkDebugReportCallback; //!< An RAII Vulkan debug report manager which calls into 'GPU::DebugCallback'
        vk::raii::PhysicalDevice vkPhysicalDevice;
        u32 vkQueueFamilyIndex{};
        TraitManager traits;
        vk::raii::Device vkDevice;
        std::mutex queueMutex; //!< Synchronizes access to the queue as it is externally synchronized
        vk::raii::Queue vkQueue; //!< A Vulkan Queue supporting graphics and compute operations

        // Most Switch games use sampled/attachment images only. Adding STORAGE
        // to every guest texture disables important Mali tiling/compression
        // optimizations. Enable it only after encountering a shader that needs
        // storage images, and cache host-format support per GPU session.
        // Diagnostic-only instrumentation, gated by the exact Kirby title ID.
        // No runtime logging or shader changes for other titles.
        // NieR:Automata Mali diagnostic session. No changes to other games.
        bool nierDiagnosticsEnabled{};
        std::atomic<u32> nierShaderSamples{};
        std::atomic<u32> nierBcDecodeSamples{};
        // Crash breadcrumbs, touched only by the NieR command-recording thread.
        // A failing Mali vkCmd* may terminate before we can log the call.
        std::atomic<u32> nierRecordPhase{};      // 1=state, 2=draw, 3=post-draw
        std::atomic<u32> nierStateCommandIndex{}; // 1-based index within the draw
        std::atomic<u32> nierStateCommandKind{};  // 1=pipeline future, 2=pipeline, 3=descriptors, 4=vertex/index, 0=other
        bool kirbyDiagnosticsEnabled{};
        std::atomic<u32> kirbyHandleSamples{};
        std::atomic<u32> kirbyMissingCbufSamples{};
        std::atomic<u32> kirbyTextureSamples{};
        std::atomic<u32> kirbyRenderTargetSamples{};
        std::atomic<u32> kirbyClearSamples{};
        std::atomic<u32> kirbySlowPipelineSamples{};

        std::atomic<bool> storageImageShadersSeen{false};
        std::mutex storageFormatCacheMutex;
        std::unordered_map<u32, bool> storageFormatSupportCache;

        bool SupportsStorageImageFormat(vk::Format format) {
            std::lock_guard lock{storageFormatCacheMutex};
            const auto key{static_cast<u32>(format)};
            if (auto it{storageFormatSupportCache.find(key)}; it != storageFormatSupportCache.end())
                return it->second;
            const bool supported{bool(vkPhysicalDevice.getFormatProperties(format).optimalTilingFeatures &
                                      vk::FormatFeatureFlagBits::eStorageImage)};
            storageFormatSupportCache.emplace(key, supported);
            return supported;
        }

        const resolution::Scale resolutionScale; //!< Fixed for this emulation session
        std::atomic<bool> resolutionQueriesSeen{}; //!< Precise pixel counters require native rasterization
        bool resolutionAllocationFailed{}; //!< Stop optional allocations after an allocation failure
        std::array<u64, 5> resolutionPassCounts{};
        u64 resolutionAttachmentCount{};
        std::atomic<u64> resolutionAllocatedBytes{};

        memory::MemoryManager memory;
        CommandScheduler scheduler;
        PresentationEngine presentation;

        TextureManager texture;
        BufferManager buffer;
        MegaBufferAllocator megaBufferAllocator;

        DescriptorAllocator descriptor;
        std::optional<ShaderManager> shader;

        HelperShaders helperShaders;

        std::optional<GraphicsPipelineAssembler> graphicsPipelineAssembler;
        cache::RenderPassCache renderPassCache;
        cache::FramebufferCache framebufferCache;

        std::mutex channelLock;
        std::optional<PipelineCacheManager> graphicsPipelineCacheManager;
        std::optional<interconnect::maxwell3d::PipelineManager> graphicsPipelineManager;
        interconnect::kepler_compute::PipelineManager computePipelineManager;

        static constexpr size_t DebugTracingBufferSize{0x80000}; //!< 512KiB
        memory::Buffer debugTracingBuffer; //!< General use buffer for debug tracing, first 4 bytes are allocated for checkpoints

        GPU(const DeviceState &state);

        /**
         * @brief Should be called after loader population to initialize the per-title caches
         */
        void Initialise();
    };
}

