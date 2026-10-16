# Contributing

Thanks for wanting to improve the Beyond: Two Souls VR Mod. It is open source under the [MIT license](LICENSE): fork it, fix it, publish your own builds. Pull requests and bug reports are welcome.

## Building

You need Windows, **Visual Studio 2022** (or its Build Tools) with the C++ workload, **CMake 3.24+** and **[vcpkg](https://github.com/microsoft/vcpkg)** (for MinHook and the OpenXR loader, listed in `source/vcpkg.json`).

```
cmake -S source -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
      -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo
```

This produces `build\RelWithDebInfo\dinput8.dll`. To try it:
- copy it next to `BeyondTwoSouls_Steam.exe`, or `cmake --build build --config RelWithDebInfo --target deploy` (the game folder is found when `B2S_VR_MOD` sits inside it; otherwise configure with `-DB2S_GAME_DIR=<game folder>`), or
- put it in `files\` and run `Play Beyond Two Souls in VR.cmd`.

The `ctest` suites are desk tests of the pose maths, the first-person rules, controller mapping and the Steam Input table; they need neither the game nor a headset.

## How the mod works

- **Loading.** The mod is a `dinput8.dll` **proxy**: the game imports `dinput8.dll`, Windows finds ours next to the exe first, and our `DllMain` starts the mod on its own thread. The real DirectInput calls are forwarded to System32 (`src/dllmain.cpp`, `src/dinput8.def`).
- **One game build.** Every engine address is for one `BeyondTwoSouls_Steam.exe` build.
  - Each address has 16 guard bytes in `src/game.cpp`; the mod starts only when all of them match the running game, and otherwise logs `FAIL-CLOSED` and does nothing.
  - Steam's DRM decrypts the game code after the mod loads, so the check waits up to 60 seconds for the guards to match.
- **Stereo.** The engine's per-frame `VIEWPORT_LIST::Display` is recorded **twice per frame**, once per eye, with the eye's view and projection written at `CAMERA::Build` (`src/stereo.cpp`). A marker after each eye's 3D pass lets the render thread copy that eye's image, which is sent to the headset through **OpenXR** (`src/xr.cpp`). The interface (2D pass) is lifted onto its own quad layer.
- **First person.** `src/fpcam.cpp` decides when first person applies (`src/player.cpp` tells walking, cutscene and Aiden apart) and builds the camera from Jodie's head bone; `src/fpbody.cpp` draws her differently without touching the engine's skeleton: the edits (hidden head, body turned to the view, arms solved to the controllers, wrists retargeted from the OpenXR grip pose through the rig's own finger layout) are written into each mesh's skinning palette right after the engine copies the bones there (`MESH::PaletteCopy`), so game logic, IK and physics keep the original pose. The pure maths lives in headers (`fpmode.h`, `fp_math.h`, `posemath.h`) with desk tests in `tests/`.
- **Input.** The game reads pads through Steam Input; `src/vrinput.cpp` adds a virtual gamepad to the game's own Steam Input table and feeds it from the VR controllers.

| File | What is in it |
|---|---|
| `dllmain.cpp`, `game.*` | proxy entry, build check (anchors + guard bytes) |
| `probe.*`, `stereo.*` | engine hooks, the per-eye recording, UI layer, culling and post-effect fixes |
| `xr.*`, `xr_recenter.h`, `posemath.h` | OpenXR session, swapchains, head / controller tracking, recenter |
| `fpcam.*`, `fpbody.*`, `player.*`, `bones.*`, `camdirector.*`, `fpmode.h`, `fp_math.h` | first person |
| `vrinput.*`, `steam_shadow.h`, `vrinput_math.h` | VR controllers as a gamepad |
| `pacing.*` | frame limiter and VSync |
| `rendersize.*` | the square VR picture (`render_size`) |
| `hotkeys.*` | F1 / F2 / F3 / F9 |
| `crashreport.*`, `log.*`, `devctl.*` | crash reports, the log, development commands |

### Debugging

- **Log.** Everything the mod does goes to `b2svr_logs\b2svr.log` next to the game's exe (the previous run: `b2svr.prev.log`).
- **Privacy.** Players send the log, crash reports and `setup_log.txt` with bug reports, so they must never identify them. Every log line passes `LogScrub` (`src/log.cpp`): the game folder and user profile paths, the Windows user name and the computer name are replaced (`<game folder>`, `<user profile>`, `<user>`, `<pc>`); `tests/log_tests.cpp` checks it. The installer does the same for `setup_log.txt`. PC specs (Windows version, CPU, GPU, RAM, headset) are fine; never log account names, IDs, serial numbers or anything outside the game.
- **Commands.** Write lines into `b2svr_cmd.txt` next to the exe while the game runs; they are executed within a few frames. Examples:
  - `xr on` / `xr off`
  - `recenter`
  - `fp on` / `fpcutscenes on` / `fparms off`
  - `fpeye 0.09` / `fpeyeup 0.07` / `fpwrist 10` (first-person eye position and hand tilt, live)
  - `fpstatus` (first-person state in the log)
  - `crashreport` (writes a sample crash report)

  See `Execute` in `src/devctl.cpp` and the `Command` functions it calls for the full list.
- **Crashes.**
  - Faults inside the game or the mod are logged (first chance) with registers and a stack.
  - A real crash also writes `b2svr_logs\crash_reports\crash_<date>.txt`, the file players send. The game's own crash handler still runs after it.
  - Keep the `dinput8.pdb` of every build you release, so a `dinput8.dll+0x...` offset from a report can be mapped to a function.
- **Installer.** `tools\b2svr_setup.ps1 -Mode Check` shows what is installed without changing anything; `-NoLaunch -Unattended -GameDir <folder>` runs Play / Uninstall against a test folder without questions or starting anything.

## Guidelines

- **Fail safe.** On anything unexpected (wrong build, guard bytes differ, a call fails), log it and leave the game as it is.
- **Threads.** The game records frames on its main thread and draws them on a render thread. Keep shared state atomic or locked, and never block the render thread on the game thread.
- **No game files.** Never add the game's files, decompiled game code or data extracted from the game to this repository.
- **Comments.** Explain *why*, and keep the reason for each fix next to the code.

## Making a release

1. Build `RelWithDebInfo` and run the tests.
2. Copy `dinput8.dll` into `files\` and update `files\BUILD.txt` (version, SHA-256, date).
3. Keep that build's `dinput8.pdb` privately (it is not part of the release).
4. Test `Play Beyond Two Souls in VR.cmd` and `Uninstall VR mod.cmd` on a clean game folder.
