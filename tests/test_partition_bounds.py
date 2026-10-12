#!/usr/bin/env python3
"""Exercise production PFS0/HFS0 construction with valid and truncated entries."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
text=(root/"app/src/main/cpp/skyline/vfs/partition_filesystem.cpp").read_text()
method=text[text.index("    PartitionFileSystem::PartitionFileSystem("):text.index("    std::shared_ptr<Backing> PartitionFileSystem::OpenFileImpl(")]
source=r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#include <memory>
#include <vector>
#include <map>
#include <span>
#include <string>
#include <stdexcept>
namespace skyline::vfs {
using u32=uint32_t;using u64=uint64_t;
using std::span;
struct exception:std::runtime_error{template<class...T>exception(const char*m,T&&...):std::runtime_error(m){}};
namespace util {template<class T>T MakeMagic(const char*p){T v{};std::memcpy(&v,p,sizeof(T));return v;}}
struct Backing {
 std::vector<uint8_t> bytes;size_t size;
 Backing(std::vector<uint8_t>b):bytes(std::move(b)),size(bytes.size()){}
 template<class T>T Read(size_t off=0){T v{};Read(span<T>(&v,1),off);return v;}
 template<class T>void Read(span<T> out,size_t off){
  if(off>size||out.size_bytes()>size-off)throw exception("short backing");
  std::memcpy(out.data(),bytes.data()+off,out.size_bytes());
 }
};
struct FileSystem {};
struct PartitionFileSystem:FileSystem {
 struct FsHeader{u32 magic,numFiles,stringTableSize,pad;}header;
 struct PartitionFileEntry{u64 offset,size;u32 stringTableOffset,pad;};
 struct HashedFileEntry{PartitionFileEntry entry;u32 pad;uint8_t hash[32];};
 bool hashed;size_t fileDataOffset;std::shared_ptr<Backing>backing;
 std::map<std::string,PartitionFileEntry>fileMap;
 PartitionFileSystem(const std::shared_ptr<Backing>&);
};
METHOD
}
using namespace skyline::vfs;
template<class T>void put(std::vector<uint8_t>&b,size_t o,T v){std::memcpy(b.data()+o,&v,sizeof(v));}
template<class F>void rejects(F f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}assert(threw);}
int main(){
 for(bool hashed:{false,true}){
  size_t stride=hashed?64:24,dataOffset=16+2*stride+4;
  std::vector<uint8_t>b(dataOffset+8);
  put<u32>(b,0,util::MakeMagic<u32>(hashed?"HFS0":"PFS0"));put<u32>(b,4,2);put<u32>(b,8,4);
  put<u64>(b,16,0);put<u64>(b,24,4);put<u32>(b,32,0);
  put<u64>(b,16+stride,4);put<u64>(b,24+stride,4);put<u32>(b,32+stride,2);
  std::memcpy(b.data()+16+2*stride,"a\0b\0",4);
  PartitionFileSystem fs(std::make_shared<Backing>(b));assert(fs.fileMap.size()==2&&fs.fileMap.at("b").offset==4);
  auto bad=b;bad.resize(dataOffset);rejects([&]{PartitionFileSystem x(std::make_shared<Backing>(bad));});
  bad=b;put<u32>(bad,32,100);rejects([&]{PartitionFileSystem x(std::make_shared<Backing>(bad));});
  bad=b;put<u64>(bad,24+stride,UINT64_MAX);rejects([&]{PartitionFileSystem x(std::make_shared<Backing>(bad));});
  bad=b;put<u32>(bad,4,UINT32_MAX);rejects([&]{PartitionFileSystem x(std::make_shared<Backing>(bad));});
  bad=b;put<u32>(bad,32+stride,0);rejects([&]{PartitionFileSystem x(std::make_shared<Backing>(bad));});
  bad=b;bad[16+2*stride+3]='c';rejects([&]{PartitionFileSystem x(std::make_shared<Backing>(bad));});
 }
}
'''.replace("METHOD",method)
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/"test.cpp").write_text(source)
 subprocess.run([os.environ.get("CXX","g++"),"-std=c++20","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer",str(p/"test.cpp"),"-o",str(p/"test")],check=True)
 subprocess.run([str(p/"test")],check=True)
print("PASS: PFS0/HFS0 file coverage, table bounds, names, duplicates and truncation")
