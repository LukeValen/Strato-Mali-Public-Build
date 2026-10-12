// SPDX-License-Identifier: MIT OR MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <soc.h>
#include <soc/host1x/command_decoder.h>
#include <services/nvdrv/devices/deserialisation/deserialisation.h>
#include "host1x_channel.h"

namespace skyline::service::nvdrv::device::nvhost {
    Host1xChannel::Host1xChannel(const DeviceState &state,
                                 Driver &driver,
                                 Core &core,
                                 const SessionContext &ctx,
                                 core::ChannelType channelType)
        : NvDevice(state, driver, core, ctx),
          channelType(channelType) {
        state.soc->host1x.channels[static_cast<size_t>(channelType)].Start();
    }

    PosixResult Host1xChannel::SetNvmapFd(In<FileDescriptor> fd) {
        LOGD("fd: {}", fd);
        return PosixResult::Success;
    }

    PosixResult Host1xChannel::Submit(span<SubmitCmdBuf> cmdBufs,
                                      span<SubmitReloc> relocs, span<u32> relocShifts,
                                      span<SubmitSyncpointIncr> syncpointIncrs, span<u32> fenceThresholds) {
        LOGD("numCmdBufs: {}, numRelocs: {}, numSyncpointIncrs: {}, numFenceThresholds: {}",
                            cmdBufs.size(), relocs.size(), syncpointIncrs.size(), fenceThresholds.size());

        if (fenceThresholds.size() > syncpointIncrs.size() || relocs.size() != relocShifts.size())
            return PosixResult::InvalidArgument;

        std::scoped_lock lock(channelMutex);

        // Validate the entire request before changing command words or publishing fences.
        for (const auto &incr : syncpointIncrs)
            if (incr.syncpointId >= soc::host1x::SyncpointCount || !core.syncpointManager.IsSyncpointAllocated(incr.syncpointId))
                return PosixResult::InvalidArgument;

        using Handle = core::NvMap::Handle;
        std::unordered_map<Handle::Id, std::shared_ptr<Handle>> handles;
        auto getHandle = [&](Handle::Id id) -> std::shared_ptr<Handle> {
            auto found{handles.find(id)};
            if (found != handles.end())
                return found->second;
            auto handle{core.nvMap.GetHandle(id)};
            if (!handle)
                return nullptr;
            std::scoped_lock handleLock(handle->mutex);
            if (!handle->allocated || !handle->address || !handle->size ||
                handle->size > UINT32_MAX || handle->alignedSize > UINT32_MAX ||
                handle->address > UINT64_MAX - handle->size)
                return nullptr;
            handles.emplace(id, handle);
            return handle;
        };

        for (const auto &cmdBuf : cmdBufs) {
            auto handle{getHandle(cmdBuf.mem)};
            if (!handle || (cmdBuf.offset & 3) || cmdBuf.offset > handle->size ||
                static_cast<u64>(cmdBuf.words) * sizeof(u32) > handle->size - cmdBuf.offset)
                return PosixResult::InvalidArgument;
        }

        struct Patch { u8 *destination; Handle::Id target; u32 offset, shift, value; };
        std::vector<Patch> patches;
        patches.reserve(relocs.size());
        for (size_t i{}; i < relocs.size(); ++i) {
            const auto &reloc{relocs[i]};
            auto patchHandle{getHandle(reloc.patchMem)};
            auto pinHandle{getHandle(reloc.pinMem)};
            if (!patchHandle || !pinHandle || (reloc.patchOffset & 3) || relocShifts[i] >= 32 ||
                reloc.patchOffset > patchHandle->size || sizeof(u32) > patchHandle->size - reloc.patchOffset ||
                reloc.pinOffset >= pinHandle->size)
                return PosixResult::InvalidArgument;

            // Relocation offsets are relative to the nvmap allocation, not the gather start.
            bool inGather{std::any_of(cmdBufs.begin(), cmdBufs.end(), [&](const auto &cmdBuf) {
                return cmdBuf.mem == reloc.patchMem && reloc.patchOffset >= cmdBuf.offset &&
                       static_cast<u64>(reloc.patchOffset - cmdBuf.offset) + sizeof(u32) <= static_cast<u64>(cmdBuf.words) * sizeof(u32);
            })};
            if (!inGather)
                return PosixResult::InvalidArgument;
            patches.push_back({reinterpret_cast<u8 *>(patchHandle->address + reloc.patchOffset),
                               reloc.pinMem, reloc.pinOffset, relocShifts[i], 0});
        }

        // One temporary pin per distinct allocation. MapBuffer pins remain owned by the guest.
        // Transfer temporary pin ownership to the asynchronous channel job.
        struct SubmitPins {
            core::NvMap &nvMap;
            std::vector<Handle::Id> ids;
            ~SubmitPins() {
                for (auto id : ids)
                    nvMap.UnpinHandle(id);
            }
            explicit SubmitPins(core::NvMap &map) : nvMap(map) {}
        };
        auto pins{std::make_shared<SubmitPins>(core.nvMap)};
        pins->ids.reserve(handles.size());
        std::unordered_map<Handle::Id, u32> addresses;
        for (const auto &[id, handle] : handles) {
            u32 address{core.nvMap.PinHandle(id)};
            if (!address)
                return PosixResult::TryAgain;
            pins->ids.push_back(id);
            addresses.emplace(id, address);
        }
        for (auto &patch : patches) {
            u64 address{static_cast<u64>(addresses.at(patch.target)) + patch.offset};
            if (address > UINT32_MAX)
                return PosixResult::InvalidArgument;
            patch.value = static_cast<u32>(address) >> patch.shift;
        }
        for (const auto &patch : patches)
            std::memcpy(patch.destination, &patch.value, sizeof(patch.value));

        if (!patches.empty())
            LOGD("Applied {} Host1x relocations for channel {}", patches.size(), static_cast<u32>(channelType));

        std::vector<std::vector<u32>> gathers;
        gathers.reserve(cmdBufs.size());

        for (const auto &cmdBuf : cmdBufs) {
            const auto &handleDesc{handles.at(cmdBuf.mem)};

            u64 gatherAddress{handleDesc->address + cmdBuf.offset};
            LOGD("Submit gather, CPU address: 0x{:X}, words: 0x{:X}", gatherAddress, cmdBuf.words);

            span gather(reinterpret_cast<u32 *>(gatherAddress), cmdBuf.words);
#ifdef STRATO_VIDEO_TRACE
            // Explicit diagnostic builds only: never parse/log gathers in normal builds.
            // Raw data remains on the device; only register writes and metadata reach the log.
            if (videoTraceSubmits < 32 && videoTraceMethods < 2048) {
                LOGI("VideoTrace channel={} submit={} gather_words={} relocations={}",
                     static_cast<u32>(channelType), videoTraceSubmits, cmdBuf.words, relocs.size());
                try {
                    soc::host1x::DecodeCommands(gather.first(std::min<size_t>(gather.size(), 8192)), videoTraceClass, [&](u32 cls, u32 method, u32 argument) {
                        if (++videoTraceMethods > 2048)
                            return;
                        LOGI("VideoTrace class=0x{:X} method=0x{:X} argument=0x{:X}", cls, method, argument);
                        if (cls != 0xf0 && cls != 0x5d)
                            return;
                        auto &stored{videoTraceStoredMethods[cls == 0xf0 ? 0 : 1]};
                        if (method == 0x10)
                            stored = argument;
                        else if (method == 0x11)
                            LOGI("VideoTrace {} register_raw=0x{:X} value=0x{:X}", cls == 0xf0 ? "NVDEC" : "VIC", stored, argument);
                    });
                } catch (const std::runtime_error &error) {
                    LOGW("VideoTrace stopped at unsupported or malformed gather: {}", error.what());
                }
            }
#endif
            gathers.emplace_back(gather.begin(), gather.end());
        }

#ifdef STRATO_VIDEO_TRACE
        ++videoTraceSubmits;
#endif

        for (size_t i{}; i < syncpointIncrs.size(); i++) {
            const auto &incr{syncpointIncrs[i]};

            u32 max{core.syncpointManager.IncrementSyncpointMaxExt(incr.syncpointId, incr.numIncrs)};

            if (i < fenceThresholds.size())
                fenceThresholds[i] = max;
        }

        state.soc->host1x.channels.at(static_cast<size_t>(channelType)).Push(std::move(gathers), std::move(pins));
        return PosixResult::Success;
    }

