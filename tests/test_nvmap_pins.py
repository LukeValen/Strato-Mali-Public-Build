#!/usr/bin/env python3
"""Exercise production nvmap pin/reclaim logic under SMMU exhaustion."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
text = (root / "app/src/main/cpp/skyline/services/nvdrv/core/nvmap.cpp").read_text()
methods = text[text.index("    void NvMap::UnmapHandle("):text.index("    bool NvMap::TryRemoveHandle(")]
methods += text[text.index("    u32 NvMap::PinHandle("):text.index("    std::optional<NvMap::FreeInfo> NvMap::FreeHandle(")]
source = r'''
#include <cassert>
#include <cstdint>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
#define LOGW(...) ((void)0)
namespace skyline::service::nvdrv::core {
struct Allocator {
 bool used=false;
 u32 Allocate(u32 size){assert(size==4096);if(used)return 0;used=true;return 4096;}
 void Free(u32 address,u32 size){assert(address==4096&&size==4096&&used);used=false;}
};
struct Smmu {
 int mapped=0,unmapped=0;
 void Map(u32 address,u8 *data,u32 size){assert(address==4096&&data&&size==4096);++mapped;}
 void Unmap(u32 address,u32 size){assert(address==4096&&size==4096);++unmapped;}
};
struct Soc {Smmu smmu;};
struct State {std::shared_ptr<Soc> soc=std::make_shared<Soc>();};
class NvMap {
 public:
 struct Handle {
  using Id=u32;std::mutex mutex;bool allocated=true;u64 address=0x10000,alignedSize=4096;
  int pins=0;u32 pinVirtAddress=0;
  std::optional<std::list<std::shared_ptr<Handle>>::iterator> unmapQueueEntry;
 };
 State state;Allocator smmuAllocator;std::mutex unmapQueueLock;
 std::list<std::shared_ptr<Handle>> unmapQueue;
 std::unordered_map<u32,std::shared_ptr<Handle>> handles;
 std::shared_ptr<Handle> GetHandle(u32 id){auto it=handles.find(id);return it==handles.end()?nullptr:it->second;}
 void UnmapHandle(Handle&);u32 PinHandle(u32);void UnpinHandle(u32);
};
METHODS
}
int main(){
 using namespace skyline::service::nvdrv::core;
 NvMap map;
 auto a=std::make_shared<NvMap::Handle>(),b=std::make_shared<NvMap::Handle>();
 map.handles={{4,a},{8,b}};
 assert(map.PinHandle(99)==0);
 b->allocated=false;assert(map.PinHandle(8)==0);b->allocated=true;
 b->alignedSize=uint64_t(UINT32_MAX)+1;assert(map.PinHandle(8)==0);b->alignedSize=4096;
 b->pins=INT32_MAX;assert(map.PinHandle(8)==0);b->pins=0;
 assert(map.PinHandle(4)==4096&&a->pins==1);
 assert(map.PinHandle(4)==4096&&a->pins==2&&map.state.soc->smmu.mapped==1);
 // No evictable mappings: must fail without reading front() on an empty queue.
 assert(map.PinHandle(8)==0&&b->pins==0&&a->pins==2);
 map.UnpinHandle(4);map.UnpinHandle(4);
 assert(a->pins==0&&map.unmapQueue.size()==1);
 // Repinning reuses the existing mapping and removes the eviction entry.
 assert(map.PinHandle(4)==4096&&map.unmapQueue.empty());map.UnpinHandle(4);
 // A handle mutex held by FreeHandle must not deadlock against the queue mutex.
 std::promise<void> ready,release;auto done=release.get_future();
 std::thread worker([&]{std::unique_lock lock(a->mutex);ready.set_value();done.wait();});
 ready.get_future().wait();assert(map.PinHandle(8)==0);release.set_value();worker.join();
 assert(map.PinHandle(8)==4096&&b->pins==1&&a->pinVirtAddress==0);
 assert(map.unmapQueue.empty()&&!a->unmapQueueEntry&&map.state.soc->smmu.unmapped==1);
 map.UnpinHandle(8);assert(map.PinHandle(4)==4096);
 assert(b->pinVirtAddress==0&&map.state.soc->smmu.unmapped==2);
 map.UnpinHandle(4);
}
'''.replace("METHODS", methods)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / "test.cpp").write_text(source)
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-g", "-pthread",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=15)
print("PASS: nvmap pin counts, reuse, exhaustion, reclamation, contention and invalid allocations")
