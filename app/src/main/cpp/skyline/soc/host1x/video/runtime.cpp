// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/codecs/h264.h"
#include "video_core/host1x/codecs/vp8.h"
#include "video_core/host1x/codecs/vp9.h"
#include "video_core/host1x/vic_processor.h"
#include <array>
#include <stdexcept>
namespace strato::video {
void Memory::ReadBlock(std::uint64_t address, void* output, std::size_t size) const {
    if (address > UINT32_MAX || size > 64 * 1024 * 1024 || size > (1ULL << 32) - address)
        throw std::runtime_error("NVDEC/VIC read outside the SMMU address space");
    read(static_cast<std::uint32_t>(address), output, size);
}
void Memory::WriteBlock(std::uint64_t address, const void* input, std::size_t size) const {
    if (address > UINT32_MAX || size > 64 * 1024 * 1024 || size > (1ULL << 32) - address)
        throw std::runtime_error("NVDEC/VIC write outside the SMMU address space");
    write(static_cast<std::uint32_t>(address), input, size);
}
struct Runtime::Impl {
    Tegra::Host1x::Host1x host;
    unsigned nextChannel{};
    struct Nvdec {
        Tegra::Host1x::NvdecCommon::NvdecRegisters registers{};
        std::unique_ptr<Tegra::Decoder> decoder;
        u32 codec{};
    };
    std::array<Nvdec, 14> nvdec;
    std::array<std::unique_ptr<Tegra::Host1x::VicProcessor>, 14> vic;
    explicit Impl(Memory memory) : host(std::move(memory)) {}
};
Runtime::Runtime(Memory memory) : impl(std::make_unique<Impl>(std::move(memory))) {}
Runtime::~Runtime() = default;
unsigned Runtime::ReserveChannel() {
    if (impl->nextChannel >= 14) throw std::runtime_error("Too many Host1x channels");
    return impl->nextChannel++;
}
void Runtime::NvdecMethod(unsigned channel, u32 method, u32 argument) {
    auto& engine = impl->nvdec.at(channel);
    if (method >= engine.registers.reg_array.size()) throw std::runtime_error("Invalid NVDEC register");
    engine.registers.reg_array[method] = argument;
    if (method != 0xc0) return;
    const u32 codec = static_cast<u32>(engine.registers.set_codec_id);
    if (!engine.decoder || engine.codec != codec) {
        impl->host.frame_queue.Open(channel);
        engine.decoder.reset();
        switch (codec) {
        case 3: engine.decoder = std::make_unique<Tegra::Decoders::H264>(impl->host, engine.registers, channel); break;
        case 5: engine.decoder = std::make_unique<Tegra::Decoders::VP8>(impl->host, engine.registers, channel); break;
        case 9: engine.decoder = std::make_unique<Tegra::Decoders::VP9>(impl->host, engine.registers, channel); break;
        default: throw std::runtime_error("Unsupported NVDEC codec ID " + std::to_string(codec));
        }
        engine.codec = codec;
    }
    engine.decoder->Decode();
}
void Runtime::VicMethod(unsigned channel, u32 method, u32 argument) {
    auto& engine = impl->vic.at(channel);
    if (!engine) engine = std::make_unique<Tegra::Host1x::VicProcessor>(impl->host);
    engine->ProcessMethod(method, argument);
}
}
