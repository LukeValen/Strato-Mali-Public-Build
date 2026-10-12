// SPDX-License-Identifier: MPL-2.0
#include "app/src/main/cpp/skyline/gpu/guest_frame_clock.h"
#include <cassert>
#include <iostream>
#include <limits>

using skyline::gpu::GuestFrameClock;
using namespace std::chrono_literals;
using Time = GuestFrameClock::TimePoint;

int main() {
    // Producers can run at any host refresh. Guest intervals remain 60/N FPS.
    for (int hostHz : {60, 90, 120, 144, 165, 240}) {
        for (unsigned interval : {1, 2, 3, 4}) {
            GuestFrameClock clock;
            Time now{10s};
            const auto start = now;
            assert(clock.SchedulePresent(now, interval, true) == now);
            for (int frame = 1; frame <= 600; ++frame) {
                // Model a buffer ready at each host tick, plus scheduling jitter.
                now += std::chrono::nanoseconds(1'000'000'000 / hostHz);
                now = clock.SchedulePresent(now, interval, true);
                assert(now - start >= GuestFrameClock::Interval(interval) * frame);
            }
        }
    }
    assert(GuestFrameClock::Interval(1) == 16'666'666ns);
    assert(GuestFrameClock::Interval(2) == 33'333'333ns);
    assert(GuestFrameClock::Interval(3) == 50ms);

    GuestFrameClock clock;
    Time start{10s};
    auto deadline = clock.SchedulePresent(start, 1, true);
    for (int frame = 1; frame <= 600; ++frame) {
        deadline = clock.SchedulePresent(deadline + 500us, 1, true);
        // Sub-period wakeup jitter must not lower the rate over time.
        assert(deadline == start + GuestFrameClock::Interval(1) * frame);
    }
    auto late = deadline + 5s;
    assert(clock.SchedulePresent(late, 2, true) == late);
    assert(clock.SchedulePresent(late, 2, true) == late + GuestFrameClock::Interval(2));
    // The title may change its interval for menus, gameplay or video.
    auto switched = clock.SchedulePresent(late, 1, true);
    assert(switched == late + GuestFrameClock::Interval(2) + GuestFrameClock::Interval(1));
    assert(clock.SchedulePresent(late, 1, false) == late);
    assert(clock.SchedulePresent(late, 1, true) == late);
    assert(clock.SchedulePresent(late, 0, true) == late);
    assert(clock.SchedulePresent(late, 2, true) == late);
    assert(GuestFrameClock::Interval(std::numeric_limits<std::uint32_t>::max()).count() > 0);

    auto tick = start;
    for (int i = 1; i <= 3600; ++i) {
        tick = GuestFrameClock::NextVsync(tick, tick + 100us);
        assert(tick == start + GuestFrameClock::VsyncPeriod * i);
    }
    // Suspension never emits a burst of stale VSync events on resume.
    assert(GuestFrameClock::NextVsync(tick, tick + 5s) == tick + 5s + GuestFrameClock::VsyncPeriod);
    std::cout << "PASS: guest 60/30/20/15 FPS at host 60/90/120/144/165/240 Hz; "
                 "interval changes, unlock, jitter, stalls and VSync resume\n";
}
