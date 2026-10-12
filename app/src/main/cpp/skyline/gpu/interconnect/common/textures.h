// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <map>
#include <tsl/robin_map.h>
#include <shader_compiler/shader_info.h>
#include <gpu/texture/texture.h>
#include <gpu/texture/format.h>
#include "common.h"
#include "tic.h"

namespace skyline::gpu::interconnect {
    // Maps the guest's storage-image declaration to a host view format.
    // A typeless declaration uses the underlying TIC format instead.
    inline texture::Format StorageImageViewFormat(Shader::ImageFormat imageFormat) {
        switch (imageFormat) {
            case Shader::ImageFormat::Typeless: return {};
            case Shader::ImageFormat::R8_UINT: return format::R8Uint;
            case Shader::ImageFormat::R8_SINT: return format::R8Sint;
            case Shader::ImageFormat::R16_UINT: return format::R16Uint;
            case Shader::ImageFormat::R16_SINT: return format::R16Sint;
            case Shader::ImageFormat::R32_UINT: return format::R32Uint;
            case Shader::ImageFormat::R32G32_UINT: return format::R32G32Uint;
            case Shader::ImageFormat::R32G32B32A32_UINT: return format::R32G32B32A32Uint;
        }
        return {};
    }

    class TexturePoolState : dirty::CachedManualDirty {
      public:
        struct EngineRegisters {
            const engine_common::TexHeaderPool &texHeaderPool;

            void DirtyBind(DirtyManager &manager, dirty::Handle handle) const;
        };

      private:
        dirty::BoundSubresource<EngineRegisters> engine;

      public:
        span<TextureImageControl> textureHeaders;

        TexturePoolState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine);

        void Flush(InterconnectContext &ctx);

        void PurgeCaches();
    };

    class Textures {
      private:
        std::shared_ptr<TextureView> nullTextureView{};
        // Storage fallback images are typed and dimensional: an invalid TIC must
        // not silently turn into an incompatible RGBA sampled-only image.
        std::map<std::pair<Shader::TextureType, Shader::ImageFormat>, std::shared_ptr<TextureView>> nullStorageTextureViews;
        dirty::ManualDirtyState<TexturePoolState> texturePool;

        tsl::robin_map<TextureImageControl, std::shared_ptr<TextureView>, util::ObjectHash<TextureImageControl>> textureHeaderStore;

        struct CacheEntry {
            TextureImageControl tic;
            TextureView *view;
            u64 sequenceNumber;
        };
        std::vector<CacheEntry> textureHeaderCache;

      public:
        Textures(DirtyManager &manager, const TexturePoolState::EngineRegisters &engine);

        void MarkAllDirty();

        TextureView *GetTexture(InterconnectContext &ctx, u32 index, Shader::TextureType shaderType);

        TextureView *GetNullStorageTexture(InterconnectContext &ctx, Shader::TextureType type, Shader::ImageFormat format);

        Shader::TextureType GetTextureType(InterconnectContext &ctx, u32 index);
    };
}
