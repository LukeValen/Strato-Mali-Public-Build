// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <fstream>
#include <ctime>
#include <system_error>
#include <range/v3/algorithm.hpp>
#include <boost/functional/hash.hpp>
#include <gpu.h>
#include <common/settings.h>
#include <shader_compiler/common/settings.h>
#include <shader_compiler/common/log.h>
#include <shader_compiler/frontend/maxwell/translate_program.h>
#include <shader_compiler/backend/spirv/emit_spirv.h>
#include <vulkan/vulkan_raii.hpp>
#include "shader_manager.h"
#include <gpu/resolution_shader.h>
#include <gpu/nier_bloom_ab1.h>

static constexpr bool DumpShaders{false};

namespace Shader::Log {
    void Debug(const std::string &message) {
        skyline::AsyncLogger::LogAsync(skyline::AsyncLogger::LogLevel::Debug, std::move(const_cast<std::string &>(message)));
    }

    void Warn(const std::string &message) {
        skyline::AsyncLogger::LogAsync(skyline::AsyncLogger::LogLevel::Warning, std::move(const_cast<std::string &>(message)));
    }

    void Error(const std::string &message) {
        skyline::AsyncLogger::LogAsync(skyline::AsyncLogger::LogLevel::Error, std::move(const_cast<std::string &>(message)));
    }
}

namespace skyline::gpu {
    void ShaderManager::CaptureNierBinary(std::string_view name, span<u8> binary) {
        // Shader dumping is intentionally best-effort. Never break gameplay
        // because external storage is full, unavailable, or permission-restricted.
        if (!gpu.nierDiagnosticsEnabled || binary.empty())
            return;
        constexpr u64 MaxBytes = 96ULL * 1024 * 1024;
        constexpr u64 MaxFileBytes = 1024ULL * 1024;
        constexpr u32 MaxFiles = 1600;
        std::scoped_lock lock{dumpMutex};
        if (!nierCaptureAvailable) return;
        if (binary.size() > MaxFileBytes ||
            nierCaptureBytes + binary.size() > MaxBytes ||
            nierCaptureFileCount >= MaxFiles) {
            if (!nierCaptureLimitReported) {
                LOGW("NIER_CAPTURE quota reached: files={} bytes={} (max 1600 files/96 MiB, per-file 1 MiB)",
                     nierCaptureFileCount, nierCaptureBytes);
                nierCaptureLimitReported = true;
            }
            return;
        }
        const auto path{nierCapturePath / std::string{name}};
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) return;
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        if (!out) {
            nierCaptureAvailable = false;
            LOGW("NIER_CAPTURE output unavailable at {}", path.parent_path().string());
            return;
        }
        out.write(reinterpret_cast<const char *>(binary.data()),
                  static_cast<std::streamsize>(binary.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(path, ec);
            nierCaptureAvailable = false;
            LOGW("NIER_CAPTURE write failed; capture stopped but game continues");
            return;
        }
        nierCaptureBytes += binary.size();
        ++nierCaptureFileCount;
    }

    void ShaderManager::LoadShaderReplacements(std::string_view replacementDir) {
        std::filesystem::path replacementDirPath{replacementDir};
        if (std::filesystem::exists(replacementDirPath)) {
            for (const auto &entry : std::filesystem::directory_iterator{replacementDirPath}) {
                if (entry.is_regular_file()) {
                    // Parse hash from filename
                    auto path{entry.path()};
                    auto &replacementMap{path.extension().string() == ".spv" ? hostShaderReplacements : guestShaderReplacements};
                    u64 hash{std::stoull(path.stem().string(), nullptr, 16)};
                    auto it{replacementMap.insert({hash, {}})};

                    // Read file into map entry
                    std::ifstream file{entry.path(), std::ios::binary | std::ios::ate};
                    it.first->second.resize(static_cast<size_t>(file.tellg()));
                    file.seekg(0, std::ios::beg);
                    file.read(reinterpret_cast<char *>(it.first->second.data()), static_cast<std::streamsize>(it.first->second.size()));
                }
            }
        }
    }

