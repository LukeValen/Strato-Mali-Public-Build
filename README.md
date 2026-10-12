# Strato Mali

## 💬 Community / Telegram

Join the **Strato Mali Telegram community** for development updates, compatibility discussions, testing feedback, and bug reports. 🫐

👉 **[Join Strato Mali on Telegram](https://t.me/+XkmbXMK4c0BhMjZh)**

**Strato Mali** is a public development fork of [Strato](https://github.com/strato-emu/strato) focused on Mali GPUs, Kbase/CSF and experimental Mesa PanVK support on Android.

The development branch is based on Strato's protected `master` history at commit `ae1566a48285816a87e81d4aeb40bd2f4e56e60b` ("Fix accidental recursion with trap handling"). The upstream `jit` branch is preserved separately for later evaluation; it is not merged into the stable base by default.

## Mali / PanVK integration

Strato already contains a custom Vulkan driver package system and an Android linker-namespace loader through libadrenotools. Strato Mali extends that path for Mali:

- custom GPU driver selection is enabled when either `/dev/kgsl-3d0` or `/dev/mali0` is present;
- Mali custom Vulkan drivers can be loaded from Strato's existing `gpu_drivers` directory;
- the KGSL/libgsl direct-memory-import hook is enabled only on Adreno/KGSL and is explicitly disabled on Mali/Kbase;
- Adreno-only maximum-clock controls remain Adreno-only;
- Mali keeps Strato's normal buffer upload path rather than attempting KGSL memory-import ioctls.

The first target is the Mali-G615 MC6 / Kbase CSF stack used for PanVK development. PanVK support is experimental and is not presented as conformant or production-ready.

## Branches

- `strato-mali` — Strato Mali development branch.
- `master` — exact upstream stable Strato master baseline.
- `jit` — upstream experimental JIT branch preserved for reference/testing.

## Upstream and credits

Strato Mali is derived from **Strato**, which continues the work of **Skyline**. The original Strato/Skyline authors and contributors retain credit for their work. This fork also uses Mesa/PanVK work and a Kbase PanVK fork maintained for the Mali integration.

See the repository history and `LICENSE.md` for license and attribution details.

## License

The Strato codebase is licensed under **GNU GPL v3.0 or later**. Existing third-party components and submodules retain their respective licenses. This fork does not remove or replace upstream copyright notices.
