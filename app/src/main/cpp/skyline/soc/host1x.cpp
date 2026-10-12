// SPDX-License-Identifier: MPL-2.0
#include <soc.h>
namespace skyline::soc::host1x {
Host1x::Host1x(const DeviceState &state)
    : video(strato::video::Memory{
        [&state](u32 address, void* destination, size_t size) {
            state.soc->smmu.Read(static_cast<u8*>(destination), address, static_cast<u32>(size));
        },
        [&state](u32 address, const void* source, size_t size) {
            state.soc->smmu.Write(address, const_cast<u8*>(static_cast<const u8*>(source)), static_cast<u32>(size));
        }}), channels{util::MakeFilledArray<ChannelCommandFifo, ChannelCount>(state, syncpoints, video)} {}
}