    span<u8> ShaderManager::ProcessShaderBinary(bool spv, u64 hash, span<u8> binary) {
        auto &replacementMap{spv ? hostShaderReplacements : guestShaderReplacements};
        auto it{replacementMap.find(hash)};
        if (it != replacementMap.end()) {
            LOGI("Replacing shader with hash: 0x{:X}", hash);
            return it->second;
        }

        if (DumpShaders) {
            std::scoped_lock lock{dumpMutex};

            auto shaderPath{dumpPath / fmt::format("{:016X}{}", hash, spv ? ".spv" : "")};
            if (!std::filesystem::exists(shaderPath)) {
                std::ofstream file{shaderPath, std::ios::binary};
                file.write(reinterpret_cast<const char *>(binary.data()), static_cast<std::streamsize>(binary.size()));
            }
        }

        return binary;
    }

    ShaderManager::ShaderManager(const DeviceState &state, GPU &gpu, std::string_view replacementDir, std::string_view dumpDir) : gpu{gpu}, dumpPath{dumpDir} {
        // The experiment is gated by Kirby's title ID in GPU::Initialise().
        // Reset explicitly when a different game is launched in the same app.
        Shader::Settings::kirby_packed_wscale_test.store(gpu.kirbyDiagnosticsEnabled, std::memory_order_relaxed);
        // Never change the geometry compiler for games other than NieR on Mali.
        // Store false on each new game, including after returning from NieR.
        Shader::Settings::nier_mali_geometry_invocation_fix.store(gpu.nierDiagnosticsEnabled, std::memory_order_relaxed);
        if (gpu.nierDiagnosticsEnabled)
            LOGI("NIER_DIAG geometry InvocationInfo compatibility workaround enabled (Mali only)");

        if (gpu.kirbyDiagnosticsEnabled)
            LOGI("KIRBY_EXPERIMENT packed WScale 0x00FF0000 enabled (shader-only test; no driver changes)");
        LoadShaderReplacements(replacementDir);

        if (gpu.nierDiagnosticsEnabled) {
            // Use a new timestamped subfolder on every launch. The base shader
            // dump directory remains unchanged for ordinary Skyline behavior.
            nierCapturePath = dumpPath / fmt::format("nier_mali_capture_{}", std::time(nullptr));
            std::error_code ec;
            std::filesystem::create_directories(nierCapturePath, ec);
            nierCaptureAvailable = !ec;
            if (nierCaptureAvailable) {
                LOGI("NIER_CAPTURE enabled at {} (96 MiB/1600 files; only NieR on Mali)",
                     nierCapturePath.string());
                std::ofstream manifest{nierCapturePath / "README.txt"};
                manifest << "Strato Mali NieR/Mali graphics capture v1\n"
                         << "Guest Maxwell shader binaries and generated Vulkan SPIR-V.\n"
                         << "Corresponding draw/compute/render-target metadata is in emulation.log.\n"
                         << "These binaries contain game shader code; share only for debugging.\n";
            } else {
                LOGW("NIER_CAPTURE disabled: unable to create folder {} error={}",
                     nierCapturePath.string(), ec.message());
            }
        }

        if constexpr (DumpShaders) {
            if (!std::filesystem::exists(dumpPath))
                std::filesystem::create_directories(dumpPath);
        }

        static_assert(sizeof(resolution::ShaderScale) == sizeof(Shader::Backend::SPIRV::RescalingLayout));
        static_assert(offsetof(resolution::ShaderScale, downFactor) ==
            Shader::Backend::SPIRV::RESCALING_LAYOUT_DOWN_FACTOR_OFFSET);
        auto &traits{gpu.traits};
        hostTranslateInfo = Shader::HostTranslateInfo{
            .support_float16 = traits.supportsFloat16,
            .support_int64 = traits.supportsInt64,
            .needs_demote_reorder = false,
            .support_snorm_render_buffer = true,
            .support_viewport_index_layer = gpu.traits.supportsShaderViewportIndexLayer,
            .min_ssbo_alignment = traits.minimumStorageBufferAlignment,
            .support_geometry_shader_passthrough = false
        };

        constexpr u32 TegraX1WarpSize{32}; //!< The amount of threads in a warp on the Tegra X1
        profile = Shader::Profile{
            .supported_spirv = traits.supportsSpirv14 ? 0x00010400U : 0x00010000U,
            .unified_descriptor_binding = true,
            .support_descriptor_aliasing = true,
            .support_int8 = traits.supportsInt8,
            .support_int16 = traits.supportsInt16,
            .support_int64 = traits.supportsInt64,
            .support_vertex_instance_id = false,
            .support_float_controls = traits.supportsFloatControls,
            .support_separate_denorm_behavior = traits.floatControls.denormBehaviorIndependence == vk::ShaderFloatControlsIndependence::eAll,
            .support_separate_rounding_mode = traits.floatControls.roundingModeIndependence == vk::ShaderFloatControlsIndependence::eAll,
            .support_fp16_denorm_preserve = static_cast<bool>(traits.floatControls.shaderDenormPreserveFloat16),
            .support_fp32_denorm_preserve = static_cast<bool>(traits.floatControls.shaderDenormPreserveFloat32),
            .support_fp16_denorm_flush = static_cast<bool>(traits.floatControls.shaderDenormFlushToZeroFloat16),
            .support_fp32_denorm_flush = static_cast<bool>(traits.floatControls.shaderDenormFlushToZeroFloat32),
            .support_fp16_signed_zero_nan_preserve = static_cast<bool>(traits.floatControls.shaderSignedZeroInfNanPreserveFloat16),
            .support_fp32_signed_zero_nan_preserve = static_cast<bool>(traits.floatControls.shaderSignedZeroInfNanPreserveFloat32),
            .support_fp64_signed_zero_nan_preserve = static_cast<bool>(traits.floatControls.shaderSignedZeroInfNanPreserveFloat64),
            .support_explicit_workgroup_layout = false,
            .support_vote = traits.supportsSubgroupVote,
            .support_viewport_index_layer_non_geometry = traits.supportsShaderViewportIndexLayer,
            .support_viewport_mask = false,
            .support_typeless_image_loads = traits.supportsImageReadWithoutFormat,
            .support_demote_to_helper_invocation = traits.supportsShaderDemoteToHelper,
            .support_int64_atomics = traits.supportsAtomicInt64,
            .support_derivative_control = true,
            .support_geometry_shader_passthrough = false,
            .support_native_ndc = false,
            .warp_size_potentially_larger_than_guest = TegraX1WarpSize < traits.subgroupSize,
            .lower_left_origin_mode = false,
            .need_declared_frag_colors = false,
            .has_broken_spirv_position_input = traits.quirks.brokenSpirvPositionInput,
            .has_broken_spirv_subgroup_mask_vector_extract_dynamic = traits.quirks.brokenSubgroupMaskExtractDynamic,
            .has_broken_spirv_subgroup_shuffle = traits.quirks.brokenSubgroupShuffle,
            .max_subgroup_size = traits.subgroupSize,
            .has_broken_spirv_vector_access_chain = traits.quirks.brokenSpirvVectorAccessChain,
            .disable_subgroup_shuffle = *state.settings->disableSubgroupShuffle,
        };

        Shader::Settings::values = {
            #ifdef NDEBUG
            .renderer_debug = false,
            .disable_shader_loop_safety_checks = false,
            #else
            .renderer_debug = true,
            .disable_shader_loop_safety_checks = true,
            #endif
            .resolution_info = {
                .up_scale = gpu.resolutionScale.Numerator(),
                .down_shift = gpu.resolutionScale.DenominatorShift(),
                .up_factor = gpu.resolutionScale.Factor(),
                .down_factor = 1.0f / gpu.resolutionScale.Factor(),
                .active = !gpu.resolutionScale.IsNative(),
                .downscale = gpu.resolutionScale.Factor() < 1.0f,
            },
        };
    }

