#!/usr/bin/env python3
"""Run the production wait SVC against valid, deleted, and out-of-range handles."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
text=(root/'app/src/main/cpp/skyline/kernel/svc.cpp').read_text()
method=text[text.index('    void WaitSynchronization('):text.index('    void CancelSynchronization(')]
source=r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
using u8=uint8_t;using u32=uint32_t;using i64=int64_t;using KHandle=u32;using Result=int;
using std::span;
namespace result {constexpr int OutOfRange=1,InvalidHandle=2,Cancelled=3,TimedOut=4;}
struct exception:std::runtime_error {template<class...T>exception(const char*s,T...):std::runtime_error(s){}};
namespace fmt {template<class...T>std::string format(T...){return {};};auto runtime(const char*s){return s;}}
#define LOGD(...) ((void)0)
// Evaluate trace arguments too: zero-handle waits must not dereference an empty span.
#define TRACE_EVENT_FMT(category,format,handle) ((void)(handle))
struct AsyncLogger {enum class LogLevel{Debug};static bool CheckLogLevel(LogLevel){return false;}};
namespace type {
enum class KType{KProcess,KThread,KEvent,KSession,KOther};struct KThread;
struct KSyncObject {KType objectType=KType::KEvent;bool signalled=false;
 inline static std::mutex syncObjectMutex;std::list<std::shared_ptr<KThread>> syncObjectWaiters;};
struct KThread:KSyncObject {
 bool cancelSync=false,isCancellable=false;KSyncObject*wakeObject=nullptr;std::atomic<int>priority{10};
 static bool IsHigherPriority(int,const std::shared_ptr<KThread>&){return false;}
};
}
struct Process {
 std::vector<std::shared_ptr<type::KSyncObject>> handles;
 auto GetHandle(KHandle h){auto p=handles.at(h-0xd000);if(!p)throw std::out_of_range("deleted");return p;}
};
struct Scheduler {void RemoveThread(){};void TimedWaitSchedule(std::chrono::nanoseconds){};void WaitSchedule(bool=true){};void InsertThread(std::shared_ptr<type::KThread>){};};
struct DeviceState {std::shared_ptr<Process>process=std::make_shared<Process>();std::shared_ptr<type::KThread>thread=std::make_shared<type::KThread>();std::shared_ptr<Scheduler>scheduler=std::make_shared<Scheduler>();};
struct SvcContext {u32 w0=999,w1=999,w2=0;uintptr_t x1=0;uint64_t x3=0;};
METHOD
int main(){
 DeviceState s;auto e=std::make_shared<type::KSyncObject>();s.process->handles={e,nullptr};
 auto wait=[&](std::vector<KHandle> hs){SvcContext c;c.x1=reinterpret_cast<uintptr_t>(hs.data());c.w2=hs.size();WaitSynchronization(s,c);return c;};
 for(auto h:{0U,1U,0xcfffU,0xd001U,0xd002U,0xffffffffU}) {auto c=wait({h});assert(c.w0==result::InvalidHandle&&c.w1==999);assert(e->syncObjectWaiters.empty());}
 assert(wait({0xd000,0xd001}).w0==result::InvalidHandle);
 assert(wait({0xd000}).w0==result::TimedOut);
 assert(wait({}).w0==result::TimedOut); // valid poll with zero handles
 e->signalled=true;auto c=wait({0xd000});assert(c.w0==0&&c.w1==0);
 assert(wait({0xd000,0xd001}).w0==result::InvalidHandle); // validate all handles before polling
 s.thread->cancelSync=true;assert(wait({}).w0==result::Cancelled&&!s.thread->cancelSync);
 e->objectType=type::KType::KOther;assert(wait({0xd000}).w0==result::InvalidHandle);
 assert(wait(std::vector<KHandle>(65,0xd000)).w0==result::OutOfRange);
}
'''.replace('METHOD',method)
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(source)
 subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: invalid/deleted/type-invalid handles, zero-handle polling, cancellation, signalled events')
