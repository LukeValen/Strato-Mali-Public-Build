// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include "common/common_types.h"
#include "video_core/host1x/nvdec_common.h"
extern "C" {
#include <libavcodec/avcodec.h>
}
namespace FFmpeg {
struct FrameOffsets { bool interlaced{}, hidden{}; u64 luma{}, luma_bottom{}; };
struct FrameDimensions { s32 width{}, height{}; };
class Frame {
public:
    Frame();
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    int GetWidth() const { return frame->width; }
    int GetHeight() const { return frame->height; }
    AVPixelFormat GetPixelFormat() const { return static_cast<AVPixelFormat>(frame->format); }
    int GetStride(int p) const { return frame->linesize[p]; }
    const u8* GetPlane(int p) const { return frame->data[p]; }
    AVFrame* GetFrame() const { return frame; }
private:
    AVFrame* frame{};
};
class DecodeApi {
public:
    ~DecodeApi();
    bool Initialize(Tegra::Host1x::NvdecCommon::VideoCodec codec);
    bool UsingDecodeOrder() const { return false; }
    bool SendPacket(std::span<const u8>, const FrameOffsets&, std::optional<FrameDimensions>);
    struct DecodedFrame { std::shared_ptr<Frame> frame; FrameOffsets offsets; };
    std::optional<DecodedFrame> ReceiveFrame();
private:
    void Drain();
    AVCodecContext* context{};
    std::queue<FrameOffsets> offsets;
    std::queue<DecodedFrame> ready;
};
}
