# Experimental BCn GPU decoding

Settings → BCn (after General) → GPU BCn decoding. Disabled by default.
The value is snapshotted when a game's GPU is created; restart the game after changing it.
With the option off, texture uploads retain the existing native-format detection and CPU decoder.
Native BC formats remain native even with the option on.

The separate **BCn compute shader cache** setting defaults on, but only acts when BCn compute
is enabled and the global shader cache is enabled. It stores Vulkan compute pipeline binaries
in a private driver/UUID-specific file, with a versioned name, size limit, checksum and Vulkan
header validation. Invalid or rejected data falls back to ordinary pipeline compilation.
The cache is written after the first creation of each decoder pipeline, outside the texture
upload command buffer. It can reduce pipeline compilation on later game launches; it does not
remove decoding or guarantee a frame-rate gain. Both toggles apply on the next game launch.

This ports the buffer-output Granite decoders from leegao/bcn_layer commit
50993a2d51772567de9c36de4d523652773f0899 into Strato's upload command buffers.
It does not load an interception layer or advertise artificial format support.
BC1 RGBA, BC2, BC3, BC6H (signed/unsigned), and BC7 are included, with sRGB bytes preserved.
BC4/BC5 retain CPU decoding because their R8/RG8 and signed packing paths need separate adaptation.
ASTC/ETC2 transcoding is not included: this version accelerates decompression and still stores
uncompressed host textures, so it does not claim the compressed-memory savings of Winlator's transcoder.
Winlator Mali and Bannerlator use [leegao's Vulkan BCn layer](https://github.com/leegao/bcn_layer)
and GPU encoders (see [Bannerlator's wrapper guide](https://github.com/The412Banner/Bannerlator/blob/main/docs/graphics-wrappers-guide.md)
and [Winlator Mali's release notes](https://github.com/GunaCharanTeja/WinlatorMali/releases/tag/bionic-mali-1.0)). A real
Switch-side transcode needs compatible encoder SPIR-V, compressed host-format selection before
image allocation, mip/layer layout handling, format-feature checks and safe treatment of readback,
render-target/storage-image use and aliasing. Simply changing the host VkFormat or exposing an
ETC2/ASTC selector would produce invalid uploads or views. Keep those modes unavailable until
the encoder and its full texture lifecycle pass GPU/device tests.

One compressed input buffer and the final upload buffer serve all mips. Shader push constants
select byte-aligned mip offsets, so there are no per-mip buffers, descriptors or intermediate
buffer copies. Input mappings are flushed before dispatch. A compute-write → transfer-read
barrier precedes the normal image upload.
Jobs, descriptors, buffers and cached pipelines remain alive with the staging buffer's FenceCycle.
Inline uploads precede guest command nodes in CommandExecutor::SubmitInternal, so the decoder's
compute bindings are overwritten by subsequent guest commands.
No queue submit, fence reset, queue-idle call, CPU wait, or driver-loader change is added.

Invalid dimensions or per-texture buffer limits fall back to CPU for that texture.
Absent compute queues and allocation/pipeline setup errors fall back before command recording
and suppress repeated setup attempts for the session. A storage-buffer allocation failure
also falls back to an ordinary upload buffer.
Device loss is propagated rather than misrepresented as a recoverable decoder failure.
A runtime GPU error after recording cannot safely be recovered by re-running the CPU decoder.

## Validation

`python3 tests/test_bcn_compute.py` builds the production job with a small real Vulkan host harness
and compares it to the existing CPU decoder. Tested on Mesa lavapipe with the project's pinned
Vulkan-Hpp: all ten formats pass; max BC1/2/3 channel difference is 1, BC6H/BC7 are byte-identical.
All BC7 modes and all fourteen valid BC6H modes are covered, including 1×1, 2×1, 7×3 and 17×9
mips with different layer counts. An invalid source range is rejected before GPU work.

The cache update's Android APK and Mali device validation remain pending. No FPS or memory improvement is claimed.
On-device A/B checks should compare the same scene at the same resolution/driver with the setting
off/on, inspect emulation logs, verify textures and restart the game between changes. For NieR on
Mali, `NIER_BCN` logs the native support mask and bounded counts for `native`, `compute` and
`cpu` uploads; `cpuDecodeUs` measures only CPU decoding. If the driver advertises native BCn,
the compute setting does not replace native textures and may have no impact on FPS.

## Shader provenance

Vendored shaders and headers retain their MIT notices. `scripts/build_bcn_shaders.py` regenerates
the aligned SPIR-V arrays using glslc (built here with shaderc 2023.8). The generated header is
committed so the Android build does not need an extra shader compiler.
