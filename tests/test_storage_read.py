#!/usr/bin/env python3
"""Compile the production storage read method and verify IPC buffer boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
text = (root / "app/src/main/cpp/skyline/services/fssrv/IStorage.cpp").read_text()
method = text[text.index("    Result IStorage::Read("):text.index("    Result IStorage::GetSize(")]
source = r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#define LOGW(...) ((void)0)
namespace skyline::service::fssrv {
using i64=int64_t; using Result=int;
namespace type {struct KSession {};}
namespace ipc {
struct IpcResponse {};
struct IpcRequest {
 i64 offset,length;int index{};std::vector<std::span<uint8_t>> outputBuf;
 template<class T>T Pop(){return static_cast<T>(index++?length:offset);}
};
}
namespace result {constexpr int InvalidOffset=1,InvalidSize=2,UnexpectedFailure=3;}
struct Backing {
 std::vector<uint8_t> bytes{1,2,3,4,5,6};size_t size=bytes.size();bool shortRead=false;
 size_t ReadUnchecked(std::span<uint8_t> out,size_t off){
  assert(off<=size&&out.size()<=size-off);
  if(shortRead)return 0;
  std::copy(bytes.begin()+off,bytes.begin()+off+out.size(),out.begin());return out.size();
 }
};
struct IStorage {
 std::unique_ptr<Backing> backing=std::make_unique<Backing>();
 Result Read(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);
};
METHOD
}
int main(){
 using namespace skyline::service::fssrv;
 IStorage storage;type::KSession session;ipc::IpcResponse response;
 std::vector<uint8_t>out(8,0xaa);
 auto read=[&](i64 offset,i64 size,bool buffer=true){
  ipc::IpcRequest request{offset,size,0,{}};
  if(buffer)request.outputBuf.push_back(out);
  return storage.Read(session,request,response);
 };
 assert(read(4,2)==0&&out[0]==5&&out[1]==6);
 for(size_t i=2;i<out.size();++i)assert(out[i]==0xaa);
 assert(read(-1,1)==result::InvalidOffset);
 assert(read(0,-1)==result::InvalidSize);
 assert(read(7,0)==result::InvalidOffset);
 assert(read(5,2)==result::InvalidSize);
 assert(read(6,0,false)==0);
 assert(read(0,1,false)==result::InvalidSize);
 storage.backing->shortRead=true;
 assert(read(0,1)==result::UnexpectedFailure);
}
'''.replace("METHOD", method)
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)
    (path/"test.cpp").write_text(source)
    subprocess.run([os.environ.get("CXX","g++"),"-std=c++20","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer",str(path/"test.cpp"),"-o",str(path/"test")],check=True)
    subprocess.run([str(path/"test")],check=True)
print("PASS: requested read size, buffer bounds, EOF, negative values and short reads")
