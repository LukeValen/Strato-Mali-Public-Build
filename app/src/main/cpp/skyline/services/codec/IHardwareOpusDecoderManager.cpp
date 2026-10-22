// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <services/serviceman.h>

#include "IHardwareOpusDecoderManager.h"
#include "IHardwareOpusDecoder.h"

namespace skyline::service::codec {
    // Invalid Opus configuration (audio service). Do not return a successful zero-sized
    // buffer: the Nintendo SDK would then try to create zero-sized transfer memory.
    static constexpr Result InvalidOpusParameters{153, 6};

    static bool ValidMultiStreamParameters(const MultiStreamParametersEx &params) {
        const bool validRate{params.sampleRate == 8000 || params.sampleRate == 12000 ||
                             params.sampleRate == 16000 || params.sampleRate == 24000 ||
                             params.sampleRate == 48000};
        return validRate && params.channelCount > 0 && params.channelCount <= 8 &&
               params.streamCount > 0 && params.streamCount <= params.channelCount &&
               params.stereoStreamCount >= 0 && params.stereoStreamCount <= params.streamCount &&
               params.streamCount + params.stereoStreamCount <= params.channelCount;
    }

    static u32 CalculateMultiStreamBufferSize(const MultiStreamParametersEx &params) {
        const auto decoderSize{opus_multistream_decoder_get_size(params.streamCount, params.stereoStreamCount)};
        if (decoderSize <= 0)
            return 0;
        return static_cast<u32>(decoderSize) + util::AlignUp(1500U * static_cast<u32>(params.streamCount), 64U) +
               CalculateOutBufferSize(params.sampleRate, params.channelCount,
                                      params.useLargerFrameSize ? MaxFrameSizeEx : MaxFrameSizeNormal);
    }

    static bool ReadMultiStreamParameters(ipc::IpcRequest &request, MultiStreamParametersEx &params) {
        if (request.inputBuf.empty() || request.inputBuf[0].size_bytes() < sizeof(params))
            return false;
        std::memcpy(&params, request.inputBuf[0].data(), sizeof(params));
        return ValidMultiStreamParameters(params);
    }

    static u32 CalculateBufferSize(i32 sampleRate, i32 channelCount, i32 useLargerFrameSize = 0) {
        u32 requiredSize{static_cast<u32>(opus_decoder_get_size(channelCount))};
        requiredSize += MaxInputBufferSize + CalculateOutBufferSize(sampleRate, channelCount, useLargerFrameSize ? MaxFrameSizeEx : MaxFrameSizeNormal);
        return requiredSize;
    }

    Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoder(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 sampleRate{request.Pop<i32>()};
        i32 channelCount{request.Pop<i32>()};
        u32 workBufferSize{request.Pop<u32>()};
        KHandle workBuffer{request.copyHandles.at(0)};

        LOGD("Creating Opus decoder: Sample rate: {}, Channel count: {}, Work buffer handle: 0x{:X} (Size: 0x{:X})", sampleRate, channelCount, workBuffer, workBufferSize);

        manager.RegisterService(std::make_shared<IHardwareOpusDecoder>(state, manager, sampleRate, channelCount, workBufferSize, workBuffer), session, response);
        return {};
    }

    Result IHardwareOpusDecoderManager::GetWorkBufferSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 sampleRate{request.Pop<i32>()};
        i32 channelCount{request.Pop<i32>()};

        response.Push<u32>(CalculateBufferSize(sampleRate, channelCount));
        return {};
    }

    Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoderEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 sampleRate{request.Pop<i32>()};
        i32 channelCount{request.Pop<i32>()};
        i32 useLargerFrameSize{request.Pop<i32>()};
        request.Pop<i32>(); // Just padding
        u32 workBufferSize{request.Pop<u32>()};
        KHandle workBuffer{request.copyHandles.at(0)};

        LOGD("Creating Opus decoder: Sample rate: {}, Channel count: {}, Work buffer handle: 0x{:X} (Size: 0x{:X})", sampleRate, channelCount, workBuffer, workBufferSize);

        manager.RegisterService(std::make_shared<IHardwareOpusDecoder>(state, manager, sampleRate, channelCount, workBufferSize, workBuffer, useLargerFrameSize), session, response);
        return {};
    }

    Result IHardwareOpusDecoderManager::GetWorkBufferSizeEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 sampleRate{request.Pop<i32>()};
        i32 channelCount{request.Pop<i32>()};
        i32 useLargerFrameSize{request.Pop<i32>()};
        request.Pop<i32>(); // Just padding

        response.Push<u32>(CalculateBufferSize(sampleRate, channelCount, useLargerFrameSize));
        return {};
    }

    Result IHardwareOpusDecoderManager::GetWorkBufferSizeForMultiStreamExEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        MultiStreamParametersEx params{};
        if (!ReadMultiStreamParameters(request, params))
            return InvalidOpusParameters;

        const u32 size{CalculateMultiStreamBufferSize(params)};
        if (!size)
            return InvalidOpusParameters;
        response.Push<u32>(size);
        return {};
    }

    Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoderForMultiStreamEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        MultiStreamParametersEx params{};
        if (!ReadMultiStreamParameters(request, params) || request.copyHandles.empty() || request.cmdArgSz < sizeof(u32))
            return InvalidOpusParameters;

        const u32 workBufferSize{request.Pop<u32>()};
        if (workBufferSize < CalculateMultiStreamBufferSize(params))
            return InvalidOpusParameters;

        manager.RegisterService(std::make_shared<IHardwareOpusDecoder>(state, manager, params, workBufferSize, request.copyHandles[0]), session, response);
        return {};
    }
}
