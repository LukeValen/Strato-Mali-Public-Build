// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace skyline::gpu::resolution {
    // ABI of the pinned shader compiler's SPIR-V RescalingLayout. A bit is
    // assigned per descriptor (not per array element), across shader stages.
    struct alignas(16) ShaderScale {
        std::array<std::uint32_t, 4> textures{};
        alignas(16) std::array<std::uint32_t, 2> images{};
        float downFactor{1.0f};
        bool SetTexture(std::uint32_t index) {
            if (index >= textures.size() * 32) return false;
            textures[index / 32] |= std::uint32_t{1} << (index % 32);
            return true;
        }
    };
    static_assert(offsetof(ShaderScale, images) == 16);
    static_assert(offsetof(ShaderScale, downFactor) == 24);
    static_assert(sizeof(ShaderScale) == 32);
}
