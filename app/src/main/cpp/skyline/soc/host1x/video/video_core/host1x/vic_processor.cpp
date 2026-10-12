// SPDX-License-Identifier: GPL-3.0-or-later
// Register layout and fixed-point colour conversion follow Eden/Yuzu's VIC.
#include "video_core/host1x/vic_processor.h"
#include "video_core/host1x/host1x.h"
#include <algorithm>
#include <stdexcept>
#include <string>
namespace Tegra::Host1x {
namespace {
u32 Align(u32 x, u32 a) { return (x + a - 1) / a * a; }
void Dimensions(u32 w, u32 h) {
    if (!w || !h || w > 4096 || h > 4096) throw std::runtime_error("Invalid VIC surface dimensions");
}
Pixel Convert(Pixel p, const SlotStruct& slot) {
    const auto& m = slot.color_matrix;
    if (m.matrix_enable) {
        const s64 r = p.r, g = p.g, b = p.b;
        const auto shift = m.matrix_r_shift.Value();
        const s64 low = slot.config.soft_clamp_low.Value();
        const s64 high = slot.config.soft_clamp_high.Value();
        if (low > high) throw std::runtime_error("Invalid VIC colour clamp");
        auto row = [&](s64 c0, s64 c1, s64 c2, s64 c3) {
            return static_cast<u16>(std::clamp((((r*c0 + g*c1 + b*c2) >> shift) + c3) >> 8, low, high));
        };
        p.r = row(m.matrix_coeff00, m.matrix_coeff01, m.matrix_coeff02, m.matrix_coeff03);
        p.g = row(m.matrix_coeff10, m.matrix_coeff11, m.matrix_coeff12, m.matrix_coeff13);
        p.b = row(m.matrix_coeff20, m.matrix_coeff21, m.matrix_coeff22, m.matrix_coeff23);
    }
    return p;
}
Pixel Sample(const FFmpeg::Frame& f, u32 x, u32 y, u16 alpha) {
    const auto format = f.GetPixelFormat();
    const u8* luma = f.GetPlane(0) + y * f.GetStride(0);
    const u8* u = f.GetPlane(1) + (y / 2) * f.GetStride(1);
    if (format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P) {
        const u8* v = f.GetPlane(2) + (y / 2) * f.GetStride(2);
        return {u16(luma[x] << 2), u16(u[x/2] << 2), u16(v[x/2] << 2), alpha};
    }
    if (format == AV_PIX_FMT_NV12)
        return {u16(luma[x] << 2), u16(u[x & ~1u] << 2), u16(u[(x & ~1u)+1] << 2), alpha};
    throw std::runtime_error("Unsupported NVDEC output pixel format");
}
}
void VicProcessor::ProcessMethod(u32 method, u32 argument) {
    if (method >= registers.reg_array.size()) throw std::runtime_error("Invalid VIC register");
    registers.reg_array[method] = argument;
    if (method == 0xc0) Execute();
}
void VicProcessor::WritePlane(u64 address, std::span<const u8> linear, u32 widthBytes,
                             u32 height, u32 pitch, BlkKind kind, unsigned logHeight) {
    if (kind == BlkKind::Pitch) {
        host.gmmu_manager.WriteBlock(address, linear.data(), linear.size());
        return;
    }
    if (kind != BlkKind::Generic_16Bx2 || logHeight > 5)
        throw std::runtime_error("Unsupported VIC surface layout");
    // Tegra block-linear layout: 64-byte x 8-row GOBs, grouped vertically.
    const u32 blockHeight = 1u << logHeight;
    const u32 widthGobs = Align(widthBytes, 64) / 64;
    const size_t size = size_t(widthGobs) * Align(height, 8 * blockHeight) * 64;
    if (size > 64 * 1024 * 1024) throw std::runtime_error("VIC surface too large");
    std::vector<u8> tiled(size);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < widthBytes; ++x) {
            const size_t gob = size_t(y / (8 * blockHeight)) * widthGobs * 512 * blockHeight +
                               size_t(x / 64) * 512 * blockHeight + size_t((y / 8) % blockHeight) * 512;
            const size_t within = ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 +
                                  ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
            tiled[gob + within] = linear[size_t(y) * pitch + x];
        }
    }
    host.gmmu_manager.WriteBlock(address, tiled.data(), tiled.size());
}
void VicProcessor::Execute() {
    ConfigStruct config{};
    host.gmmu_manager.ReadBlock(registers.config_struct_offset.Address(), &config, sizeof(config));
    const auto& output = config.output_surface_config;
    const u32 width = output.out_surface_width + 1, height = output.out_surface_height + 1;
    Dimensions(width, height);
    if (config.output_config.output_transpose)
        throw std::runtime_error("VIC transpose is not implemented");
    std::vector<Pixel> pixels(size_t(width) * height, Pixel{
        u16(config.output_config.background_r), u16(config.output_config.background_g),
        u16(config.output_config.background_b), u16(config.output_config.background_a)});
    bool enabled = false, decoded = false;
    for (size_t i = 0; i < config.slot_structs.size(); ++i) {
        const auto& slot = config.slot_structs[i];
        if (!slot.config.slot_enable) continue;
        if (enabled || slot.config.planar_alpha.Value() != 1023)
            throw std::runtime_error("VIC alpha/multiple-slot composition is not implemented");
        if (slot.config.frame_format.Value() != DxvhadFrameFormat::Progressive)
            throw std::runtime_error("VIC interlaced output is not implemented");
        enabled = true;
        const auto address = registers.surfaces[i][SurfaceIndex::Current].luma.Address();
        const auto id = host.frame_queue.VicFindNvdecFdFromOffset(address);
        const auto frame = host.frame_queue.GetFrame(id, address);
        if (!frame) continue; // Decode reordering may not yet have produced a displayable frame.
        Dimensions(frame->GetWidth(), frame->GetHeight());
        const u32 left = slot.config.dest_rect_left, top = slot.config.dest_rect_top;
        const u32 right = slot.config.dest_rect_right + 1, bottom = slot.config.dest_rect_bottom + 1;
        if (right <= left || bottom <= top) throw std::runtime_error("Invalid VIC destination rectangle");
        // Source rectangles use 16 fractional bits. Destination rectangles are integer pixels.
        const u32 sx0 = slot.config.source_rect_left.Value() >> 16;
        const u32 sy0 = slot.config.source_rect_top.Value() >> 16;
        const u32 sx1 = (slot.config.source_rect_right.Value() >> 16) + 1;
        const u32 sy1 = (slot.config.source_rect_bottom.Value() >> 16) + 1;
        if (sx1 <= sx0 || sy1 <= sy0)
            throw std::runtime_error("Invalid VIC source rectangle: source=" +
                std::to_string(sx0) + "," + std::to_string(sy0) + ".." +
                std::to_string(sx1) + "," + std::to_string(sy1) + " decoded=" +
                std::to_string(frame->GetWidth()) + "x" + std::to_string(frame->GetHeight()) +
                " raw=" + std::to_string(slot.config.source_rect_left.Value()) + "," +
                std::to_string(slot.config.source_rect_top.Value()) + ".." +
                std::to_string(slot.config.source_rect_right.Value()) + "," +
                std::to_string(slot.config.source_rect_bottom.Value()));
        const u32 clipLeft = std::max(left, u32(config.output_config.target_rect_left));
        const u32 clipTop = std::max(top, u32(config.output_config.target_rect_top));
        const u32 clipRight = std::min({right, width, u32(config.output_config.target_rect_right) + 1});
        const u32 clipBottom = std::min({bottom, height, u32(config.output_config.target_rect_bottom) + 1});
        for (u32 y = clipTop; y < clipBottom; ++y) {
            const u32 sy = sy0 + u64(y - top) * (sy1 - sy0) / (bottom - top);
            if (sy >= u32(frame->GetHeight())) continue;
            for (u32 x = clipLeft; x < clipRight; ++x) {
                const u32 sx = sx0 + u64(x - left) * (sx1 - sx0) / (right - left);
                // Surface rectangles may include padding beyond the decoded frame.
                // Preserve the requested transform and clip unavailable source pixels;
                // shrinking the source rectangle would stretch the visible image.
                if (sx >= u32(frame->GetWidth())) continue;
                auto p = Convert(Sample(*frame, sx, sy, slot.config.planar_alpha), slot);
                pixels[size_t(y)*width+x] = p;
            }
        }
        decoded = true;
    }
    if (enabled && !decoded) return;
    const u32 lw = output.out_luma_width + 1, lh = output.out_luma_height + 1;
    Dimensions(lw, lh);
    const auto kind = output.out_block_kind.Value();
    const auto bh = output.out_block_height.Value();
    const auto format = output.out_pixel_format.Value();
    if (format == VideoPixelFormat::A8B8G8R8 || format == VideoPixelFormat::X8B8G8R8 || format == VideoPixelFormat::A8R8G8B8) {
        const u32 pitch = Align(lw * 4, 16);
        std::vector<u8> linear(size_t(pitch) * lh);
        for (u32 y = 0; y < std::min(height, lh); ++y) {
            for (u32 x = 0; x < std::min(width, lw); ++x) {
                const auto& p = pixels[size_t(y)*width+x];
                const u32 dx = config.output_config.output_flip_x ? width - 1 - x : x;
                const u32 dy = config.output_config.output_flip_y ? height - 1 - y : y;
                if (dx >= lw || dy >= lh) continue;
                const size_t dst = size_t(dy)*pitch + dx*4;
                linear[dst] = (format == VideoPixelFormat::A8R8G8B8 ? p.b : p.r) >> 2;
                linear[dst+1] = p.g >> 2;
                linear[dst+2] = (format == VideoPixelFormat::A8R8G8B8 ? p.r : p.b) >> 2;
                linear[dst+3] = format == VideoPixelFormat::X8B8G8R8 ? 255 : p.a >> 2;
            }
        }
        WritePlane(registers.output_surface.luma.Address(), linear, lw*4, lh, pitch, kind, bh);
    } else if (format == VideoPixelFormat::Y8__V8U8_N420) {
        if (config.output_config.output_flip_x || config.output_config.output_flip_y)
            throw std::runtime_error("VIC flipped NV12 output is not implemented");
        const u32 cw = output.out_chroma_width + 1, ch = output.out_chroma_height + 1;
        Dimensions(cw, ch);
        const u32 yp = Align(lw,16), cp = Align(cw*2,16);
        std::vector<u8> luma(size_t(yp)*lh), chroma(size_t(cp)*ch);
        for (u32 y=0; y<std::min(height,lh); ++y) for (u32 x=0; x<std::min(width,lw); ++x) {
            const auto& p=pixels[size_t(y)*width+x];
            luma[size_t(y)*yp+x]=p.r>>2;
            if (!(x&1) && !(y&1) && x/2<cw && y/2<ch) {
                chroma[size_t(y/2)*cp+x]=p.g>>2;
                chroma[size_t(y/2)*cp+x+1]=p.b>>2;
            }
        }
        WritePlane(registers.output_surface.luma.Address(),luma,lw,lh,yp,kind,bh);
        WritePlane(registers.output_surface.chroma_u.Address(),chroma,cw*2,ch,cp,kind,bh);
    } else throw std::runtime_error("Unsupported VIC output pixel format");
}
}
