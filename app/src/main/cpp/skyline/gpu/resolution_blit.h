// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <vulkan/vulkan_raii.hpp>

namespace skyline::gpu::resolution {
    inline bool NeedsNativeLoad(const vk::AttachmentDescription &attachment, vk::ImageAspectFlags aspects,
                                vk::Rect2D renderArea, vk::Extent2D nativeExtent) {
        // A clear/discard only replaces the render area. Preserve the remaining
        // pixels, and preserve stencil separately when only depth is cleared.
        return renderArea.offset.x != 0 || renderArea.offset.y != 0 ||
            renderArea.extent != nativeExtent ||
            attachment.loadOp == vk::AttachmentLoadOp::eLoad ||
            ((aspects & vk::ImageAspectFlagBits::eStencil) &&
             attachment.stencilLoadOp == vk::AttachmentLoadOp::eLoad);
    }

    // Shared by the renderer and the standalone Vulkan integration test.
    inline void Barrier(const vk::raii::CommandBuffer &commandBuffer, vk::Image image,
                        vk::ImageAspectFlags aspects, bool initialize = false) {
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
            vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, vk::ImageMemoryBarrier{
                .srcAccessMask = initialize ? vk::AccessFlags{} : vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = initialize ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = {aspects, 0, 1, 0, 1},
            });
    }

    inline void Blit(const vk::raii::CommandBuffer &commandBuffer, vk::Image source, vk::Extent2D sourceExtent,
                     vk::Image destination, vk::Extent2D destinationExtent,
                     vk::ImageAspectFlags aspects, vk::Filter filter) {
        // Depth/stencil aspects must have separate regions and use nearest.
        for (auto aspect : {vk::ImageAspectFlagBits::eColor, vk::ImageAspectFlagBits::eDepth, vk::ImageAspectFlagBits::eStencil}) {
            if (!(aspects & aspect)) continue;
            commandBuffer.blitImage(source, vk::ImageLayout::eGeneral, destination, vk::ImageLayout::eGeneral,
                vk::ImageBlit{
                    .srcSubresource = {aspect, 0, 0, 1},
                    .srcOffsets = std::array<vk::Offset3D, 2>{{{0, 0, 0}, {static_cast<int32_t>(sourceExtent.width), static_cast<int32_t>(sourceExtent.height), 1}}},
                    .dstSubresource = {aspect, 0, 0, 1},
                    .dstOffsets = std::array<vk::Offset3D, 2>{{{0, 0, 0}, {static_cast<int32_t>(destinationExtent.width), static_cast<int32_t>(destinationExtent.height), 1}}},
                }, aspect == vk::ImageAspectFlagBits::eColor ? filter : vk::Filter::eNearest);
        }
    }
}
