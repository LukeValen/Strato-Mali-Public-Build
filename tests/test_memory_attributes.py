#!/usr/bin/env python3
"""Run production memory edits and SVC validation against real chunk maps."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
base = root / "app/src/main/cpp/skyline/kernel"
header = (base / "memory.h").read_text()
cpp = (base / "memory.cpp").read_text()
svc = (base / "svc.cpp").read_text()
types = header[header.index("    namespace memory {"):header.index("        /**\n         * @brief A memory region")]
functions = cpp[cpp.index("    void MemoryManager::MapInternal("):cpp.index("    void MemoryManager::ForeachChunkInRange(")]
functions += cpp[cpp.index("    bool MemoryManager::CollectSvcMemoryRange("):cpp.index("    void MemoryManager::SetRegionBorrowed(")]
functions += cpp[cpp.index("    bool MemoryManager::SvcMapMemory("):cpp.index("    void MemoryManager::SvcUnmapMemory(")]
method = svc[svc.index("    void SetMemoryAttribute("):svc.index("    void MapMemory(")]
source = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>
#include <sys/mman.h>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using Result=int;
template<class T>using span=std::span<T>;
#define LOGW(...) ((void)0)
#define LOGD(...) ((void)0)
namespace skyline {
TYPES
class MemoryManager {
public:
 std::map<u8*,ChunkDescriptor> chunks;
 std::shared_mutex mutex;
 bool AddressSpaceContains(span<u8> s) {return !s.empty() && chunks.begin()->first<=s.data() && s.data()+s.size()<=chunks.rbegin()->first;}
 span<u8> GetHostSpan(span<u8> s){return s;}
 void MapInternal(const std::pair<u8*,ChunkDescriptor>&);
 bool CollectSvcMemoryRange(span<u8>,u32,u8,std::vector<std::pair<u8*,ChunkDescriptor>>&);
 bool SvcSetMemoryAttribute(span<u8>,u32,u32);
 bool SvcSetMemoryPermission(span<u8>,memory::Permission);
 bool SvcMapMemory(span<u8>,span<u8>);
 bool SvcMapTransferMemory(span<u8>,memory::Permission,ChunkDescriptor&,const std::function<void()>&);
};
FUNCTIONS
namespace svc {
namespace result {constexpr int InvalidAddress=1,InvalidSize=2,InvalidCurrentMemory=3,InvalidCombination=4;}
namespace util {
 bool IsPageAligned(u64 v){return (v&0xfff)==0;}
 bool IsPageAligned(u8 *p){return IsPageAligned(reinterpret_cast<u64>(p));}
}
struct SvcContext {u64 x0{},x1{};u32 w2{},w3{};int w0=-1;};
struct Process {MemoryManager memory;};
struct DeviceState {Process *process;};
METHOD
}
}}
int main(){
 using namespace skyline;using namespace skyline::kernel;
 alignas(4096) static u8 bytes[0x10000]{};
 svc::Process process;auto &m=process.memory;
 auto reset=[&](memory::MemoryState state=memory::states::CodeMutable){
  m.chunks.clear();
  ChunkDescriptor d{};d.state=state;d.permission=memory::Permission{true,true,false};d.size=sizeof(bytes);
  m.chunks[bytes]=d;d.size=0;m.chunks[bytes+sizeof(bytes)]=d;
 };
 auto attr=[&](size_t off){return std::prev(m.chunks.upper_bound(bytes+off))->second.attributes.value;};
 auto query=[&](u64 address,u64 size,u32 mask,u32 value){
  svc::SvcContext ctx{address,size,mask,value};svc::DeviceState state{&process};svc::SetMemoryAttribute(state,ctx);return ctx.w0;
 };
 const u64 addr=reinterpret_cast<u64>(bytes);
 reset();
 assert(query(addr+0x1000,0x2000,0x10,0x10)==0);
 assert(attr(0)==0&&attr(0x1000)==0x10&&attr(0x2fff)==0x10&&attr(0x3000)==0);
 assert(query(addr+0x1000,0x2000,0x10,0x10)==0); // Idempotent lock.
 assert(query(addr+0x1000,0x2000,0x10,0)==svc::result::InvalidCombination);
 assert(!m.SvcSetMemoryPermission({bytes+0x1000,0x2000},memory::Permission{true,false,false}));
 assert(!m.SvcSetMemoryPermission({bytes,0x3000},memory::Permission{true,true,false}));
 assert(attr(0)==0&&attr(0x1000)==0x10); // No partial mutation.
 // Locking must not be bypassed through stack aliases or transfer mappings.
 assert(!m.SvcMapMemory({bytes,0x3000},{bytes+0x8000,0x3000}));
 ChunkDescriptor original{};bool mapped=false;
 assert(!m.SvcMapTransferMemory({bytes,0x3000},memory::Permission{true,true,false},original,[&]{mapped=true;}));
 assert(!mapped);
 assert(query(addr,0x4000,8,8)==0); // Preserve locks while changing caching.
 assert(attr(0)==8&&attr(0x1000)==0x18&&attr(0x3000)==8);
 assert(query(addr,0x4000,8,0)==0);
 assert(attr(0)==0&&attr(0x1000)==0x10);
 assert(query(addr,0x4000,0x18,0x18)==0);
 assert(attr(0)==0x18&&attr(0x3000)==0x18);
 reset(memory::states::Heap);
 assert(query(addr,0x1000,0x10,0x10)==svc::result::InvalidCurrentMemory);
 assert(attr(0)==0);
 assert(query(addr,0x1000,8,8)==0);assert(attr(0)==8);
 reset(memory::states::Code);
 assert(query(addr,0x1000,0x10,0x10)==svc::result::InvalidCurrentMemory);
 reset(memory::states::AliasCodeData);assert(query(addr,0x1000,0x10,0x10)==0);
 reset();
 for(u32 bits:{1U,2U,4U,0x20U,0x100U,0x80000000U}) {
  assert(query(addr,0x1000,bits,bits)==svc::result::InvalidCombination);
 }
 assert(query(addr,0x1000,0,8)==svc::result::InvalidCombination);
 assert(query(addr,0x1000,0,0)==0);
 assert(query(addr+1,0x1000,16,16)==svc::result::InvalidAddress);
 assert(query(addr,0,16,16)==svc::result::InvalidSize);
 assert(query(addr,1,16,16)==svc::result::InvalidSize);
 assert(query(~0ULL-0xfff,0x2000,16,16)==svc::result::InvalidCurrentMemory);
 // Reject unsupported attributes or a different state/permission anywhere.
 for(int mode=0;mode<4;++mode) {
  reset();auto second=m.chunks[bytes];m.chunks[bytes].size=0x2000;
  second.size=sizeof(bytes)-0x2000;
  if(mode==0)second.state=memory::states::Heap;
  if(mode==1)second.permission=memory::Permission{true,false,false};
  if(mode==2)second.attributes.value=1;
  if(mode==3)second.attributes.value=2;
  m.chunks[bytes+0x2000]=second;
  assert(query(addr,0x3000,16,16)==svc::result::InvalidCurrentMemory);
  assert(attr(0)==0);
 }
 reset();auto second=m.chunks[bytes];m.chunks[bytes].size=0x2000;
 second.size=sizeof(bytes)-0x2000;second.attributes.value=4;m.chunks[bytes+0x2000]=second;
 assert(query(addr,0x3000,16,16)==0);assert(attr(0)==16&&attr(0x2000)==20);
 // Existing unlocked permission and transfer operations remain available.
 reset(memory::states::Heap);
 assert(m.SvcSetMemoryPermission({bytes,0x1000},memory::Permission{true,false,false}));
 reset(memory::states::Heap);
 assert(m.SvcMapTransferMemory({bytes,0x1000},memory::Permission{true,true,false},original,[&]{mapped=true;}));
 assert(mapped&&attr(0)==1&&original.state==memory::states::Heap);
}
'''.replace("TYPES", types).replace("FUNCTIONS", functions).replace("METHOD", method)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / "test.cpp").write_text(source)
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: memory attribute SVC, sticky locks, whole-range validation and protected mutations")
