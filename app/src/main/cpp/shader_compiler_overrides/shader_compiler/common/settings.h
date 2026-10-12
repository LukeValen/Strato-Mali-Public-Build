// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <atomic>
#include <shader_compiler/common/common_types.h>

namespace Shader::Settings {
    /**
     * @note Only contains the settings relevant to the shader compiler
     */
    struct Values {
        bool renderer_debug;
        bool disable_shader_loop_safety_checks;
        struct ResolutionScalingInfo {
            u32 up_scale{1};
            u32 down_shift{0};
            f32 up_factor{1.0f};
            f32 down_factor{1.0f};
            bool active{};
            bool downscale{};
        } resolution_info;
    };

    // One program-wide object: renderer, translation and optimization must agree.
    inline Values values{}; //!< A static structure with the values set by Skyline code

    // The compiler includes this local override before submodule headers.
    // Shared by the renderer and translator; disabled outside Kirby.
    inline std::atomic<bool> kirby_packed_wscale_test{false};
    // Game-specific compatibility setting. Never enabled for other titles.
    inline std::atomic<bool> nier_mali_geometry_invocation_fix{false};
    // A/B experiment: accurate Maxwell ISBERD translation for NieR + Mali.
    inline std::atomic<bool> nier_mali_isberd_ab1{false};
}
