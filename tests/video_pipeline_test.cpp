// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.h"
#include "video_core/host1x/codec_types.h"
#include "video_core/host1x/vic_types.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/vic_processor.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
using namespace Tegra::Host1x;
using namespace Tegra::Decoders;
int main(int argc, char** argv) {
    assert(argc==3);
    std::vector<u8> memory(4*1024*1024, 0xcd);
    strato::video::Memory access{
        [&](u32 a,void* p,size_t n){ if (a>memory.size() || n>memory.size()-a) throw std::runtime_error("read bounds"); std::memcpy(p,memory.data()+a,n); },
        [&](u32 a,const void* p,size_t n){ if(a>memory.size() || n>memory.size()-a) throw std::runtime_error("write bounds"); std::memcpy(memory.data()+a,p,n); }
    };
    auto load=[](const char* p){std::ifstream f(p,std::ios::binary); if (!f) throw std::runtime_error("Missing video fixture"); return std::vector<u8>(std::istreambuf_iterator<char>(f),{});};
    for (unsigned codec : {3,5}) for (unsigned blockHeight : {0,1,2,3,4,5,6}) {
        strato::video::Runtime runtime(access);
        if (codec==5) {
            auto packet=load(argv[2]);
            VP8PictureInfo context{};
            context.frame_width=32; context.frame_height=16;context.key_frame=1;
            const u32 tag=u32(packet[0])|(u32(packet[1])<<8)|(u32(packet[2])<<16);
            context.version=(tag>>1)&7; context.first_part_size=tag>>5;
            context.vld_buffer_size=packet.size()-10;
            access.WriteBlock(0x1000,&context,sizeof(context));
            access.WriteBlock(0x2000,packet.data()+10,packet.size()-10);
            runtime.NvdecMethod(3,0x10c+3,0x100000>>8);
        } else {
            auto packet=load(argv[1]);
            H264DecoderContext context{};
            auto& p=context.h264_parameter_set;
            p.pic_width_in_mbs=2;p.frame_height_in_mbs=1;p.frame_mbs_only_flag=1;
            p.chroma_format_idc.Assign(1);p.pic_order_cnt_type.Assign(2);p.entropy_coding_mode_flag=1;
            p.deblocking_filter_control_present_flag=1;p.chroma_qp_index_offset.Assign(-2);
            p.second_chroma_qp_index_offset.Assign(-2);p.flags.direct_8x8_inference.Assign(1);
            context.weight_scale_4x4.fill(16);context.weight_scale_8x8.fill(16);
            context.stream_len=packet.size();
            access.WriteBlock(0x1000,&context,sizeof(context));access.WriteBlock(0x2000,packet.data(),packet.size());
            runtime.NvdecMethod(3,0x10c,0x100000>>8);
        }
        runtime.NvdecMethod(3,0x80,codec);runtime.NvdecMethod(3,0x101,0x10);runtime.NvdecMethod(3,0x102,0x20);
        runtime.NvdecMethod(3,0xc0,0x100);
        ConfigStruct config{};
        auto& o=config.output_surface_config;o.out_surface_width.Assign(31);o.out_surface_height.Assign(15);
        o.out_luma_width.Assign(31);o.out_luma_height.Assign(15);o.out_pixel_format.Assign(VideoPixelFormat::A8B8G8R8);
        o.out_block_kind.Assign(blockHeight==6?BlkKind::Pitch:BlkKind::Generic_16Bx2);
        o.out_block_height.Assign(blockHeight==6?0:blockHeight);
        config.output_config.target_rect_right.Assign(31);config.output_config.target_rect_bottom.Assign(15);
        auto& slot=config.slot_structs[0];slot.config.slot_enable.Assign(1);slot.config.planar_alpha.Assign(1023);
        slot.config.soft_clamp_high.Assign(1023);slot.config.source_rect_right.Assign(31<<16);slot.config.source_rect_bottom.Assign(15<<16);
        slot.config.dest_rect_right.Assign(31);slot.config.dest_rect_bottom.Assign(15);
        auto& m=slot.color_matrix;m.matrix_enable.Assign(1);
        m.matrix_coeff00.Assign(298);m.matrix_coeff02.Assign(409);m.matrix_coeff03.Assign(-228864);
        m.matrix_coeff10.Assign(298);m.matrix_coeff11.Assign(-100);m.matrix_coeff12.Assign(-208);m.matrix_coeff13.Assign(138880);
        m.matrix_coeff20.Assign(298);m.matrix_coeff21.Assign(516);m.matrix_coeff23.Assign(-283520);
        access.WriteBlock(0x10000,&config,sizeof(config));
        std::fill(memory.begin()+0x20000,memory.begin()+0x40000,0xcd);
        runtime.VicMethod(1,0x100,0x100000>>8);runtime.VicMethod(1,0x1c2,0x10000>>8);
        runtime.VicMethod(1,0x1c8,0x20000>>8);runtime.VicMethod(1,0xc0,1);
        // Independent golden GOB addresses for both 64-byte columns and all 16 rows.
        for(unsigned y=0;y<16;y++)for(unsigned x=0;x<32;x++) {
            size_t at=y*128+x*4;
            if(blockHeight!=6){unsigned bx=x*4,bh=1<<blockHeight;at=(y/(8*bh))*1024*bh+(bx/64)*512*bh+((y/8)%bh)*512+((bx%64)/32)*256+((y%8)/2)*64+((bx%32)/16)*32+(y%2)*16+bx%16;}
            const auto* pixel=memory.data()+0x20000+at;
            assert(pixel[0]>=250 && pixel[1]<=3 && pixel[2]<=3 && pixel[3]==255);
        }
        assert(memory[0x1ffff]==0xcd && memory[0x40000]==0xcd);
    }
    // A surface/crop can extend beyond decoded pixels. Clip the visible region
    // without stretching it or reading the padding as if it were frame data.
    {
        Host1x host(access);
        host.frame_queue.Open(3);
        VicProcessor vic(host);
        ConfigStruct config{};
        auto& o=config.output_surface_config;
        o.out_surface_width.Assign(7);o.out_surface_height.Assign(3);
        o.out_luma_width.Assign(7);o.out_luma_height.Assign(3);
        o.out_pixel_format.Assign(VideoPixelFormat::A8B8G8R8);o.out_block_kind.Assign(BlkKind::Pitch);
        config.output_config.background_r.Assign(12*4);
        config.output_config.background_g.Assign(13*4);
        config.output_config.background_b.Assign(14*4);
        config.output_config.background_a.Assign(1023);
        config.output_config.target_rect_right.Assign(7);config.output_config.target_rect_bottom.Assign(3);
        auto& slot=config.slot_structs[0];slot.config.slot_enable.Assign(1);slot.config.planar_alpha.Assign(1023);
        slot.config.source_rect_right.Assign(7<<16);slot.config.source_rect_bottom.Assign(3<<16);
        slot.config.dest_rect_right.Assign(7);slot.config.dest_rect_bottom.Assign(3);
        vic.ProcessMethod(0x100,0x100000>>8);vic.ProcessMethod(0x1c2,0x10000>>8);vic.ProcessMethod(0x1c8,0x20000>>8);
        auto queueFrame=[&]{
            auto frame=std::make_shared<FFmpeg::Frame>();auto* f=frame->GetFrame();
            f->format=AV_PIX_FMT_YUV420P;f->width=4;f->height=2;
            assert(av_frame_get_buffer(f,32)==0);
            for(int y=0;y<2;++y)for(int x=0;x<4;++x)f->data[0][y*f->linesize[0]+x]=30+y*10+x;
            for(int x=0;x<2;++x){f->data[1][x]=90;f->data[2][x]=160;}
            host.frame_queue.PushPresentOrder(3,0x100000,std::move(frame));
        };
        access.WriteBlock(0x10000,&config,sizeof(config));queueFrame();vic.ProcessMethod(0xc0,1);
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<8;++x){
            const auto* p=memory.data()+0x20000+y*32+x*4;
            if(x<4&&y<2){assert(p[0]==30+y*10+x&&p[1]==90&&p[2]==160);}
            else {assert(p[0]==12&&p[1]==13&&p[2]==14);}
            assert(p[3]==255);
        }
        // Nonzero crop origin + scaling: original mapping remains intact after clipping.
        slot.config.source_rect_left.Assign(1<<16);slot.config.source_rect_right.Assign(4<<16);
        slot.config.source_rect_top.Assign(0);slot.config.source_rect_bottom.Assign(1<<16);
        access.WriteBlock(0x10000,&config,sizeof(config));queueFrame();vic.ProcessMethod(0xc0,1);
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<8;++x){
            const unsigned sx=1+x/2,sy=y/2;const auto* p=memory.data()+0x20000+y*32+x*4;
            if(sx<4)assert(p[0]==30+sy*10+sx&&p[1]==90&&p[2]==160);
            else assert(p[0]==12&&p[1]==13&&p[2]==14);
        }
        slot.config.source_rect_left.Assign(5<<16); // Truly inverted rectangles still fail.
        access.WriteBlock(0x10000,&config,sizeof(config));queueFrame();bool rejected=false;
        try{vic.ProcessMethod(0xc0,1);}catch(const std::runtime_error& e){rejected=std::string(e.what()).find("decoded=4x2")!=std::string::npos;}
        assert(rejected);
    }
    bool failed=false;try{access.ReadBlock(UINT32_MAX,nullptr,2);}catch(const std::runtime_error&){failed=true;}assert(failed);
    std::cout<<"PASS: H264 and VP8 reconstruction -> FFmpeg -> VIC colour conversion -> pitch/block-linear output (GOB heights 1..32); crop clipping/scaling; bounds\n";
}
