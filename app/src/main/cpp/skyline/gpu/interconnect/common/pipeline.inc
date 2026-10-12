// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <gpu/interconnect/command_executor.h>
#include "samplers.h"
#include "textures.h"

namespace skyline::gpu::interconnect {
    union BindlessHandle {
        u32 raw;

        struct {
            u32 textureIndex : 20;
            u32 samplerIndex : 12;
        };
    };

    static DynamicBufferBinding GetConstantBufferBinding(InterconnectContext &ctx,
                                                         span<const u32, Shader::Info::MAX_CBUFS> cbufSizes,
                                                         BufferView view, size_t idx,
                                                         vk::PipelineStageFlagBits dstStage,
                                                         vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        if (!view) // Return a dummy buffer if the constant buffer isn't bound
            return BufferBinding{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, PAGE_SIZE).buffer, 0, PAGE_SIZE};

        ctx.executor.AttachBuffer(view);
        view.GetBuffer()->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

        size_t sizeOverride{std::min<size_t>(cbufSizes[idx], view.size)};
        if (auto megaBufferBinding{view.TryMegaBuffer(ctx.executor.cycle, ctx.gpu.megaBufferAllocator, ctx.executor.executionTag, sizeOverride)}) {
            return megaBufferBinding;
        } else {
            view.GetBuffer()->BlockSequencedCpuBackingWrites();
            return view;
        }
    }

    static DynamicBufferBinding GetStorageBufferBinding(InterconnectContext &ctx, const auto &desc,
                                                        ConstantBuffer &cbuf, CachedMappedBufferView &cachedView,
                                                        vk::PipelineStageFlagBits dstStage,
                                                        vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        struct SsboDescriptor {
            u64 address;
            u32 size;
        };
        auto ssbo{cbuf.Read<SsboDescriptor>(ctx.executor, desc.cbuf_offset)};
        if (ssbo.size == 0)
            return BufferBinding{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, PAGE_SIZE).buffer, 0, PAGE_SIZE};

        size_t padding{ssbo.address & (ctx.gpu.traits.minimumStorageBufferAlignment - 1)};
        cachedView.Update(ctx, ssbo.address - padding, util::AlignUp(ssbo.size + padding, ctx.gpu.traits.minimumStorageBufferAlignment));
        if (!cachedView.view) // Return a dummy buffer if the SSBO isn't bound
            return BufferBinding{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, PAGE_SIZE).buffer, 0, PAGE_SIZE};

        auto view{cachedView.view};
        ctx.executor.AttachBuffer(view);
        view.GetBuffer()->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

        if (desc.is_written) {
            if (view.GetBuffer()->SequencedCpuBackingWritesBlocked()) {
                srcStageMask |= vk::PipelineStageFlagBits::eAllCommands;
                dstStageMask |= dstStage;
            }

            view.GetBuffer()->MarkGpuDirty(ctx.executor.usageTracker);
        } else {
            if (auto megaBufferBinding{view.TryMegaBuffer(ctx.executor.cycle, ctx.gpu.megaBufferAllocator, ctx.executor.executionTag)})
                return megaBufferBinding;
        }

        view.GetBuffer()->BlockSequencedCpuBackingWrites();

        return view;
    }

    static BindlessHandle ReadBindlessHandle(InterconnectContext &ctx, auto &constantBuffers, const auto &desc, size_t arrayIdx) {
        ConstantBuffer &primaryCbuf{constantBuffers[desc.cbuf_index]};
        if (!primaryCbuf.view) {
            if (ctx.gpu.kirbyDiagnosticsEnabled &&
                ctx.gpu.kirbyMissingCbufSamples.fetch_add(1, std::memory_order_relaxed) < 64) {
                LOGW("KIRBY_DIAG missing CBUF cbuf={} offset=0x{:X} element={} -> bindless=0",
                     static_cast<u32>(desc.cbuf_index), static_cast<u32>(desc.cbuf_offset),
                     static_cast<u32>(arrayIdx));
            }
            return { .raw = 0 };
        }

        size_t elemOffset{arrayIdx << desc.size_shift};
        size_t primaryCbufOffset{desc.cbuf_offset + elemOffset};
        u32 primaryVal{primaryCbuf.Read<u32>(ctx.executor, primaryCbufOffset)};
        u32 handleValue{primaryVal};

        if constexpr (requires { desc.has_secondary; }) {
            if (desc.has_secondary) {
                ConstantBuffer &secondaryCbuf{constantBuffers[desc.secondary_cbuf_index]};
                size_t secondaryCbufOffset{desc.secondary_cbuf_offset + elemOffset};
                u32 secondaryVal{secondaryCbuf.Read<u32>(ctx.executor, secondaryCbufOffset)};
                handleValue = (primaryVal << desc.shift_left) | (secondaryVal << desc.secondary_shift_left);
            }
        }

        if (ctx.gpu.kirbyDiagnosticsEnabled) {
            const u32 sample{ctx.gpu.kirbyHandleSamples.fetch_add(1, std::memory_order_relaxed)};
            if (sample < 48 || (sample % 512 == 0 && sample < 32768)) {
                LOGI("KIRBY_DIAG CBUF handle sample={} index={} offset=0x{:X} elem={} primary=0x{:08X} handle=0x{:08X}",
                     sample, static_cast<u32>(desc.cbuf_index), static_cast<u32>(primaryCbufOffset),
                     static_cast<u32>(arrayIdx), primaryVal, handleValue);
            }
        }
        return {.raw = handleValue};
    }

    static std::pair<vk::DescriptorImageInfo, TextureView *> GetTextureBinding(InterconnectContext &ctx, const auto &desc,
                                                                               Samplers &samplers, Textures &textures,
                                                                               BindlessHandle handle,
                                                                               vk::PipelineStageFlagBits dstStage,
                                                                               vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        auto sampler{samplers.GetSampler(ctx, handle.samplerIndex, handle.textureIndex)};
        auto texture{textures.GetTexture(ctx, handle.textureIndex, desc.type)};
        if (ctx.gpu.kirbyDiagnosticsEnabled) {
            const u32 sample{ctx.gpu.kirbyTextureSamples.fetch_add(1, std::memory_order_relaxed)};
            if (sample < 40 || (sample % 512 == 0 && sample < 32768)) {
                LOGI("KIRBY_DIAG sampled image sample={} handle=0x{:08X} tic={} sampler={} guest={} fmt={} size={}x{} usage={} layout={}",
                     sample, handle.raw, static_cast<u32>(handle.textureIndex),
                     static_cast<u32>(handle.samplerIndex), bool(texture->texture->guest),
                     static_cast<u32>(texture->texture->format->vkFormat),
                     texture->texture->dimensions.width, texture->texture->dimensions.height,
                     static_cast<u32>(static_cast<VkImageUsageFlags>(texture->texture->usage)),
                     static_cast<u32>(texture->texture->layout));
            }
        }
        ctx.executor.AttachTexture(texture);
        auto view{texture->GetView()};
        texture->texture->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

        return {
            vk::DescriptorImageInfo{
                .sampler = **sampler,
                .imageView = view,
                .imageLayout = texture->texture->layout
            },
            texture
        };
    }
    // Unlike sampled textures, storage images are descriptor-bound without a
    // sampler, must use an identity swizzle and require VK_IMAGE_USAGE_STORAGE.
    // The caller supplies the shader stage so both graphics and compute can
    // insert a read-after-write barrier before execution.
    static vk::DescriptorImageInfo GetStorageImageBinding(InterconnectContext &ctx, const auto &desc,
                                                           Textures &textures, BindlessHandle handle,
                                                           vk::PipelineStageFlagBits dstStage,
                                                           vk::PipelineStageFlags &srcStageMask,
                                                           vk::PipelineStageFlags &dstStageMask) {
        auto *guestView{textures.GetTexture(ctx, handle.textureIndex, desc.type)};
        const auto requestedFormat{StorageImageViewFormat(desc.format)};
        const auto &guestTexture{guestView->texture};

        const bool storageUsage{bool(guestTexture->usage & vk::ImageUsageFlagBits::eStorage)};
        const bool formatCompatible{!requestedFormat ||
            (guestTexture->format->IsCompatible(*requestedFormat) &&
             (guestTexture->format == requestedFormat ||
              bool(guestTexture->flags & vk::ImageCreateFlagBits::eMutableFormat)) &&
             ctx.gpu.SupportsStorageImageFormat(requestedFormat->vkFormat))};

        TextureView *view{};
        if (storageUsage && formatCompatible && guestTexture->layout == vk::ImageLayout::eGeneral) {
            // Storage image views cannot use the guest TIC's component swizzle.
            // Keep this identity view alive for the recorded submission.
            auto identityView{guestTexture->GetView(guestView->type, guestView->range,
                                                   requestedFormat, vk::ComponentMapping{})};
            ctx.executor.AttachDependency(identityView);
            view = identityView.get();
        } else {
            // Invalid/unmapped TICs must never become unbound Vulkan
            // descriptors. Use a storage-capable, dimension-matched dummy.
            // Keep diagnostics bounded: this runs in a hot draw/dispatch path.
            static std::atomic<u32> fallbackReports{};
            if (fallbackReports.fetch_add(1, std::memory_order_relaxed) < 12) {
                LOGW("Storage image fallback: TIC={} shaderFmt={} hostFmt={} storageUsage={} compatible={} layout={}",
                     static_cast<u32>(handle.textureIndex), static_cast<u32>(desc.format),
                     static_cast<u32>(guestTexture->format->vkFormat), storageUsage,
                     formatCompatible, static_cast<u32>(guestTexture->layout));
            }
            view = textures.GetNullStorageTexture(ctx, desc.type, desc.format);
        }

        ctx.executor.AttachTexture(view);
        view->texture->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

        // Store the write stage on the backing texture, so the next reader
        // (sampled or storage) can issue a precise cross-dispatch barrier.
        if (desc.is_written)
            view->texture->RecordStorageWrite(dstStage, ctx.executor.usageTracker);

        return vk::DescriptorImageInfo{
            .sampler = {},
            .imageView = view->GetView(true),
            .imageLayout = vk::ImageLayout::eGeneral,
        };
    }

}
