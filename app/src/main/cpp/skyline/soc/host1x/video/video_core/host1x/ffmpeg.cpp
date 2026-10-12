// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/host1x/ffmpeg.h"
#include <cstring>
#include <stdexcept>
#include <string>
namespace FFmpeg {
static void Check(int result, const char* operation) {
    if (result >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, error, sizeof(error));
    throw std::runtime_error(std::string(operation) + ": " + error);
}
Frame::Frame() : frame(av_frame_alloc()) { if (!frame) throw std::bad_alloc(); }
Frame::~Frame() { av_frame_free(&frame); }
DecodeApi::~DecodeApi() { avcodec_free_context(&context); }
bool DecodeApi::Initialize(Tegra::Host1x::NvdecCommon::VideoCodec type) {
    using Codec = Tegra::Host1x::NvdecCommon::VideoCodec;
    AVCodecID id;
    switch (type) {
    case Codec::H264: id = AV_CODEC_ID_H264; break;
    case Codec::VP8: id = AV_CODEC_ID_VP8; break;
    case Codec::VP9: id = AV_CODEC_ID_VP9; break;
    default: throw std::runtime_error("Unsupported NVDEC codec");
    }
    const auto* codec = avcodec_find_decoder(id);
    if (!codec) throw std::runtime_error("Required FFmpeg decoder missing");
    context = avcodec_alloc_context3(codec);
    if (!context) throw std::bad_alloc();
    // Slice threading avoids adding frame-thread latency to the NVDEC/VIC pipeline.
    context->thread_count = 2;
    context->thread_type = FF_THREAD_SLICE;
    context->max_pixels = 4096LL * 4096;
    Check(avcodec_open2(context, codec, nullptr), "Opening NVDEC decoder");
    return true;
}
void DecodeApi::Drain() {
    for (;;) {
        auto frame = std::make_shared<Frame>();
        const int result = avcodec_receive_frame(context, frame->GetFrame());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
        Check(result, "Receiving NVDEC frame");
        if (offsets.empty()) throw std::runtime_error("Decoded video frame has no destination");
        ready.push({std::move(frame), offsets.front()});
        offsets.pop();
    }
}
bool DecodeApi::SendPacket(std::span<const u8> data, const FrameOffsets& destination,
                           std::optional<FrameDimensions>) {
    if (data.empty()) return true; // Some VP9 commands only update reconstruction state.
    if (data.size() > 16 * 1024 * 1024) throw std::runtime_error("NVDEC packet exceeds limit");
    auto deleter = [](AVPacket* p) { av_packet_free(&p); };
    std::unique_ptr<AVPacket, decltype(deleter)> packet(av_packet_alloc(), deleter);
    if (!packet) throw std::bad_alloc();
    // av_new_packet supplies FFmpeg's required zero padding; no guest buffer overread.
    Check(av_new_packet(packet.get(), static_cast<int>(data.size())), "Allocating NVDEC packet");
    std::memcpy(packet->data, data.data(), data.size());
    int result = avcodec_send_packet(context, packet.get());
    if (result == AVERROR(EAGAIN)) {
        Drain();
        result = avcodec_send_packet(context, packet.get());
    }
    Check(result, "Decoding NVDEC packet");
    if (!destination.hidden) offsets.push(destination);
    if (offsets.size() > 64) throw std::runtime_error("NVDEC decoder is not producing frames");
    Drain();
    return true;
}
std::optional<DecodeApi::DecodedFrame> DecodeApi::ReceiveFrame() {
    if (ready.empty()) return std::nullopt;
    auto frame = std::move(ready.front()); ready.pop(); return frame;
}
}
