// SPDX-License-Identifier: MPL-2.0
#include "vic.h"
namespace skyline::soc::host1x {
VicClass::VicClass(std::function<void()> done, strato::video::Runtime &video, unsigned channel)
    : opDoneCallback(std::move(done)), video(video), channel(channel) {}
void VicClass::CallMethod(u32 method, u32 argument) {
    video.VicMethod(channel, method, argument);
    if (method == 0xc0) opDoneCallback();
}
}
