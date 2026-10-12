// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <atomic>
#include <cstdint>

namespace skyline::gpu::resolution {
    // Limit additional scaled image memory, independent of the guest/native cache.
    // The reservation outlives command submission when its image is fence-owned.
    struct Reservation {
        static constexpr std::uint64_t Limit{128 * 1024 * 1024};
        std::atomic<std::uint64_t> &allocated;
        const std::uint64_t bytes;
        bool reserved{};

        Reservation(std::atomic<std::uint64_t> &allocated, std::uint64_t bytes)
            : allocated{allocated}, bytes{bytes} {
            auto used{allocated.load()};
            while (used <= Limit && bytes <= Limit - used) {
                if (allocated.compare_exchange_weak(used, used + bytes)) {
                    reserved = true;
                    break;
                }
            }
        }
        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;
        ~Reservation() { if (reserved) allocated.fetch_sub(bytes); }
    };
}
