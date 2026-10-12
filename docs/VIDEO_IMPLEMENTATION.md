# NVDEC/VIC video playback

Status: experimental implementation; Android and real-game playback validation pending.

The previous branch applied Host1x relocations and advanced syncpoints without
executing NVDEC/VIC commands. Audio could play but no video frames were generated.

This branch executes owned command gathers on Host1x workers, retains NvMap pins
until completion and stops workers before NvMap destruction. Syncpoint conditions
are completed on the worker, after preceding synchronous engine work.

NVDEC reconstructs H.264/VP8/VP9 packets using Eden/Yuzu code, decodes with pinned
FFmpeg n6.1.2 software decoders, and passes frames through a synchronized queue.
VIC reads guest configuration, applies its color matrix and writes RGBA/BGRA/XRGB
or NV12 surfaces to SMMU memory in pitch or Tegra block-linear layout.

Reference: Eden 746a6d9160c817a173d97f645c7b8abf75020f32. Original attribution
is preserved; license texts and notices are bundled under assets/licenses/video.

## Scope and remaining limitations

Progressive 8-bit YUV420/NV12 input and a single opaque VIC slot are implemented.
Advanced VIC filtering, interlacing, transpose and alpha composition are not
implemented. Explicit errors identify unsupported output paths. VP9 packet
reconstruction is ported but has not yet passed an end-to-end sample test.
CPU decoding may be slower than hardware decoding; no PanVK, renderer, UI or
frame-pacing behavior is changed by this work.

## Validation

`tests/test_video_pipeline.py` compiles the actual portable video module with
ASan/UBSan, reconstructs synthetic H.264 and VP8 bitstreams, decodes them and
checks guest pixels against known red-color values. Pitch and GOB heights
1 through 32 and out-of-range address rejection are covered.

`tests/test_host1x_relocations.py` exercises the production submission method:
relocations, malformed request rejection, failure atomicity, immutable command
copies, deferred syncpoint completion and pin retention/release.

Passing these tests does not establish playback in Donkey Kong. The Android build
and opening cinematic still require validation on the device.

## Source rectangle clipping

A reported Persona 5 Royal crash ended in `Invalid VIC source rectangle` on
the Host1x worker. The old check rejected source extents outside decoded pixels,
even though a guest surface may include padding. The compositor now clips
available source pixels while retaining the original destination transform and
configured background outside the decoded image. Inverted rectangles still fail
with raw coordinates and decoded dimensions in the error.

A nonuniform YUV fixture verifies padding, nonzero crop origins and scaling;
the previous implementation fails this test. The original H.264/VP8 and GOB
tests remain. The report did not include rectangle dimensions, so confirmation
that this addresses Persona's specific request requires another device test.
