// SPDX-License-Identifier: MPL-2.0
#include "nvdec.h"
namespace skyline::soc::host1x {
NvDecClass::NvDecClass(std::function<void()> done, strato::video::Runtime &video, unsigned channel)
    : opDoneCallback(std::move(done)), video(video), channel(channel) {}
void NvDecClass::CallMethod(u32 method, u32 argument) {
    video.NvdecMethod(channel, method, argument);
    if (method == 0xc0) opDoneCallback();
}
}