    /**
     * @brief A shader environment for all graphics pipeline stages
     */
    class GraphicsEnvironment : public Shader::Environment {
      private:
        span<u8> binary;
        u32 baseOffset;
        u32 textureBufferIndex;
        bool viewportTransformEnabled;
        ShaderManager::ConstantBufferRead constantBufferRead;
        ShaderManager::GetTextureType getTextureType;

      public:
        GraphicsEnvironment(const std::array<u32, 8> &postVtgShaderAttributeSkipMask,
                            Shader::Stage pStage,
                            span<u8> pBinary, u32 baseOffset,
                            u32 textureBufferIndex,
                            bool viewportTransformEnabled,
                            ShaderManager::ConstantBufferRead constantBufferRead, ShaderManager::GetTextureType getTextureType)
            : binary{pBinary}, baseOffset{baseOffset},
              textureBufferIndex{textureBufferIndex},
              viewportTransformEnabled{viewportTransformEnabled},
              constantBufferRead{std::move(constantBufferRead)}, getTextureType{std::move(getTextureType)} {
            gp_passthrough_mask = postVtgShaderAttributeSkipMask;
            stage = pStage;
            sph = *reinterpret_cast<Shader::ProgramHeader *>(binary.data());
            start_address = baseOffset;
            is_propietary_driver = textureBufferIndex == 2;
        }

