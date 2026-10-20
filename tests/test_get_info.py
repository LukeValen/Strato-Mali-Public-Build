#!/usr/bin/env python3
"""Exercise the production GetInfo dispatcher with a minimal kernel fixture."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
text = (root / "app/src/main/cpp/skyline/kernel/svc.cpp").read_text()
method = text[text.index("    void GetInfo("):text.index("    void MapPhysicalMemory(")]
source = r'''
#include <algorithm>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
using u32=uint32_t; using u64=uint64_t; using KHandle=u32; using Result=int;
struct exception : std::runtime_error { using std::runtime_error::runtime_error; };
namespace result { constexpr int InvalidCombination=1, InvalidHandle=2, InvalidEnumValue=3; }
namespace util { u64 GetTimeTicks(){return 123;} }
#define LOGW(...) ((void)0)
#define LOGD(...) ((void)0)
struct Region {
 Region &guest=*this;
 unsigned char *data() const {return reinterpret_cast<unsigned char *>(0x100000);}
 u64 size() const {return 0x1000000000ULL;}
};
struct Memory {
 Region alias,heap,base,stack;
 u64 GetUserMemoryUsage(){return 100;}
 u64 GetSystemResourceUsage(){return 20;}
};
namespace type {
struct KProcess {
 Memory memory;
 struct {
  struct {std::bitset<4> coreMask{7};struct {u64 Mask(){return 0xff;}} priority;} threadInfo;
  struct {u64 systemResourceSize=0;} meta;
  struct {u64 programId=0x123;} aci0;
 } npdm;
 void *tlsExceptionContext=nullptr;
 u64 freeThreadCount=255;
 u64 GetFreeThreadCount(){return freeThreadCount;}
 int lookups=0;
 template<class T> std::shared_ptr<T> GetHandle(KHandle handle) {
  ++lookups;
  if(handle==0xffff8001 || handle==0x100) return std::shared_ptr<T>(this,[](T*){});
  if(handle==0x101) throw exception("wrong kernel object type");
  if(handle==0x102) throw exception("deleted handle");
  throw std::out_of_range("invalid handle");
 }
};
}
struct DeviceState {std::shared_ptr<type::KProcess> process=std::make_shared<type::KProcess>();};
struct SvcContext {int w0=-1; union {u64 x1=0;u32 w1;};u32 w2=0;u64 x3=0;};
METHOD
int main(){
 DeviceState state;
 auto query=[&](u32 id,u32 handle,u64 subtype){SvcContext ctx;ctx.w1=id;ctx.w2=handle;ctx.x3=subtype;GetInfo(state,ctx);return ctx;};
 for(auto handle:{0xffff8001U,0x100U}) {
  auto ctx=query(28,handle,0);assert(ctx.w0==0&&ctx.x1==0);
  ctx=query(24,handle,0);assert(ctx.w0==0&&ctx.x1==255);
 }
 for(auto handle:{0U,0xffffffffU,0xffff8000U,0x101U,0x102U}) {
  auto ctx=query(28,handle,0);assert(ctx.w0==result::InvalidHandle&&ctx.x1==28);
  ctx=query(24,handle,0);assert(ctx.w0==result::InvalidHandle&&ctx.x1==24);
 }
 for(u64 subtype:{1ULL,0x100000000ULL,~0ULL}) {
  int before=state.process->lookups;
  auto ctx=query(28,0,subtype);
  assert(ctx.w0==result::InvalidCombination&&ctx.x1==28);
  assert(state.process->lookups==before);
  ctx=query(24,0,subtype);
  assert(ctx.w0==result::InvalidCombination&&ctx.x1==24);
  assert(state.process->lookups==before);
 }
 // Existing alias queries must retain the original reserved size.
 auto alias=query(3,0xffff8001,0);assert(alias.w0==0&&alias.x1==0x1000000000ULL);
 auto unknown=query(0xdead,0xffff8001,0);assert(unknown.w0==result::InvalidEnumValue);
}
'''.replace("METHOD", method)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / "test.cpp").write_text(source)
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: GetInfo(24/28), subtype precedence, process handles and existing queries")
