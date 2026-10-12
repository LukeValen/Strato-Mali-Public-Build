#!/usr/bin/env python3
"""Compile production Host1x Submit and exercise address patching and failure atomicity."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
text = (root / "app/src/main/cpp/skyline/services/nvdrv/devices/nvhost/host1x_channel.cpp").read_text()
method = text[text.index("    PosixResult Host1xChannel::Submit("):text.index("    PosixResult Host1xChannel::GetSyncpoint(")]
source = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <soc/host1x/command_decoder.h>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
using std::span;
#define LOGD(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)
namespace skyline::service::nvdrv::device::nvhost {
enum class PosixResult {Success,InvalidArgument,TryAgain};
namespace soc::host1x {constexpr size_t SyncpointCount=192;using ::skyline::soc::host1x::DecodeCommands;}
namespace core {
struct NvMap {
 struct Handle {
  using Id=u32; std::mutex mutex;bool allocated=true;u64 address,size=64,alignedSize=4096;u32 iova;int pins=0;
  std::array<u32,16> words{};
  Handle(u32 iova):address(reinterpret_cast<u64>(words.data())),iova(iova){words.fill(0xcccccccc);}
 };
 std::unordered_map<u32,std::shared_ptr<Handle>> handles;
 int calls=0,failAfter=-1;bool throwing=false;
 std::shared_ptr<Handle> GetHandle(u32 id){auto it=handles.find(id);return it==handles.end()?nullptr:it->second;}
 u32 PinHandle(u32 id){if(calls++==failAfter){if(throwing)throw std::runtime_error("pin failure");return 0;}auto h=GetHandle(id);++h->pins;return h->iova;}
 void UnpinHandle(u32 id){auto h=GetHandle(id);assert(h->pins>0);--h->pins;}
};
}
struct Syncpoint {u32 value=0;void Increment(){++value;}};
struct QueuedChannel {
 struct Job {std::shared_ptr<void> pins; std::vector<std::vector<u32>> gathers;};
 std::vector<Job> jobs;
 void Push(std::vector<std::vector<u32>> gathers,std::shared_ptr<void> pins){jobs.push_back({std::move(pins),std::move(gathers)});}
 void Complete(){jobs.clear();}
};
struct Host {std::array<Syncpoint,192> syncpoints; std::array<QueuedChannel,14> channels;};
struct Soc {Host host1x;};
struct State {std::shared_ptr<Soc> soc=std::make_shared<Soc>();};
struct Manager {
 u32 maximum=0;
 bool IsSyncpointAllocated(u32 id){assert(id<192);return id==54;}
 u32 IncrementSyncpointMaxExt(u32 id,u32 n){assert(id==54);return maximum+=n;}
};
struct Core {core::NvMap nvMap;Manager syncpointManager;};
struct Host1xChannel {
 struct SubmitCmdBuf {u32 mem,offset,words;};
 struct SubmitReloc {u32 patchMem,patchOffset,pinMem,pinOffset;};
 struct SubmitSyncpointIncr {u32 syncpointId,numIncrs;};
 Core core;State state;std::mutex channelMutex;u32 channelType=3;bool executionWarningShown=false;
 u32 videoTraceClass=1,videoTraceSubmits=0,videoTraceMethods=0;std::array<u32,2> videoTraceStoredMethods{};
 PosixResult Submit(span<SubmitCmdBuf>,span<SubmitReloc>,span<u32>,span<SubmitSyncpointIncr>,span<u32>);
};
METHOD
}
int main(){
 using namespace skyline::service::nvdrv::device::nvhost;
 Host1xChannel channel;
 auto cmd=std::make_shared<core::NvMap::Handle>(0x1000),target=std::make_shared<core::NvMap::Handle>(0x12345000);
 channel.core.nvMap.handles={{4,cmd},{8,target}};
 // Existing MapBuffer ownership must survive a submit.
 target->pins=1;
 std::vector<Host1xChannel::SubmitCmdBuf> cmds{{4,8,6}};
 std::vector<Host1xChannel::SubmitReloc> relocs{{4,12,8,20},{4,16,8,32}};
 std::vector<u32> shifts{0,8},fences{0xdeadbeef};
 std::vector<Host1xChannel::SubmitSyncpointIncr> sync{{54,1}};
 auto submit=[&]{channel.core.nvMap.calls=0;return channel.Submit(cmds,relocs,shifts,sync,fences);};
 auto unchanged=[&]{auto old=cmd->words;auto fence=fences;auto count=channel.core.syncpointManager.maximum;
  assert(submit()!=PosixResult::Success);assert(cmd->words==old&&fences==fence&&count==channel.core.syncpointManager.maximum);assert(cmd->pins==0&&target->pins==1);};
 assert(submit()==PosixResult::Success);
 assert(cmd->words[3]==0x12345014&&cmd->words[4]==0x123450);
 assert(cmd->words[2]==0xcccccccc&&cmd->words[5]==0xcccccccc);
 assert(fences[0]==1&&channel.state.soc->host1x.syncpoints[54].value==0);
 assert(cmd->pins==1&&target->pins==2&&channel.core.nvMap.calls==2);
 auto& queue=channel.state.soc->host1x.channels[3];
 assert(queue.jobs.size()==1&&queue.jobs[0].gathers[0][1]==0x12345014);
 cmd->words[3]=0;
 assert(queue.jobs[0].gathers[0][1]==0x12345014); // Private snapshot cannot be changed by guest reuse.
 queue.Complete();
 assert(cmd->pins==0&&target->pins==1);
 for(int i=0;i<50;++i){assert(submit()==PosixResult::Success);queue.Complete();}
 assert(cmd->pins==0&&target->pins==1);
 relocs[1].pinMem=99;unchanged();relocs[1].pinMem=8;
 relocs[1].patchMem=8;unchanged();relocs[1].patchMem=4;
 relocs[1].patchOffset=64;unchanged();relocs[1].patchOffset=16;
 relocs[1].patchOffset=0;unchanged();relocs[1].patchOffset=16;
 relocs[1].patchOffset=17;unchanged();relocs[1].patchOffset=16;
 relocs[1].pinOffset=64;unchanged();relocs[1].pinOffset=32;
 shifts[1]=32;unchanged();shifts[1]=8;
 shifts.pop_back();unchanged();shifts.push_back(8);
 cmds[0].words=UINT32_MAX;unchanged();cmds[0].words=6;
 cmds[0].offset=UINT32_MAX;unchanged();cmds[0].offset=8;
 target->allocated=false;unchanged();target->allocated=true;
 target->address=UINT64_MAX-2;unchanged();target->address=reinterpret_cast<u64>(target->words.data());
 sync[0].syncpointId=192;unchanged();sync[0].syncpointId=54;
 sync[0].syncpointId=53;unchanged();sync[0].syncpointId=54;
 fences.push_back(0);unchanged();fences.pop_back();
 target->iova=0xfffffff0;unchanged();target->iova=0x12345000;
 channel.core.nvMap.failAfter=1;unchanged();
 channel.core.nvMap.throwing=true;bool threw=false;
 try{submit();}catch(const std::runtime_error&){threw=true;}
 assert(threw&&cmd->pins==0&&target->pins==1);
 channel.core.nvMap.throwing=false;channel.core.nvMap.failAfter=-1;
 relocs.clear();shifts.clear();assert(submit()==PosixResult::Success);
 cmds.clear();sync.clear();fences.clear();assert(submit()==PosixResult::Success);
 queue.Complete();assert(cmd->pins==0&&target->pins==1);
}
'''.replace("METHOD", method)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / "test.cpp").write_text(source)
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-g", "-pthread",
                    "-I" + str(root / "app/src/main/cpp/skyline"),
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: Host1x relocation addresses/shifts, gather bounds, failure atomicity, deferred fences, immutable gathers and asynchronous pin ownership")