        [[nodiscard]] u64 ReadInstruction(u32 address) final {
            address -= baseOffset;
            if (binary.size() < (address + sizeof(u64)))
                throw exception("Out of bounds instruction read: 0x{:X}", address);
            return *reinterpret_cast<u64 *>(binary.data() + address);
        }

        [[nodiscard]] u32 ReadCbufValue(u32 index, u32 offset) final {
            return constantBufferRead(index, offset);
        }

        [[nodiscard]] Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) final {
            throw exception("ReadTexturePixelFormat not implemented");
        }

        [[nodiscard]] Shader::TextureType ReadTextureType(u32 handle) final {
            return getTextureType(handle);
        }

        [[nodiscard]] u32 ReadViewportTransformState() final {
            return viewportTransformEnabled ? 1 : 0; // Only relevant for graphics shaders
        }

        [[nodiscard]] u32 TextureBoundBuffer() const final {
            return textureBufferIndex;
        }

        [[nodiscard]] u32 LocalMemorySize() const final {
            return static_cast<u32>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
        }

        [[nodiscard]] u32 SharedMemorySize() const final {
            return 0; // Only relevant for compute shaders
        }

        [[nodiscard]] std::array<u32, 3> WorkgroupSize() const final {
            return {0, 0, 0}; // Only relevant for compute shaders
        }

        [[nodiscard]] bool HasHLEMacroState() const final {
            return stage == Shader::Stage::VertexB || stage == Shader::Stage::VertexA;
        }

