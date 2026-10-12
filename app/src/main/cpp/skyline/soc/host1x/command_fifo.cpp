// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/signal.h>
#include <nce.h>
#include <loader/loader.h>
#include <kernel/types/KProcess.h>
#include <soc.h>
#include "command_fifo.h"
#include "command_decoder.h"

namespace skyline::soc::host1x {
    ChannelCommandFifo::ChannelCommandFifo(const DeviceState &state, SyncpointSet &syncpoints, strato::video::Runtime &video)
        : state(state), gatherQueue(std::make_unique<CircularQueue<Job>>(GatherQueueSize)), videoChannel(video.ReserveChannel()),
          host1XClass(syncpoints), nvDecClass(syncpoints, video, videoChannel), vicClass(syncpoints, video, videoChannel) {}

    void ChannelCommandFifo::Send(ClassId targetClass, u32 method, u32 argument) {
        LOGV("Calling method in class: 0x{:X}, method: 0x{:X}, argument: 0x{:X}", targetClass, method, argument);

        switch (targetClass) {
            case ClassId::Host1x:
                host1XClass.CallMethod(method, argument);
                break;
            case ClassId::NvDec:
                nvDecClass.CallMethod(method, argument);
                break;
            case ClassId::VIC:
                vicClass.CallMethod(method, argument);
                break;
            default:
                LOGE("Sending method to unimplemented class: 0x{:X}", targetClass);
                break;
        }
    }

    void ChannelCommandFifo::Process(span<u32> gather) {
        DecodeCommands(gather, selectedClass, [this](u32 cls, u32 method, u32 argument) {
            Send(static_cast<ClassId>(cls), method, argument);
        });
    }

    void ChannelCommandFifo::Start() {
        std::scoped_lock lock(threadStartMutex);

        if (!gatherQueue)
            throw exception("Starting a stopped Host1x channel");
        if (!thread.joinable())
            thread = std::thread(&ChannelCommandFifo::Run, this);
    }

    void ChannelCommandFifo::Run() {
        if (int result{pthread_setname_np(pthread_self(), "ChannelCmdFifo")})
            LOGW("Failed to set the thread name: {}", strerror(result));
        AsyncLogger::UpdateTag();

        try {
            gatherQueue->Process([this](Job &job) {
                for (auto &gather : job.gathers)
                    Process(span<u32>(gather.data(), gather.size()));
                // Slots are raw storage. Pair Emplace with destruction after processing;
                // queued jobs are destroyed by CircularQueue if the worker is interrupted.
                std::destroy_at(&job);
            }, [] {});
        } catch (const signal::SignalException &e) {
            if (e.signal != SIGINT) {
                LOGE("{}\nStack Trace:{}", e.what(), state.loader->GetStackTrace(e.frames));
                signal::BlockSignal({SIGINT});
                state.process->Kill(false);
            }
        } catch (const exception &e) {
            LOGENF("{}\nStack Trace:{}", e.what(), state.loader->GetStackTrace(e.frames));
            signal::BlockSignal({SIGINT});
            state.process->Kill(false);
        } catch (const std::exception &e) {
            LOGE("{}", e.what());
            signal::BlockSignal({SIGINT});
            state.process->Kill(false);
        }
    }

    void ChannelCommandFifo::Push(std::vector<std::vector<u32>> gathers, std::shared_ptr<void> pins) {
        gatherQueue->Emplace(Job{std::move(pins), std::move(gathers)});
    }

    void ChannelCommandFifo::Stop() {
        std::scoped_lock lock(threadStartMutex);
        if (thread.joinable()) {
            pthread_kill(thread.native_handle(), SIGINT);
            thread.join();
        }
        gatherQueue.reset();
    }

    ChannelCommandFifo::~ChannelCommandFifo() {
        Stop();
    }
}

