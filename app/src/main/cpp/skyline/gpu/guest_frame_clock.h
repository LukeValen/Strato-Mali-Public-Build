// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace skyline::gpu {
    // Guest display intervals are measured in 60 Hz ticks, never host refreshes.
    class GuestFrameClock {
      public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;
        static constexpr std::chrono::nanoseconds VsyncPeriod{1'000'000'000 / 60};

        static constexpr std::chrono::nanoseconds Interval(std::uint32_t ticks) {
            return std::chrono::nanoseconds{1'000'000'000LL * ticks / 60};
        }

        TimePoint SchedulePresent(TimePoint now, std::uint32_t ticks, bool throttle) {
            if (!throttle || !ticks) {
                lastPresent.reset();
                return now;
            }
            // Keep absolute deadlines to avoid accumulating sleep jitter. A late
            // renderer starts a new cadence instead of bursting through missed frames.
            lastPresent = lastPresent ? std::max(now, *lastPresent + Interval(ticks)) : now;
            return *lastPresent;
        }

        static TimePoint NextVsync(TimePoint previous, TimePoint now) {
            auto next{previous + VsyncPeriod};
            return next > now ? next : now + VsyncPeriod;
        }

      private:
        std::optional<TimePoint> lastPresent;
    };
}