        [[nodiscard]] std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) final {
            if (bank != 0 || !is_propietary_driver)
                return std::nullopt;

            // Replace constant buffer offsets containing draw parameters with the appropriate shader attribute, required as e.g. in the case of indirect draws the constant buffer contents won't be entirely correct for these specific parameters
            switch (offset) {
                case 0x640:
                    return Shader::ReplaceConstant::BaseVertex;
                case 0x644:
                    return Shader::ReplaceConstant::BaseInstance;
                case 0x648:
                    return Shader::ReplaceConstant::DrawID;
                default:
                    return std::nullopt;
            }
        }

        void Dump(u64 hash) final {}
    };

    /**
     * @brief A shader environment for all compute pipeline stages
     */
    class ComputeEnvironment : public Shader::Environment {
      private:
        span<u8> binary;
        u32 baseOffset;
        u32 textureBufferIndex;
        u32 localMemorySize;
        u32 sharedMemorySize;
        std::array<u32, 3> workgroupDimensions;
        ShaderManager::ConstantBufferRead constantBufferRead;
        ShaderManager::GetTextureType getTextureType;

      public:
        ComputeEnvironment(span<u8> pBinary,
                           u32 baseOffset,
                           u32 textureBufferIndex,
                           u32 localMemorySize, u32 sharedMemorySize,
                           std::array<u32, 3> workgroupDimensions,
                           ShaderManager::ConstantBufferRead constantBufferRead, ShaderManager::GetTextureType getTextureType)
            : binary{pBinary},
              baseOffset{baseOffset},
              textureBufferIndex{textureBufferIndex},
              localMemorySize{localMemorySize},
              sharedMemorySize{sharedMemorySize},
              workgroupDimensions{workgroupDimensions},
              constantBufferRead{std::move(constantBufferRead)},
              getTextureType{std::move(getTextureType)} {
            stage = Shader::Stage::Compute;
            start_address = baseOffset;
            is_propietary_driver = textureBufferIndex == 2;
        }

        [[nodiscard]] u64 ReadInstruction(u32 address) final {
            address -= baseOffset;
            if (binary.size() < (address + sizeof(u64)))
                throw exception("Out of bounds instruction read: 0x{:X}", address);
            return *reinterpret_cast<u64 *>(binary.data() + address);
        }

        [[nodiscard]] u32 ReadCbufValue(u32 index, u32 offset) final {
            return constantBufferRead(index, offset);
        }

        [[nodiscard]] Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) final {
            throw exception("ReadTexturePixelFormat not implemented");
        }

        [[nodiscard]] Shader::TextureType ReadTextureType(u32 handle) final {
            return getTextureType(handle);
        }

        [[nodiscard]] u32 ReadViewportTransformState() final {
            return 0; // Only relevant for graphics shaders
        }

        [[nodiscard]] u32 TextureBoundBuffer() const final {
            return textureBufferIndex;
        }

        [[nodiscard]] u32 LocalMemorySize() const final {
            return localMemorySize;
        }

        [[nodiscard]] u32 SharedMemorySize() const final {
            return sharedMemorySize;
        }

        [[nodiscard]] std::array<u32, 3> WorkgroupSize() const final {
            return workgroupDimensions;
        }

        [[nodiscard]] bool HasHLEMacroState() const final {
            return false;
        }

        [[nodiscard]] std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) final {
            return std::nullopt;
        }

        void Dump(u64 hash) final {}
    };


    /**
     * @brief A shader environment for VertexB during combination as it only requires the shader header and no higher level context
     */
    class VertexBEnvironment : public Shader::Environment {
      public:
        explicit VertexBEnvironment(span<u8> binary) {
            sph = *reinterpret_cast<Shader::ProgramHeader *>(binary.data());
            stage = Shader::Stage::VertexB;
        }

        [[nodiscard]] u64 ReadInstruction(u32 address) final {
            throw exception("Not implemented");
        }

        [[nodiscard]] u32 ReadCbufValue(u32 index, u32 offset) final {
            throw exception("Not implemented");
        }

        [[nodiscard]] Shader::TextureType ReadTextureType(u32 handle) final {
            throw exception("Not implemented");
        }

        [[nodiscard]] Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) final {
            throw exception("Not implemented");
        }

        [[nodiscard]] u32 ReadViewportTransformState() final {
            throw exception("Not implemented");
        }

        [[nodiscard]] u32 TextureBoundBuffer() const final {
            throw exception("Not implemented");
        }

        [[nodiscard]] u32 LocalMemorySize() const final {
            return static_cast<u32>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
        }

        [[nodiscard]] u32 SharedMemorySize() const final {
            return 0; // Only relevant for compute shaders
        }

        [[nodiscard]] std::array<u32, 3> WorkgroupSize() const final {
            return {0, 0, 0}; // Only relevant for compute shaders
        }

        [[nodiscard]] bool HasHLEMacroState() const final {
            return false;
        }

        [[nodiscard]] std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) final {
            return std::nullopt;
        }

        void Dump(u64 hash) final {}
    };

    Shader::IR::Program ShaderManager::ParseGraphicsShader(const std::array<u32, 8> &postVtgShaderAttributeSkipMask,
                                                           Shader::Stage stage,
                                                           u64 hash, span<u8> binary, u32 baseOffset,
                                                           u32 textureConstantBufferIndex,
                                                           bool viewportTransformEnabled,
                                                           const ConstantBufferRead &constantBufferRead, const GetTextureType &getTextureType, bool enableResolutionScaling) {
        binary = ProcessShaderBinary(false, hash, binary);
        if (gpu.nierDiagnosticsEnabled && hash)
            CaptureNierBinary(fmt::format("guest_{:016X}.bin", hash), binary);

        std::scoped_lock lock{poolMutex};

        struct RestoreScaling {
            bool active{Shader::Settings::values.resolution_info.active};
            ~RestoreScaling() { Shader::Settings::values.resolution_info.active = active; }
        } restoreScaling;
        Shader::Settings::values.resolution_info.active = restoreScaling.active && enableResolutionScaling;
        GraphicsEnvironment environment{postVtgShaderAttributeSkipMask, stage, binary, baseOffset, textureConstantBufferIndex, viewportTransformEnabled, constantBufferRead, getTextureType};
        Shader::Maxwell::Flow::CFG cfg{environment, flowBlockPool, Shader::Maxwell::Location{static_cast<u32>(baseOffset + sizeof(Shader::ProgramHeader))}};
        return  Shader::Maxwell::TranslateProgram(instructionPool, blockPool, environment, cfg, hostTranslateInfo);
    }

    Shader::IR::Program ShaderManager::CombineVertexShaders(Shader::IR::Program &vertexA, Shader::IR::Program &vertexB, span<u8> vertexBBinary) {
        std::scoped_lock lock{poolMutex};

        VertexBEnvironment env{vertexBBinary};
        return Shader::Maxwell::MergeDualVertexPrograms(vertexA, vertexB, env);
    }

    Shader::IR::Program ShaderManager::GenerateGeometryPassthroughShader(Shader::IR::Program &layerSource, Shader::OutputTopology topology) {
        std::scoped_lock lock{poolMutex};

        return Shader::Maxwell::GenerateGeometryPassthrough(instructionPool, blockPool, hostTranslateInfo, layerSource, topology);
    }

    Shader::IR::Program ShaderManager::ParseComputeShader(u64 hash, span<u8> binary, u32 baseOffset,
                                                          u32 textureConstantBufferIndex,
                                                          u32 localMemorySize, u32 sharedMemorySize,
                                                          std::array<u32, 3> workgroupDimensions,
                                                          const ConstantBufferRead &constantBufferRead, const GetTextureType &getTextureType) {
        binary = ProcessShaderBinary(false, hash, binary);
        if (gpu.nierDiagnosticsEnabled && hash)
            CaptureNierBinary(fmt::format("guest_{:016X}.bin", hash), binary);

        std::scoped_lock lock{poolMutex};

        ComputeEnvironment environment{binary, baseOffset, textureConstantBufferIndex, localMemorySize, sharedMemorySize, workgroupDimensions, constantBufferRead, getTextureType};
        Shader::Maxwell::Flow::CFG cfg{environment, flowBlockPool, Shader::Maxwell::Location{static_cast<u32>(baseOffset)}};
        // Compute still uses native images; its pipeline layout has no scaling
        // constants. Restore the global compiler setting even if translation throws.
        struct RestoreScaling {
            bool active{Shader::Settings::values.resolution_info.active};
            ~RestoreScaling() { Shader::Settings::values.resolution_info.active = active; }
        } restoreScaling;
        Shader::Settings::values.resolution_info.active = false;
        return Shader::Maxwell::TranslateProgram(instructionPool, blockPool, environment, cfg, hostTranslateInfo);
    }

    vk::ShaderModule ShaderManager::CompileShader(const Shader::RuntimeInfo &runtimeInfo, Shader::IR::Program &program, Shader::Backend::Bindings &bindings, u64 hash) {
        std::scoped_lock lock{poolMutex};

        if (program.info.loads.Legacy() || program.info.stores.Legacy())
            Shader::Maxwell::ConvertLegacyToGeneric(program, runtimeInfo);

        auto spirvEmitted{Shader::Backend::SPIRV::EmitSPIRV(profile, runtimeInfo, program, bindings)};
        auto spirv{program.info.uses_rescaling_uniform ? span<u32>{spirvEmitted} :
            ProcessShaderBinary(true, hash, span<u32>{spirvEmitted}.cast<u8>()).cast<u32>()};

        // Experimental NieR-only A/B for colored bloom orbs on ARM Mali.
        // The guest's primary HDR color attachment (location 0) is untouched.
        // A strict SPIR-V guard refuses all unexpected layouts, rather than
        // attempting to rewrite arbitrary shaders.
        std::vector<u32> nierBloomPatched;
        if (gpu.nierDiagnosticsEnabled && hash == 0xF4215778B3A6FB13ULL &&
            program.stage == Shader::Stage::Fragment) {
            nierBloomPatched.assign(spirv.begin(), spirv.end());
            if (nier_bloom::SuppressBrightPass(nierBloomPatched)) {
                spirv = span<u32>{nierBloomPatched};
                LOGW("NIER_BLOOM_AB1 bright-pass suppressed: hash=0x{:016X} RT1=black (primary HDR/RT0 unchanged)", hash);
            } else {
                LOGW("NIER_BLOOM_AB1 skipped: bright-pass SPIR-V structure unexpected, original shader kept hash=0x{:016X}", hash);
            }
        }

        if (gpu.nierDiagnosticsEnabled) {
            const u32 sample = gpu.nierShaderSamples.fetch_add(1, std::memory_order_relaxed);
            const auto stage = static_cast<u32>(program.stage);
            // Generated geometry helpers may have hash=0; use compilation index
            // to prevent overwriting variants with different runtime state.
            const auto name = fmt::format("spv_{:06}_{:016X}_s{}.spv", sample, hash, stage);
            CaptureNierBinary(name, spirv.cast<u8>());
            if (sample < 384 || (sample % 64 == 0 && sample < 32768)) {
                u32 imageWrites{}, imageReads{};
                for (const auto &desc : program.info.image_descriptors) {
                    imageWrites += desc.is_written ? 1U : 0U;
                    imageReads += desc.is_read ? 1U : 0U;
                }
                LOGI("NIER_CAPTURE shader sample={} hash=0x{:016X} stage={} spv={} words={} geometryPass={} vote={} shuffle={} mask={} lane={} fragCoord={} rescale={} derivatives={} imageReads={} imageWrites={} textures={} storageBuffers={} storageGlobals={}",
                     sample, hash, stage, name, spirv.size(), program.is_geometry_passthrough,
                     program.info.uses_subgroup_vote, program.info.uses_subgroup_shuffles,
                     program.info.uses_subgroup_mask, program.info.uses_subgroup_invocation_id,
                     program.info.loads[Shader::IR::Attribute::PositionX] ||
                         program.info.loads[Shader::IR::Attribute::PositionY],
                     program.info.uses_rescaling_uniform, program.info.uses_derivatives,
                     imageReads, imageWrites, program.info.texture_descriptors.size(),
                     program.info.storage_buffers_descriptors.size(),
                     program.info.stores_global_memory);
            }
        }

        vk::ShaderModuleCreateInfo createInfo{
            .pCode = spirv.data(),
            .codeSize = spirv.size_bytes(),
        };

        return (*gpu.vkDevice).createShaderModule(createInfo, nullptr, *gpu.vkDevice.getDispatcher());
    }

    void ShaderManager::ResetPools() {
        std::scoped_lock lock{poolMutex};

        instructionPool.ReleaseContents();
        blockPool.ReleaseContents();
        flowBlockPool.ReleaseContents();
    }
}