    PosixResult Host1xChannel::GetSyncpoint(In<u32> channelSyncpointIdx, Out<u32> syncpointId) {
        LOGD("channelSyncpointIdx: {}", channelSyncpointIdx);

        if (channelSyncpointIdx > 0)
            throw exception("Multiple channel syncpoints are unimplemented!");

        u32 id{core::SyncpointManager::ChannelSyncpoints[static_cast<u32>(channelType)]};
        if (!id)
            throw exception("Requested syncpoint for a channel with none specified!");

        LOGD("syncpointId: {}", id);
        syncpointId = id;
        return PosixResult::Success;
    }

    PosixResult Host1xChannel::GetWaitBase(In<core::ChannelType> pChannelType, Out<u32> waitBase) {
        LOGD("channelType: {}", static_cast<u32>(pChannelType));
        waitBase = 0;
        return PosixResult::Success;
    }

    PosixResult Host1xChannel::SetSubmitTimeout(In<u32> timeout) {
        LOGD("timeout: {}", timeout);
        return PosixResult::Success;
    }

    PosixResult Host1xChannel::MapBuffer(u8 compressed, span<BufferHandle> handles) {
        LOGD("compressed: {}", compressed);

        for (auto &bufferHandle : handles) {
            bufferHandle.address = core.nvMap.PinHandle(bufferHandle.handle);
            LOGD("handle: {}, address: 0x{:X}", bufferHandle.handle, bufferHandle.address);
        }

        return PosixResult::Success;
    }

