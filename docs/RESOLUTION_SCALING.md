# Experimental texture-cache resolution scaling

The selector is in global Settings > Graphics and per-title settings: 0.50x,
0.75x, 1x (default), 1.25x, 1.50x, 1.75x and 2x. Restart the game after changing
it. Invalid preference indices use 1x.

## Integration checkpoint

This work builds on `feature/resolution-scaling` at
`e7291091b61388af2993a67042258c69e940354e`.
Commit `5bd431b8e707dc36444666a9697b99610d9d68bc` passed the Android arm64
build and every workflow test in [run 37716191859](https://github.com/LukeValen/Strato-Mali/actions/runs/37716191859).
The APK is available as the `Strato-Mali-APK` artifact. The user observed
resolution scaling working in Celeste with docked mode disabled on the device.
The exact factor, driver version, performance and docked behavior were not
recorded. Other games and settings still need device validation. Build and
host-test success do not establish Eden parity or crash freedom.

- A texture-cache entry owns a persistent scaled companion. Its original image,
  guest allocations, tiling, pitches and addresses remain native.
- Compatible 2D views share that companion, including their component swizzles
  and depth/stencil sampling aspect. Graphics descriptors select views at command
  recording time, after preceding render passes have established their contents.
- The pinned shader compiler's RescalingPass receives the selected rational
  factor. A 32-byte push-constant block supplies the render down factor and the
  per-descriptor texture bitset across graphics stages. Full descriptor updates
  are used for scaled sessions so a cached native descriptor cannot survive a
  native-to-scaled transition.
- The pinned compiler declares `Settings::values` with internal linkage. A small
  CMake include override changes it to one inline variable shared by renderer,
  translation and optimization. The submodule itself is not modified.
- Render area, viewport, scissor, clears and Fermi2D helper draws use the selected
  factor. Partial render areas and mixed attachment extents are accepted when
  all views and bounds are valid. Framebuffer dimensions fit every attachment.
- A version counter invalidates scaled contents on CPU uploads, native passes
  and image copies. Existing scaled contents are reused instead of being rebuilt
  from the native image at every render pass. Uploads wait for prior recording
  before changing this version.
- Same-format full color copies preserve scaled companions when possible.
  Presentation uses a valid scaled source directly when format and subresource
  constraints permit. Fermi2D samples the current companion with normalized UVs.
- Native images are still resolved eagerly after scaled passes. This conservative
  coherency policy keeps CPU readback, compute, incompatible aliases and existing
  native consumers correct; it retains a bandwidth cost compared with Eden's
  more extensive lazy texture-cache synchronization.
- Sampled companions, images, views and framebuffers remain alive through their
  submission fence. Extents, supported formats and a 128 MiB reservation budget
  are checked. Optional allocation failures fall back; device loss still propagates.

## Remaining limits

Layered/mip render views, MSAA, non-triangle topology, attachment feedback, storage
writes, indexed fragment attributes, precise occlusion queries and unsupported
formats retain native rendering. Descriptor arrays retain native sampling because
one compiler scaling bit covers the entire array. Format-reinterpreting aliases
and non-base-mip sampling retain native views. Compute remains native; the existing
Strato image/storage descriptor implementation is incomplete and is not filled in
by this change. Compiler ABI overflow and render-area push-constant conflicts use
an explicitly unscaled shader variant. Host SPIR-V replacements are not substituted
for shaders requiring rescaling constants, since they have not been patched.

Fractional factors truncate image extents like the pinned compiler. Very small or
odd dimensions can therefore have rounding differences, including inverse integer
image-size queries. These require game-level validation. This is not full Eden parity.

## Verification

- `tests/test_resolution_scaling.py`: all seven ratios, tiny/odd dimensions,
  invalid indices, overflow, device/memory limits, constant-buffer layout and
  settings visibility between separate translation units, under ASan/UBSan.
- `tests/test_resolution_vulkan.py`: 14 host Vulkan cases (seven factors, even and
  odd sizes), checking shader coordinates, descriptor bitsets, color/depth/stencil
  readback and a subsequent native draw, with synchronization validation enabled.
  Its GLSL test shader exercises the ABI; it does not run a translated game shader
  or instantiate the full emulator's texture manager.
- Android arm64 release compilation, Kotlin/resources validation and all workflow
  regression tests passed for commit `5bd431b`.
- Celeste: user-reported visible scaling with docked mode disabled; no
  measurements or factor-by-factor results supplied yet.
- Full-renderer validation, translated shader tests and Mali/PanVK device
  measurements remain required before release.

## Device validation

Record APK commit, driver, title/version, docked mode and display refresh rate.
Compare the same scene at 1x, 0.50x, 0.75x, 2x, then the intermediate factors,
restarting between settings. Capture screenshots and frame times, menus/HUD,
post-processing, depth/stencil effects, cutscenes and several minutes of gameplay.
Check `Internal resolution scale`, `Scaled texture cache image` and native-fallback
log entries. A setting label alone is not evidence of scaled rendering. Restore 1x
and verify the baseline again. Compare with Eden independently on the same device.
