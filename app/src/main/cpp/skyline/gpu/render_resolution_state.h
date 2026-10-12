// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vulkan/vulkan_raii.hpp>

namespace skyline::gpu {
    // Command recording has its own thread. Cache unscaled dynamic state so a
    // native fallback can restore it even if the guest did not change it.
    struct RenderResolutionState {
        float x{1.0f}, y{1.0f};
        vk::Extent2D bounds{};
        std::array<std::optional<vk::Viewport>, 16> viewports{};
        std::array<std::optional<vk::Rect2D>, 16> scissors{};

        vk::Viewport Transform(vk::Viewport viewport) const {
            viewport.x *= x;
            viewport.y *= y;
            viewport.width *= x;
            viewport.height *= y; // Preserve negative height and depth range.
            return viewport;
        }

        vk::Rect2D Transform(vk::Rect2D rect) const {
            if (x == 1.0f && y == 1.0f)
                return rect;
            const auto left{std::clamp(std::floor(double(rect.offset.x) * x), 0.0, double(bounds.width))};
            const auto top{std::clamp(std::floor(double(rect.offset.y) * y), 0.0, double(bounds.height))};
            const auto right{std::clamp(std::ceil((double(rect.offset.x) + rect.extent.width) * x), left, double(bounds.width))};
            const auto bottom{std::clamp(std::ceil((double(rect.offset.y) + rect.extent.height) * y), top, double(bounds.height))};
            return {{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top)},
                    {static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top)}};
        }

        void Begin(vk::raii::CommandBuffer &commandBuffer, float scaleX, float scaleY, vk::Extent2D extent) {
            const bool changed{x != scaleX || y != scaleY ||
                ((scaleX != 1.0f || scaleY != 1.0f) && bounds != extent)};
            x = scaleX;
            y = scaleY;
            bounds = extent;
            if (!changed)
                return;
            for (std::uint32_t i{}; i < viewports.size(); i++) {
                if (viewports[i]) commandBuffer.setViewport(i, Transform(*viewports[i]));
                if (scissors[i]) commandBuffer.setScissor(i, Transform(*scissors[i]));
            }
        }
    };

    inline thread_local RenderResolutionState renderResolutionState;
}
