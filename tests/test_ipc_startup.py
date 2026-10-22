#!/usr/bin/env python3
"""Exercise production session cloning/closing and the HOS 20 applet event ABI."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
base = root / 'app/src/main/cpp/skyline'
session = (base / 'kernel/types/KSession.h').read_text().replace('#include "KSyncObject.h"', '')
manager = (base / 'services/serviceman.cpp').read_text()
close = manager[manager.index('    void ServiceManager::CloseSession('):manager.index('    void ServiceManager::SyncRequestHandler(')]
clone = manager[manager.index('                        case ipc::ControlCommand::CloneCurrentObject:'):manager.index('                        case ipc::ControlCommand::QueryPointerBufferSize:')]
clone = clone[clone.index('                            auto clone{'):clone.index('                            break;')]
am = (base / 'services/am/controller/IApplicationFunctions.cpp').read_text()
constructor = am[am.index('    IApplicationFunctions::IApplicationFunctions('):am.index('    Result IApplicationFunctions::PopLaunchParameter(')]
event = am[am.index('    Result IApplicationFunctions::GetUnknownEvent('):am.index('    Result IApplicationFunctions::GetNotificationStorageChannelEvent(')]
assert 'SFUNC(0xD2, IApplicationFunctions, GetUnknownEvent)' in (base / 'services/am/controller/IApplicationFunctions.h').read_text()
source = r'''
#include <cassert>
#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>
using KHandle=uint32_t;
using Result=int;
namespace skyline {
struct Process;
struct DeviceState {std::shared_ptr<Process> process;};
namespace service {struct BaseService {BaseService()=default; template<class T> BaseService(const DeviceState&,T&){}; virtual ~BaseService()=default;};}
namespace kernel::type {
enum class KType {KSession};
struct KSyncObject {const DeviceState &state; bool signalled=false; std::vector<int> syncObjectWaiters;
 KSyncObject(const DeviceState &s,KType):state(s){}; virtual ~KSyncObject()=default;};
struct KEvent:KSyncObject {KEvent(const DeviceState &s,bool b):KSyncObject(s,KType::KSession){signalled=b;}};
}
}
SESSION
namespace skyline {
namespace type=kernel::type;
struct Process {
 std::map<KHandle,std::shared_ptr<type::KSyncObject>> handles; KHandle next=0xd000;
 template<class T> KHandle InsertItem(std::shared_ptr<T> &p){handles.emplace(next,p);return next++;}
 template<class T> std::shared_ptr<T> GetHandle(KHandle h){return std::dynamic_pointer_cast<T>(handles.at(h));}
};
namespace ipc {struct IpcRequest{};struct IpcResponse{std::vector<KHandle> copyHandles,moveHandles;};}
namespace service {
struct ServiceManager {
 const DeviceState &state; std::mutex mutex; std::map<int,std::shared_ptr<BaseService>> serviceMap;
 explicit ServiceManager(const DeviceState&s):state(s){}
 void CloseSession(KHandle handle);
 KHandle Clone(KHandle h){auto session=state.process->GetHandle<type::KSession>(h);ipc::IpcResponse response;
CLONE
 return response.moveHandles.at(0);}
};
CLOSE
namespace am {
struct IApplicationFunctions:BaseService {
 const DeviceState &state;
 std::shared_ptr<type::KEvent> gpuErrorEvent,friendInvitationStorageChannelEvent,notificationStorageChannelEvent,unknownEvent;
 IApplicationFunctions(const DeviceState&,ServiceManager&);
 Result GetUnknownEvent(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);
};
CONSTRUCTOR
EVENT
}
}
}
int main(){
 using namespace skyline;
 DeviceState state{std::make_shared<Process>()};service::ServiceManager manager(state);
 std::shared_ptr<service::BaseService> obj=std::make_shared<service::BaseService>();
 for(bool domain:{false,true}) for(bool closeOriginal:{false,true}) {
  auto original=std::make_shared<type::KSession>(state,obj);
  if(domain) assert(original->ConvertDomain()==0);
  original->signalled=true;original->syncObjectWaiters.push_back(42);
  auto h=state.process->InsertItem(original);
  auto c=manager.Clone(h);auto cloned=state.process->GetHandle<type::KSession>(c);
  assert(original!=cloned && original->serviceObject==cloned->serviceObject);
  assert(!cloned->signalled && cloned->syncObjectWaiters.empty());
  assert(cloned->isDomain==domain);
  if(domain) {
   assert(cloned->domainState==original->domainState);
   cloned->domainState->objects.push_back(obj);
   auto id=cloned->domainState->nextHandle++;
   assert(id==1 && original->domainState->objects.at(id)==obj);
   original->domainState->objects.at(id).reset();
   assert(!cloned->domainState->objects.at(id));
  }
  manager.CloseSession(closeOriginal?h:c);
  assert((closeOriginal?cloned:original)->isOpen);
  assert(!(closeOriginal?original:cloned)->isOpen);
  manager.CloseSession(closeOriginal?h:c); // Closing twice must not affect the survivor.
  assert((closeOriginal?cloned:original)->isOpen);
 }
 // A clone made before conversion keeps its ordinary CMIF protocol.
 auto original=std::make_shared<type::KSession>(state,obj);
 auto cloned=original->Clone();original->ConvertDomain();assert(!cloned->isDomain&&!cloned->domainState);
 // A genuine kernel handle alias still addresses the same session.
 auto h=state.process->InsertItem(cloned),alias=state.process->InsertItem(cloned);
 manager.CloseSession(alias);assert(!state.process->GetHandle<type::KSession>(h)->isOpen);
 service::am::IApplicationFunctions app(state,manager);ipc::IpcRequest req;ipc::IpcResponse a,b;
 assert(app.GetUnknownEvent(*original,req,a)==0 && a.copyHandles.size()==1 && a.moveHandles.empty());
 assert(app.GetUnknownEvent(*original,req,b)==0 && b.copyHandles.size()==1);
 auto first=state.process->GetHandle<type::KEvent>(a.copyHandles[0]);
 assert(first && !first->signalled && first==state.process->GetHandle<type::KEvent>(b.copyHandles[0]));
 state.process->handles.erase(a.copyHandles[0]);
 assert(state.process->GetHandle<type::KEvent>(b.copyHandles[0])==first);
}
'''.replace('SESSION', session).replace('CLONE',clone).replace('CLOSE',close).replace('CONSTRUCTOR',constructor.replace(': BaseService(state, manager),',': BaseService(state, manager), state(state),')).replace('EVENT',event)
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.cpp').write_text(source)
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS: independent CMIF session lifetime, shared domain IDs, command 210 event handles')