    PosixResult Host1xChannel::UnmapBuffer(u8 compressed, span<BufferHandle> handles) {
        LOGD("compressed: {}", compressed);

        for (auto &bufferHandle : handles) {
            core.nvMap.UnpinHandle(bufferHandle.handle);
            LOGD("handle: {}", bufferHandle.handle);
        }

        return PosixResult::Success;
    }

// @fmt:off
#include <services/nvdrv/devices/deserialisation/macro_def.inc>
    static constexpr u32 Host1xChannelMagic{0x00};
    static constexpr u32 GpuChannelMagic{0x48}; //!< Used for SetNvmapFd which is needed in both GPU and host1x channels

    VARIABLE_IOCTL_HANDLER_FUNC(Host1xChannel,
        IOCTL_CASE_ARGS(IN,    SIZE(0x4), MAGIC(GpuChannelMagic),    FUNC(0x1),
                        SetNvmapFd,       ARGS(In<FileDescriptor>))
        IOCTL_CASE_ARGS(INOUT, SIZE(0x8), MAGIC(Host1xChannelMagic), FUNC(0x2),
                        GetSyncpoint,     ARGS(In<u32>, Out<u32>))
        IOCTL_CASE_ARGS(INOUT, SIZE(0x8), MAGIC(Host1xChannelMagic), FUNC(0x3),
                        GetWaitBase,      ARGS(In<core::ChannelType>, Out<u32>))
        IOCTL_CASE_ARGS(IN,    SIZE(0x4), MAGIC(Host1xChannelMagic), FUNC(0x7),
                        SetSubmitTimeout, ARGS(In<u32>))
    ,
        VARIABLE_IOCTL_CASE_ARGS(INOUT, MAGIC(Host1xChannelMagic), FUNC(0x1),
                                 Submit,      ARGS(Save<u32, 0>, Save<u32, 1>, Save<u32, 2>, Save<u32, 3>,
                                                   SlotSizeSpan<SubmitCmdBuf, 0>,
                                                   SlotSizeSpan<SubmitReloc, 1>, SlotSizeSpan<u32, 1>,
                                                   SlotSizeSpan<SubmitSyncpointIncr, 2>, SlotSizeSpan<u32, 3>))
        VARIABLE_IOCTL_CASE_ARGS(INOUT, MAGIC(Host1xChannelMagic), FUNC(0x9),
                                 MapBuffer,   ARGS(Save<u32, 0>, Pad<u32>, In<u8>, Pad<u8, 3>, SlotSizeSpan<BufferHandle, 0>))
        VARIABLE_IOCTL_CASE_ARGS(INOUT, MAGIC(Host1xChannelMagic), FUNC(0xA),
                                 UnmapBuffer, ARGS(Save<u32, 0>, Pad<u32>, In<u8>, Pad<u8, 3>, SlotSizeSpan<BufferHandle, 0>))
    )
#include <services/nvdrv/devices/deserialisation/macro_undef.inc>
// @fmt:on
}

