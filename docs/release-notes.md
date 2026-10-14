# KytyPS5 U59 int19.2

int19.2 is int19.1 plus one fix; int19's notes are in docs/release-notes-history.md and on the int19 release page. The first launch rebuilds the shader and program caches once, so the first minutes can stutter.

- **Fixed:** with the game's "[Performance]" patches on (no grass, SSAO/DoF/SSR off, ...), the emulator no longer closes ("SPIR-V validation failed ... buffer ADD_TID is only valid for compute shaders") and no longer drops to a few fps and freezes at some spots in Sky Garden. Leftover descriptors in slots the game does not use no longer compile a new shader version for every value.
- **Known issue:** rare GPU device loss (`ErrorDeviceLost`) on NVIDIA is under investigation; none reported by players so far. Workaround: `KYTY_DEFER_PROTECTS=0` in `u59-preset.json`. To report, reproduce once with `KYTY_DEVICE_FAULT_DIAGNOSTICS=1` and Printf output set to File, then attach `_kyty.txt` and the `_kyty` folder to an issue.
- **AMD, Intel and Linux users:** their fixes are in the int19 notes.

## int19.1

- **Fixed:** the emulator no longer closes (`occlusion.cpp:405`) with "Accurate occlusion queries" on, seen in rocket_hourglass.
- **Faster:** the release build's profile-guided optimisation profile was retrained on the int19 code, a few percent more fps in Sky Garden on an RTX 3090 / Ryzen 9 7950X3D.
