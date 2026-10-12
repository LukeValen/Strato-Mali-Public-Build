// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <kernel/types/KProcess.h>
#include <loader/loader.h>
#include "IAddOnContentManager.h"
#include "IPurchaseEventManager.h"

namespace skyline::service::aocsrv {
    IAddOnContentManager::IAddOnContentManager(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager),
          addOnContentListChangedEvent(std::make_shared<type::KEvent>(state, false)) {}

    Result IAddOnContentManager::CountAddOnContent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u32>(static_cast<u32>(state.loader->addOnContent.size()));
        return {};
    }

    Result IAddOnContentManager::ListAddOnContent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto offset = request.Pop<u32>();
        auto count = request.Pop<u32>();
        if (request.outputBuf.empty()) return Result(2, 6001);
        auto output = request.outputBuf.at(0).cast<u32>();
        auto base = state.loader->nacp && state.loader->nacp->nacpContents.addOnContentBaseId ? state.loader->nacp->nacpContents.addOnContentBaseId : state.loader->programId + 0x1000;
        size_t written{}, index{};
        for (const auto &[id, backing] : state.loader->addOnContent) {
            if (index++ < offset) continue;
            if (written >= count || written >= output.size()) break;
            output[written++] = static_cast<u32>(id - base);
        }
        response.Push<u32>(static_cast<u32>(written));
        return {};
    }

    Result IAddOnContentManager::GetAddOnContentBaseId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto base = state.loader->nacp && state.loader->nacp->nacpContents.addOnContentBaseId ? state.loader->nacp->nacpContents.addOnContentBaseId : state.loader->programId + 0x1000;
        response.Push<u64>(base);
        return {};
    }

    Result IAddOnContentManager::PrepareAddOnContent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto index = request.Pop<u32>();
        auto base = state.loader->nacp && state.loader->nacp->nacpContents.addOnContentBaseId ? state.loader->nacp->nacpContents.addOnContentBaseId : state.loader->programId + 0x1000;
        return state.loader->addOnContent.contains(base + index) ? Result{} : Result(2, 1002);
    }

    Result IAddOnContentManager::GetAddOnContentListChangedEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        KHandle handle{state.process->InsertItem(addOnContentListChangedEvent)};
        LOGD("Add On Content List Changed Event Handle: 0x{:X}", handle);

        response.copyHandles.push_back(handle);
        return {};
    }

    Result IAddOnContentManager::GetAddOnContentListChangedEventWithProcessId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        KHandle handle{state.process->InsertItem(addOnContentListChangedEvent)};
        LOGD("Add On Content List Changed Event Handle: 0x{:X}", handle);

        response.copyHandles.push_back(handle);
        return {};
    }

    Result IAddOnContentManager::CheckAddOnContentMountStatus(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IAddOnContentManager::CreateEcPurchasedEventManager(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(IPurchaseEventManager), session, response);
        return {};
    }
}
