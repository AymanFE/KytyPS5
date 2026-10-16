# KytyPS5 U59 int19.4

int19.4 is int19.1 plus the unreleased int19.2 and int19.3 work, more performance work and the latest upstream KytyPS5 changes; the older notes are in docs/release-notes-history.md. The first launch rebuilds the shader and program caches once, so the first minutes can stutter.

- **Faster:** a few percent faster in scenes limited by the command processor. GPU compute dispatches the game does not order now overlap, shader code generation is tighter, more draws reuse the previous draw's state, empty indirect dispatches are skipped, and the release build has a new profile-guided optimisation profile recorded over several levels.
- **New (automatic V-cache placement on dual-CCD X3D CPUs):** on a Ryzen 9 7900X3D, 7950X3D, 9900X3D or 9950X3D without Process Lasso, Kyty keeps the game, command-processor and render threads on the CCD with the 3D V-cache and moves background threads to the other CCD. It does nothing when Process Lasso (or another tool) sets CPU sets or affinity for Kyty, and nothing on other CPUs. `KYTY_VCACHE_PLACEMENT=0` turns it off.
- **Fixed:** with the game's "[Performance]" patches on, the emulator no longer closes ("buffer ADD_TID is only valid for compute shaders") or freezes at some spots in Sky Garden (from int19.2).
- **Fixed:** a GPU device loss (`ErrorDeviceLost`) when a partially resident texture got more mip levels, and NaN handling of two pack instructions (from int19.3).
- **From upstream KytyPS5:** shader and texture fixes for other games (among them Demon's Souls), DLC and add-on support, input mapping tabs in the launcher with an emulated touchpad and rumble, and more system modules.
- **Launcher renamed:** the launcher is now `kyty_launcher.exe`. Extracting over an old folder leaves the old `launcher.exe` behind; delete it and start `kyty_launcher.exe`.
- **Known issues:** in some levels the game can stop with its own "too many objects/batches in this scene" error (`command_buffer_writer.ppr.cpp:31`); this is under investigation. If you get a GPU device loss, reproduce it once with `KYTY_DEVICE_FAULT_DIAGNOSTICS=1` and Printf output set to File, then attach `_kyty.txt` and the `_kyty` folder to an issue; workaround: `KYTY_DEFER_PROTECTS=0` in `u59-preset.json`.
- **AMD, Intel and Linux users:** their fixes are in the int19 notes.
