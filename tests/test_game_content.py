#!/usr/bin/env python3
"""Exercise production CNMT/BKTR parsers and patch reads with ASan/UBSan."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
skyline = root / "app/src/main/cpp/skyline"
# The backing uses existing AES-CTR code. The mapper harness supplies identity CTR
# to test the actual address routing separately from cryptography and Android.
backing = (skyline / "vfs/bktr_backing.h").read_text()
backing = "\n".join(line for line in backing.splitlines() if not line.startswith(("#include", "#pragma")))
harness = r'''
#include <cassert>
#include <iostream>
#include <memory>
#include <span>
#include <array>
#include <string>
#include <vfs/patch_table.h>
#include <loader/content_metadata.h>
namespace skyline {
using u8=uint8_t; using u32=uint32_t; using u64=uint64_t;
template<class T> using span=std::span<T>;
namespace util {
template<class T> T MakeMagic(const char *p) { T v{}; std::memcpy(&v,p,sizeof(T));return v; }
inline uint32_t SwapEndianness(uint32_t v) { return __builtin_bswap32(v); }
}
namespace crypto { struct KeyStore {using Key128=std::array<u8,16>;}; }
namespace vfs {
struct Backing {
 struct Mode { bool read=true,write=false,append=false;};
 size_t size{}; virtual ~Backing()=default;
 virtual size_t ReadImpl(span<u8>,size_t)=0;
 size_t ReadUnchecked(span<u8> out,size_t offset=0) {return ReadImpl(out,offset);}
};
struct Memory : Backing {
 std::vector<u8> bytes;
 explicit Memory(std::vector<u8> b):bytes(std::move(b)){size=bytes.size();}
 size_t ReadImpl(span<u8> out,size_t off) override {
  patch::Require(off<=size && out.size()<=size-off,"memory range");
  std::memcpy(out.data(),bytes.data()+off,out.size());return out.size();
 }
};
struct CtrEncryptedBacking : Backing {
 std::shared_ptr<Backing> backing;
 CtrEncryptedBacking(crypto::KeyStore::Key128,crypto::KeyStore::Key128,std::shared_ptr<Backing> b,size_t):backing(b){size=b->size;}
 size_t ReadImpl(span<u8> out,size_t off)override{return backing->ReadUnchecked(out,off);}
};
}}
BACKING
using namespace skyline;
template<class T> void put(std::vector<u8>& b,size_t o,T v){assert(o+sizeof(v)<=b.size());std::memcpy(b.data()+o,&v,sizeof(v));}
template<class F> void rejects(F f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}assert(threw);}
std::vector<u8> table(bool reloc,u64 end,std::vector<vfs::patch::Entry> entries){
 std::vector<u8>b(0x8000);put<u32>(b,4,1);put<u64>(b,8,end);put<u64>(b,16,0);
 put<u32>(b,0x4004,entries.size());put<u64>(b,0x4008,end);
 for(size_t i=0;i<entries.size();++i){size_t o=0x4010+i*(reloc?20:16);auto&e=entries[i];put<u64>(b,o,e.offset);
  if(reloc){put<u64>(b,o+8,e.physical);put<u32>(b,o+16,e.source);}else put<u32>(b,o+12,e.source);
 }return b;
}
int main(){
 constexpr u64 game=0x01002b30028f6000;
 std::vector<u8> cnmt(0x20+0x10+0x38);
 put<u64>(cnmt,0,game|0x800);put<u32>(cnmt,8,65536);put<u8>(cnmt,12,0x81);
 put<uint16_t>(cnmt,14,0x10);put<uint16_t>(cnmt,16,1);put<u64>(cnmt,0x20,game);
 put<u8>(cnmt,0x30+0x31,0x10);put<u8>(cnmt,0x30+0x36,1);
 auto meta=loader::content::ParseMetadata(cnmt,game);assert(meta.version==65536&&meta.records[0].size==0x1000);
 // A complete update can omit delta fragments; essential NCAs remain mandatory.
 auto delta=meta.records[0];delta.type=6;
 assert(!loader::content::ValidateRecordPresence(meta,delta,false,0));
 assert(loader::content::ValidateRecordPresence(meta,delta,true,delta.size));
 rejects([&]{loader::content::ValidateRecordPresence(meta,delta,true,delta.size-1);});
 for(uint8_t type : {1,2,3,4,5}) {
  auto required=delta;required.type=type;
  rejects([&]{loader::content::ValidateRecordPresence(meta,required,false,0);});
 }
 auto dlcMeta=meta;dlcMeta.type=0x82;
 rejects([&]{loader::content::ValidateRecordPresence(dlcMeta,delta,false,0);});
 auto complete=cnmt;complete.resize(cnmt.size()+0x38);put<uint16_t>(complete,16,2);
 std::copy(cnmt.begin()+0x30,cnmt.end(),complete.begin()+cnmt.size());
 put<u8>(complete,cnmt.size()+0x36,6);
 assert(loader::content::ParseMetadata(complete,game).records.size()==2);
 rejects([&]{loader::content::ParseMetadata(cnmt,game+0x2000);});
 auto bad=cnmt;bad.resize(0x30);rejects([&]{loader::content::ParseMetadata(bad,game);});
 bad=cnmt;put<uint16_t>(bad,16,65535);rejects([&]{loader::content::ParseMetadata(bad,game);});
 bad=cnmt;put<u8>(bad,12,0x80);rejects([&]{loader::content::ParseMetadata(bad,game);});
 bad=cnmt;put<u8>(bad,0x30+0x36,6);rejects([&]{loader::content::ParseMetadata(bad,game);});
 bad=cnmt;put<u8>(bad,12,0x82);put<u64>(bad,0,game+0x1001);put<u8>(bad,0x30+0x36,2);
 assert(loader::content::ParseMetadata(bad,game).type==0x82);
 put<u64>(bad,0,game+0x2000);rejects([&]{loader::content::ParseMetadata(bad,game);});
 auto rel=table(true,12,{{0,4,0},{3,2,1},{8,12,0}});
 auto sub=table(false,16,{{0,0,1},{4,0,2}});
 auto parsed=vfs::patch::Parse(rel,true,3);assert(vfs::patch::Find(parsed,3)==1&&vfs::patch::Find(parsed,11)==2);
 rejects([&]{vfs::patch::Find(parsed,12);});
 rejects([&]{vfs::patch::Parse(rel,true,4);});
 auto invalid=rel;put<u32>(invalid,4,UINT32_MAX);rejects([&]{vfs::patch::Parse(invalid,true,3);});
 invalid=rel;put<u32>(invalid,0x4004,UINT32_MAX);rejects([&]{vfs::patch::Parse(invalid,true,3);});
 invalid=rel;put<u64>(invalid,0x4010+20,0);rejects([&]{vfs::patch::Parse(invalid,true,3);});
 invalid=rel;put<u32>(invalid,0x4010+16,2);rejects([&]{vfs::patch::Parse(invalid,true,3);});
 invalid=rel;invalid.resize(0x4000);rejects([&]{vfs::patch::Parse(invalid,true,3);});
 // Read across base/patch mappings and across encryption-subsection boundaries.
 std::vector<u8>raw(0x10010);for(size_t i=0;i<16;++i)raw[i]=100+i;
 std::memcpy(raw.data()+16,rel.data(),rel.size());std::memcpy(raw.data()+0x8010,sub.data(),sub.size());
 std::vector<u8>baseBytes(32);for(size_t i=0;i<baseBytes.size();++i)baseBytes[i]=i;
 auto base=std::make_shared<vfs::Memory>(baseBytes),patch=std::make_shared<vfs::Memory>(raw);
 vfs::BktrHeader rh{16,0x8000,util::MakeMagic<u32>("BKTR"),1,3,0},sh{0x8010,0x8000,util::MakeMagic<u32>("BKTR"),1,2,0};
 vfs::BktrBacking bk(base,patch,patch,rh,sh,{},{},0,false);
 // Silksong's update has 0x4200 bytes after its complete subsection table.
 // The extra section data must not invalidate a correctly bounded BKTR map.
 auto paddedRaw=raw;paddedRaw.resize(raw.size()+0x4200,0xa5);
 auto paddedPatch=std::make_shared<vfs::Memory>(paddedRaw);
 vfs::BktrBacking padded(base,paddedPatch,paddedPatch,rh,sh,{},{},0,false);
 std::vector<u8> paddedRead(12);
 assert(padded.ReadUnchecked(paddedRead,0)==paddedRead.size());
 assert((paddedRead==std::vector<u8>{4,5,6,102,103,104,105,106,12,13,14,15}));
 auto tooLong=sh;tooLong.size=paddedRaw.size()-sh.offset+1;
 rejects([&]{vfs::BktrBacking x(base,paddedPatch,paddedPatch,rh,tooLong,{},{},0,false);});
 auto separated=sh;separated.offset++;
 rejects([&]{vfs::BktrBacking x(base,paddedPatch,paddedPatch,rh,separated,{},{},0,false);});
 // A table advertising the metadata position as its physical data limit is
 // inconsistent: the patch bytes stop at the relocation table's start.
 auto wrongSubRoot=table(false,sh.offset,{{0,0,1},{4,0,2}});
 auto wrongRaw=raw;
 std::memcpy(wrongRaw.data()+sh.offset,wrongSubRoot.data(),wrongSubRoot.size());
 auto wrongPatch=std::make_shared<vfs::Memory>(wrongRaw);
 rejects([&]{vfs::BktrBacking x(base,wrongPatch,wrongPatch,rh,sh,{},{},0,false);});
 std::vector<u8> expected{4,5,6,102,103,104,105,106,12,13,14,15};
 for(size_t off=0;off<=expected.size();++off)for(size_t len=0;len<=expected.size()-off;++len){
  std::vector<u8>out(len);assert(bk.ReadUnchecked(out,off)==len);assert(std::equal(out.begin(),out.end(),expected.begin()+off));
 }
 std::vector<u8>out(2);rejects([&]{bk.ReadUnchecked(out,11);});
 rejects([&]{vfs::BktrBacking x(nullptr,patch,patch,rh,sh,{},{},0,false);});
 auto small=std::make_shared<vfs::Memory>(std::vector<u8>(5));rejects([&]{vfs::BktrBacking x(small,patch,patch,rh,sh,{},{},0,false);});
 auto invalidHeader=rh;invalidHeader.size=UINT64_MAX;rejects([&]{vfs::BktrBacking x(base,patch,patch,invalidHeader,sh,{},{},0,false);});
 // A terminal subsection sentinel at the patch-data/relocation boundary is
 // valid. A larger value still describes bytes inside the BKTR metadata.
 auto terminalSub=table(false,16,{{0,0,1},{4,0,2},{16,0,3}});
 std::vector<u8> terminalRaw(raw);
 std::memcpy(terminalRaw.data()+0x8010,terminalSub.data(),terminalSub.size());
 auto terminalBacking=std::make_shared<vfs::Memory>(terminalRaw);
 auto terminalHeader=sh;terminalHeader.entries=3;
 vfs::BktrBacking terminal(base,terminalBacking,terminalBacking,rh,terminalHeader,{},{},0,false);
 std::vector<u8> terminalRead(expected.size());
 assert(terminal.ReadUnchecked(terminalRead,0)==terminalRead.size() && terminalRead==expected);
 auto pastSub=table(false,16,{{0,0,1},{4,0,2},{17,0,3}});
 std::memcpy(terminalRaw.data()+0x8010,pastSub.data(),pastSub.size());
 auto pastBacking=std::make_shared<vfs::Memory>(terminalRaw);
 rejects([&]{vfs::BktrBacking x(base,pastBacking,pastBacking,rh,terminalHeader,{},{},0,false);});
 // Repeated invalid input must throw rather than trigger sanitizer failures.
 for(size_t len=0;len<cnmt.size();++len){auto truncated=cnmt;truncated.resize(len);rejects([&]{loader::content::ParseMetadata(truncated,game);});}
 std::cout<<"PASS: content ownership, CNMT bounds, BKTR validation and cross-boundary reads\n";
}
'''.replace("BACKING", backing)
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    source.write_text(harness)
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(skyline), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
