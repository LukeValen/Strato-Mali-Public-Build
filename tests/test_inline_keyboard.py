#!/usr/bin/env python3
"""Verify the real inline swkbd state machine and HOS wire packets, without Android."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = r'''
#include "app/src/main/cpp/skyline/applet/swkbd/inline_keyboard.h"
#include <cassert>
using skyline::applet::swkbd::InlineKeyboard;
using Bytes = std::vector<std::uint8_t>;
static void put(Bytes& b, size_t p, uint32_t x) { for (int i=0;i<4;i++) b.at(p+i)=x>>(i*8); }
static uint32_t get(const Bytes& b, size_t p) { uint32_t x=0; for(int i=0;i<4;i++) x|=uint32_t(b.at(p+i))<<(i*8); return x; }
static Bytes calc(bool modern, uint32_t flags, bool utf8=false, std::u16string text=u"Luke") {
    const auto size = modern ? 0x4E8 : 0x4A0;
    Bytes b(size+4);
    put(b, 0, 10); put(b, 4, 0x30000);
    b[8]=size&255; b[9]=size>>8; put(b, 12, flags);
    const auto a=4+(modern?0x18:0x20), t=4+(modern?0x90:0x68);
    put(b,a+0x20,12); put(b,a+0x24,1);
    put(b,4+(modern?0x8C:0x1C),text.size());
    for(size_t i=0;i<text.size();++i) { b.at(t+i*2)=text[i]&255; b.at(t+i*2+1)=text[i]>>8; }
    b[4+(modern?0x484:0x45C)]=utf8;
    return b;
}
static InlineKeyboard::Update request(InlineKeyboard& k, const Bytes& b) { return k.Request(b.data(), b.size()); }
static void cycle(bool modern, bool utf8, bool v2) {
    InlineKeyboard k;
    auto initial=request(k,calc(modern,1|8|16|32,utf8));
    assert(initial.valid && initial.action==InlineKeyboard::Action::None);
    assert(k.State()==1 && initial.replies.size()==2);
    assert(get(initial.replies[0],4)==1 && get(initial.replies[1],4)==0);
    assert(initial.replies[1].size()==9);
    if(v2) { request(k,Bytes{13,0,0,0,1}); request(k,Bytes{14,0,0,0,1}); }
    auto shown=request(k,calc(modern,4));
    assert(shown.action==InlineKeyboard::Action::Show && k.IsShown());
    assert(shown.replies.size()==2 && get(shown.replies[0],0)==2 && get(shown.replies[1],0)==3);
    const std::u16string name=u"João 🫐";
    auto changed=k.Input(2,name,name.size(),k.revision);
    assert(changed.replies.size()==1);
    const auto& packet=changed.replies[0];
    const size_t stringSize=utf8?0x7D4:0x3EC;
    assert(packet.size()==8+stringSize+16+(v2?1:0));
    assert(get(packet,4)==(v2?(utf8?15:13):(utf8?7:2)));
    assert(get(packet,8+stringSize)==name.size());
    assert(get(packet,8+stringSize+12)==name.size());
    if(utf8) assert(std::string(reinterpret_cast<const char*>(packet.data()+8))=="João 🫐");
    else assert(std::memcmp(packet.data()+8,name.data(),name.size()*2)==0);
    auto moved=k.Input(3,name,2,k.revision);
    assert(moved.replies[0].size()==8+stringSize+8+(v2?1:0));
    assert(get(moved.replies[0],4)==(v2?(utf8?16:14):(utf8?8:3)));
    auto final=k.Input(5,name,name.size(),k.revision);
    assert(final.action==InlineKeyboard::Action::Hide && k.State()==1);
    assert(final.replies.size()==3 && get(final.replies[0],4)==(utf8?9:5));
    assert(final.replies[0].size()==8+stringSize+4);
    assert(get(final.replies[1],0)==4 && get(final.replies[2],0)==1);
    assert(k.Input(2,u"stale",0,k.revision).replies.empty());
    assert(request(k,calc(modern,4)).action==InlineKeyboard::Action::Show);
    auto cancel=k.Input(6,u"",0,k.revision);
    assert(cancel.replies.size()==3 && get(cancel.replies[0],4)==6 && cancel.replies[0].size()==8);
    auto done=request(k,Bytes{4,0,0,0});
    assert(done.action==InlineKeyboard::Action::Finish && k.State()==0);
    assert(k.Input(5,u"stale",0,k.revision).replies.empty());
    assert(!request(k,calc(modern,1)).valid);
}
int main() {
    for(bool modern:{false,true}) for(bool utf8:{false,true}) for(bool v2:{false,true}) cycle(modern,utf8,v2);
    for(bool modern:{false,true}) {
        const auto b=calc(modern,1|4|8|16|32);
        for(size_t n=0;n<b.size();++n) {
            InlineKeyboard k;
            auto result=k.Request(b.data(),n);
            assert(!result.valid && k.State()==0);
        }
    }
    InlineKeyboard k;
    request(k,calc(false,1|4|8|16));
    const auto oldRevision=k.revision;
    auto update=request(k,calc(false,8|16,false,u"New"));
    assert(update.action==InlineKeyboard::Action::Update && k.text==u"New");
    assert(k.Input(5,u"Old",3,oldRevision).replies.empty() && k.IsShown());
    assert(k.Input(5,u"",0,k.revision).replies.empty()); // minimum length
    k.appearance.disableCancel=true;
    assert(k.Input(6,u"",0,k.revision).replies.empty() && k.IsShown());
    assert(request(k,calc(false,128)).action==InlineKeyboard::Action::Hide);
    assert(request(k,Bytes{6,0,0,0}).replies[0].size()==8);
    assert(get(request(k,Bytes{12,0,0,0}).replies[0],4)==12);
    assert(!request(k,Bytes{13,0,0,0}).valid);
    // Truncation preserves surrogate-pair boundaries.
    request(k,calc(false,4));
    k.appearance.maxLength=2;
    auto truncated=k.Input(2,u"A🫐",3,k.revision);
    assert(k.text==u"A" && k.cursor==1);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp=Path(tmp)/'test.cpp'; cpp.write_text(source)
    exe=Path(tmp)/'test'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(root),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: inline initialize/appear/edit/cursor/submit/cancel/reopen/finalize; both Calc layouts; UTF-8/16; V2; malformed packets and stale events')
