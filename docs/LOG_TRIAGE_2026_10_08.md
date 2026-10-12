# Log triage — 2026-10-08

Base: 63689d6 (menu/inline keyboard), preserving resolution behavior and pausing further keyboard work.
Historical branches checked: resolution-scaling, strato-mali, perf/strato-mali-v1. Read current source and deepened history before editing. No duplicate GetInfo/resolution/keyboard fix applied.

| Log | Game | Evidence and disposition |
|---|---|---|
| 6 | Pokémon Let's Go Eevee | Missing HID/Bluetooth commands; cubeb minimum latency query fails then stream opens. No fatal termination in this log. Commands require individual ABI investigation; no blanket success stubs added. |
| 7 | KOF XIII Global Match | Raw termination 3535362 = 0x35F202 = FS NotMounted (2002-6905). Existing Result id field was 12 bits and incorrectly printed 2809. Fix Result encoding/decoding. Guest stack ends in GetFreeSpaceSize, but this does not identify which mount is missing. Do not mistake it for a capacity shortage or claim the game crash fixed. |
| 8 | Donkey Kong Country Returns HD | GetInfo 28 / VammManager abort. AliasRegionExtraSize is already implemented by 82dbc7d with argument validation and tests. Skip duplicate fix; old log alone does not prove current regression. |
| 9 | Fire Emblem Three Houses | PanVK queue timeout/device lost; texel-buffer descriptors unsupported. RomFS candidate warnings are followed by successful data storage access. Need a current driver log and descriptor-path work to establish cause; not a proven corrupt-ROM problem. |
| 10 | Persona 5 Royal | Shader compiler stubs and compressed/uncompressed view warnings; CRI audio underruns after extended runtime. Cannot infer a single performance bottleneck from log timestamps or guarantee an FPS gain. |
| 11 | Kirby and the Forgotten Land | 11,593 divisor warnings, 1,502 sRGB/UNORM view warnings, 176 invalid TIC formats. Divisor 1 requires no extension: skip redundant extension state and false warning. Real unsupported divisors remain warnings including binding/value. Mutable-format and invalid TIC handling remain open. |
| 12 | JoJo All-Star Battle R | PanVK external-memory import fails at swapchain creation. Separate driver/buffer-import issue, not solved by menu/resolution changes. |
| 13 | Cult of the Lamb | 24 MADV_REMOVE permission errors. Reclaim through a temporary writable shared alias only on EACCES, preserving live guest permissions and zeroing shared backing. Log uses old experimental pass scaling; do not reapply superseded resolution implementation. |
| 14 | Dark Souls Remastered | 414,642 inline stub warnings. Already addressed by 63689d6; no second keyboard fix. |
| 15 | Dark Souls Remastered | New inline session starts and four maxLength=14 requests reach Android bridge. Does not by itself prove IME display/submission success. Keyboard investigation paused at user's request. |

## Changes in this batch

1. Shared memory reclamation: keep the original MADV_REMOVE fast path; retry EACCES through an mremap alias with writable permissions; always unmap the alias. Do not use MADV_DONTNEED as a substitute: shared contents must be cleared, not merely evicted from one VMA.
2. HOS Result uses all 13 description bits and initializes reserved bits to zero when constructed from module/description. Preserve 4-byte ABI and raw-result construction.
3. Core Vulkan divisor 1 no longer requests unnecessary extension state or emits unsupported-extension warnings.

## Validation

- test_memory_reclaim.py exercises real shared/aliased mappings, untouched neighboring pages, read-only/PROT_NONE preservation, split VMAs, and rejection of private mappings. An injected EACCES reproduces older Android kernel behavior on hosts that now accept VM_MAYWRITE; actual alias/mprotect/madvise/munmap operations are not mocked.
- test_result_codes.py checks the actual Result header: KOF raw result, every 13-bit description for representative modules, reserved bits, and ABI size.
- Android compile and device game retests tracked separately; no blanket claim of fixed crashes or measured FPS improvements.

References: Linux madvise(2); Vulkan vertex input divisor specification; Switchbrew Error_codes; Atmosphere fs_results.hpp (NotMounted = 6905).

## Follow-up: ten additional logs and failed build

- Preserve commit 7518c59's memory reclamation, 13-bit results and divisor-1 fixes.
- Build 37810508187 failed because kernel::memory shadowed skyline::memory in memory.cpp and KMemory headers. Rename the helper namespace to memoryReclaim and add a namespace lookup regression to the existing memory test.
- Remove only the settings version TextView and its Kotlin binding access; keep the Strato Mali card/title.
- ARM proprietary driver: retain mutable-format image creation. The previous AFBC optimization omitted MUTABLE_FORMAT while guest games requested compatible sRGB/UNORM views. This corrects image creation, not merely log suppression. It can affect AFBC/performance; device game retests remain required. PanVK and Adreno policies are unchanged.
- Evidence: emulation (1) (6) Odyssey and (1) (7) Donkey Kong Country Returns HD repeatedly request sRGB/UNORM views. (1) (5) Mario 3D World and (1) (4) Link's Awakening also report them.
- Outstanding: (22) socket initialization/closed handle and 2010-0212; (26) audio GetWorkBufferSize and 2153-1537; (23)/(24) BOTW 1.0.0 unresolved __nnDetailInitLibc0 and reprotection failure; Link's Awakening guest thread crash. These are not established as fixed by this patch.
- BOTW 1.6.0 (1) (3) and Mario Kart (25) have no explicit fatal ending. Logs naming Fusion report ArmProprietary, not MesaPanvk.
- Validation on Android: memory reclamation and Result tests pass with ASan/UBSan, ASAN_OPTIONS=detect_leaks=0 because LeakSanitizer cannot run in this remote Android environment. XML parses and removed version binding has no remaining reference. Full build runs in GitHub Actions.
- Vulkan reference: https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewCreateInfo.html
