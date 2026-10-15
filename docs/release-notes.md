# KytyPS5 U59 int19.3

int19.3 is int19.2 plus the fifth sync with upstream KytyPS5, a device-loss fix, a shader accuracy fix and some opt-in switches; int19's notes are in docs/release-notes-history.md and on the int19 release page. The first launch rebuilds the shader and program caches once, so the first minutes can stutter.

- **Fixed (GPU device loss with sparse textures):** when a partially resident texture got more mip levels, Kyty bound the new memory on a second Vulkan queue while work already sent to the GPU could still use that texture, which the Vulkan specification does not allow. The bind is now ordered on the GPU after the work in flight, and later work waits for it. This fixed the `ErrorDeviceLost` in the user's galaxy-map case; confirmation on an RTX 4090 and an RTX 5070 is still pending.
- **Fixed (shader accuracy):** `V_CVT_PKNORM_I16_F32` and `V_CVT_PKNORM_U16_F32` now turn NaN into 0, as the RDNA2 instruction set specifies (the signed form gave 0x7fff).
- **Upstream sync 5** (143 upstream commits). Kyty's own defaults and the `u59-preset.json` behaviour stay as in int19.2. Taken from upstream, among others:
  - Block-compressed (BC) textures in the PS5 R64 tiling mode are now accepted by the tiler (upstream b7b335ac6, `tile.cpp`).
  - Pixel shaders: in ordinary draws, flat and smooth reads of the same vertex output now share one location and read the raw vertex values; rectangle lists keep their old handling. `KYTY_PS_PER_VERTEX=0` also turns this off.
  - More GPU instructions and fixes (16-bit buffer loads, 64-bit compares, 64-bit LDS max and compare-exchange, signed 32-bit image atomics, R8 SNORM sampling, CMASK and RG32 clear decoding), uploads of images larger than the upload ring, a lost-completion fix for batched asynchronous file reads (APR), HDR presentation, the splash screen and a reorganised launcher (Kyty's audio sliders, occlusion and Intel CPU options are kept).
  - Controllers: a game can now open only the standard pad port of the configured user, and only once. The first pad opens are printed in the console log, so a game that opens another port shows up there.
- **New opt-in switches** (all off unless set):
  - `KYTY_GPU_TIMESTAMP_SCALE=<100-200>`: stretches the GPU time the game measures within each frame by that percentage, so Astro Bot's dynamic resolution keeps a lower internal resolution.
  - `KYTY_AFTERMATH=1`, for developers: NVIDIA Nsight Aftermath crash dumps on a device loss. Release builds do not include it; it works only in a build configured with the Aftermath SDK (CMake `KYTY_AFTERMATH_SDK`).
  - `KYTY_AGC_CB_WATERMARK=1` and `KYTY_GUEST_PROBE`: diagnostics for Astro Bot's "too many objects/batches" stop (see docs/EXPERIMENTAL.md).
  - `KYTY_READ_FAULT_DIAG` and `KYTY_READ_FAULT_PUBLISHED`: a diagnostic and an experiment for game threads that read GPU-written memory.
- **Known issue:** if you still get a GPU device loss (`ErrorDeviceLost`), reproduce it once with `KYTY_DEVICE_FAULT_DIAGNOSTICS=1` and Printf output set to File, then attach `_kyty.txt` and the `_kyty` folder to an issue. Workaround: `KYTY_DEFER_PROTECTS=0` in `u59-preset.json`.
- **AMD, Intel and Linux users:** their fixes are in the int19 notes.

## int19.2

- **Fixed:** with the game's "[Performance]" patches on (no grass, SSAO/DoF/SSR off, ...), the emulator no longer closes ("SPIR-V validation failed ... buffer ADD_TID is only valid for compute shaders") and no longer drops to a few fps and freezes at some spots in Sky Garden. Leftover descriptors in slots the game does not use no longer compile a new shader version for every value.

## int19.1

- **Fixed:** the emulator no longer closes (`occlusion.cpp:405`) with "Accurate occlusion queries" on, seen in rocket_hourglass.
- **Faster:** the release build's profile-guided optimisation profile was retrained on the int19 code, a few percent more fps in Sky Garden on an RTX 3090 / Ryzen 9 7950X3D.
