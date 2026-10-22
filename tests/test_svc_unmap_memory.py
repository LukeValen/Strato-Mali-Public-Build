#!/usr/bin/env python3
"""Exercise the production SvcUnmapMemory on split, invalid and repeated maps."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
header = (root / "app/src/main/cpp/skyline/kernel/memory.h").read_text()
cpp = (root / "app/src/main/cpp/skyline/kernel/memory.cpp").read_text()
types = header[header.index("    namespace memory {"):header.index("        /**\n         * @brief A memory region")]
mapping = cpp[cpp.index("    void MemoryManager::MapInternal("):cpp.index("    void MemoryManager::ForeachChunkInRange(")]
collect = cpp[cpp.index("    bool MemoryManager::CollectSvcMemoryRange("):cpp.index("    bool MemoryManager::SvcSetMemoryAttribute(")]
unmap = cpp[cpp.index("    bool MemoryManager::SvcUnmapMemory("):cpp.index("    void MemoryManager::AddRef(")]
source = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>
#include <sys/mman.h>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
template<class T>using span=std::span<T>;
#define LOGW(...) ((void)0)
#define mprotect(...) 0
namespace skyline {
TYPES
class MemoryManager {
public:
 std::map<u8*,ChunkDescriptor> chunks;
 std::shared_mutex mutex;
 span<u8> GetHostSpan(span<u8> s){return s;}
 void MapInternal(const std::pair<u8*,ChunkDescriptor>&);
 bool CollectSvcMemoryRange(span<u8>,u32,u8,std::vector<std::pair<u8*,ChunkDescriptor>>&);
 bool SvcUnmapMemory(span<u8>,span<u8>);
};
MAPPING
COLLECT
UNMAP
}}
int main(){
 using namespace skyline;using namespace skyline::kernel;
 alignas(4096) static u8 bytes[0x10000]{};
 MemoryManager m;
 auto borrowed=[](size_t size,u8 attr){
  ChunkDescriptor d{};d.size=size;d.state=memory::states::Heap;
  d.permission={false,false,false};d.attributes.isBorrowed=true;
  d.attributes.value |= attr;return d;
 };
 auto stack=borrowed(0x4000,0);
 stack.state=memory::states::Stack;stack.permission={true,true,false};
 stack.attributes.isBorrowed=false;
 auto hole=borrowed(0x4000,0);hole.state=memory::states::Unmapped;
 hole.attributes.isBorrowed=false;
 auto end=hole;end.state=memory::states::Reserved;end.size=0;
 m.chunks={{bytes,borrowed(0x1000,0)},
           {bytes+0x1000,borrowed(0x3000,8)},
           {bytes+0x4000,hole},
           {bytes+0x8000,stack},
           {bytes+0xc000,hole},
           {bytes+0x10000,end}};
 std::memset(bytes+0x8000,0x5a,0x4000);
 assert(!m.SvcUnmapMemory({bytes,0x4000},{bytes+0x4000,0x4000}));
 assert(m.chunks.at(bytes).attributes.isBorrowed); // Reject invalid destination without mutation.
 assert(!m.SvcUnmapMemory({bytes,0x5000},{bytes+0x8000,0x5000}));
 assert(m.chunks.at(bytes).attributes.isBorrowed); // Reject range past source mapping.
 assert(m.SvcUnmapMemory({bytes,0x4000},{bytes+0x8000,0x4000}));
 for(size_t i=0;i<0x4000;++i)assert(bytes[i]==0x5a);
 assert(!m.chunks.at(bytes).attributes.isBorrowed);
 assert(!m.chunks.at(bytes+0x1000).attributes.isBorrowed);
 assert(m.chunks.at(bytes).permission==memory::Permission(true,true,false));
 assert(m.chunks.at(bytes+0x1000).permission==memory::Permission(true,true,false));
 assert(!m.SvcUnmapMemory({bytes,0x4000},{bytes+0x8000,0x4000})); // Repeated unmap.
}
'''.replace("TYPES", types).replace("MAPPING", mapping).replace("COLLECT", collect).replace("UNMAP", unmap)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / "test.cpp").write_text(source)
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: complete stack unmap, split source, invalid and repeated ranges")
