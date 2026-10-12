// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "video_core/host1x/vic_types.h"
namespace Tegra::Host1x {
class VicProcessor {
public:
    explicit VicProcessor(Host1x& host) : host(host) {}
    void ProcessMethod(u32 method, u32 argument);
private:
    void Execute();
    void WritePlane(u64 address, std::span<const u8> plane, u32 widthBytes,
                    u32 height, u32 pitch, BlkKind kind, unsigned blockHeight);
    Host1x& host;
    VicRegisters registers{};
};
}
