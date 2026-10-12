// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "render_resolution_state.h"
#include <vector>
#include <utility>
#include <gpu/memory_manager.h>
#include "resolution_scaling.h"
#include "resolution_budget.h"

namespace skyline::gpu {
    // Persistent companion of a texture-cache entry. Guest memory stays native;
    // graphics descriptors may sample this image while its native version matches.
    struct ScaledRenderTarget {
        std::shared_ptr<resolution::Reservation> reservation;
        memory::Image image;
        vk::raii::ImageView view;
        vk::Extent2D extent;
        bool initialized{};
        std::uint64_t nativeVersion{~std::uint64_t{0}};
        std::vector<std::pair<vk::ImageViewCreateInfo, vk::raii::ImageView>> sampledViews;


        ScaledRenderTarget(std::shared_ptr<resolution::Reservation> reservation, memory::Image &&image, vk::raii::ImageView &&view, vk::Extent2D extent)
            : reservation{std::move(reservation)}, image{std::move(image)}, view{std::move(view)}, extent{extent} {}
    };

}
