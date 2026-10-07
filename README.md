# Zero Hour x64 / Vulkan (experimental fork)

An experimental, personal fork of [TheSuperHackers/GeneralsGameCode](https://github.com/TheSuperHackers/GeneralsGameCode)
(the community-maintained source of *Command & Conquer: Generals* and *Zero Hour*, originally released by EA under the
GPL v3). **This version is based on the community fix version**: it is the community's bug-fix code base (the one behind the
"Community Patch" builds) with the changes below added on top. All of the community's fixes and the retail-compatibility
work come from that project, not from this fork. It tries to make **Zero Hour** build and run as a **64-bit program on top of Vulkan**, and adds a handful of
updated visual effects.

> **One-time project: no support, no updates.**
> This was done once, as a personal experiment. It will **not** be troubleshot, answered in issues, or kept up to date with
> upstream or with new tools. Fork it, change it, take pieces of it, do whatever you like with it (within the license, see
> [Licenses and credits](#licenses-and-credits)). Treat it as a starting point, not as a maintained product.

> **Please read this first.**
> This is a hobby project by one person with one test machine. It works for the author on that machine, in the situations
> listed under [Tested](#what-was-tested). Everything else is unknown. Expect rough edges, crashes and things that were
> simply never tried. There is no support, no guarantee, and no promise that it works on your system.
> It does **not** contain the game. You need your own legitimate copy of *Command & Conquer: Generals - Zero Hour*.

Not affiliated with, or endorsed by, Electronic Arts. *Command & Conquer*, *Generals* and *Zero Hour* are trademarks of
their owners.

---

## Contents

1. [What this is (and is not)](#what-this-is-and-is-not)
2. [Summary of the changes](#summary-of-the-changes)
3. [What was tested](#what-was-tested)
4. [Requirements](#requirements)
5. [Building](#building)
6. [Installing into your game folder](#installing-into-your-game-folder)
7. [Running and configuration](#running-and-configuration)
8. [Detailed description of every change](#detailed-description-of-every-change)
9. [Known problems and limitations](#known-problems-and-limitations)
10. [Troubleshooting](#troubleshooting)
11. [Repository layout of the additions](#repository-layout-of-the-additions)
12. [Licenses and credits](#licenses-and-credits)

---

## What this is (and is not)

**It is**
- The upstream source tree plus a set of changes that let `GeneralsMD` (Zero Hour) compile for x64 and run through
  [DXVK](https://github.com/doitsujin/dxvk) (Direct3D 8 translated to Vulkan).
- Build scripts that fetch/build everything that is not in this repository.

**It is not**
- A game download. No game data, no executables from the original game, no archives (`.big`), no videos, no music.
- A new renderer. The game still renders through its original Direct3D 8 code; DXVK translates that to Vulkan at run
  time. A native Vulkan renderer is only a documented idea, see [docs/PORT_NOTES.md](docs/PORT_NOTES.md).
- A multiplayer-safe build. See [Known problems](#known-problems-and-limitations).
- A drop-in replacement for the upstream project. If you want a well supported build with retail compatibility, use
  upstream: [README_UPSTREAM.md](README_UPSTREAM.md) and the link above.

## Summary of the changes

Short version; every item is described properly in
[Detailed description of every change](#detailed-description-of-every-change).

| Area | Change |
|---|---|
| 64-bit | Zero Hour builds as x64 (`scripts/build-x64.ps1`). Replacements for the 32-bit-only pieces: a DirectX 8 helper library (`d3dx8`), the Miles sound system (`mss32`) and the Bink video player. |
| Graphics API | Runs on DXVK (Vulkan). The x64 launcher uses a **lightly patched DXVK** (one condition removed) so a pixel shader above model 1.x is accepted on a Direct3D 8 device. |
| Frame rate | Rendering is no longer tied to the 30 Hz game logic. With `FPSLimit = no` the picture is drawn as fast as the GPU allows while the simulation stays at 30 Hz. |
| Water | Updated water shaders: depth from the terrain, refraction, new wave shapes, reflections, foam, ripples and wakes around units on the water. Falls back to the original water if the shaders cannot be created. |
| Explosions | A screen-space shockwave ring on explosions that shake the camera or scorch the ground. |
| Content filter | A `-uncensored` command line flag that makes the game ignore localized (e.g. German) model/texture overrides when a base version exists. No assets are included. |
| Housekeeping | Many 64-bit pointer-size fixes, registry lookup for 32-bit registered installs, crash-handler stubs for x64, DDS header size fix, fullscreen and cursor handling notes. |

## What was tested

Everything below was done by the author, on one machine. Nothing else was tested.

**Machine**
- Windows 11 Pro (build 26200), x64
- NVIDIA GeForce RTX 4070 Ti, Vulkan through DXVK 3.1.1
- Visual Studio 2022 (MSVC 14.44), CMake, vcpkg (FFmpeg)

**Game install**
- *Zero Hour* 1.04 from the **EA App**, **German** edition (this matters: the language/registry handling and the
  `-uncensored` flag were developed against it)

**What was run**
- Main menu and the animated shell map: yes, many times, windowed (1600x900) and fullscreen (1920x1080)
- Loading a map directly with `-map` and watching the in-game frame counter: yes (Flooded Plains)
- Starting a skirmish from the menu: yes, briefly. The frame limit fix for skirmish start was verified through the
  direct map load, not by clicking through the skirmish menu
- Sound, mouse cursor and videos: yes
- Explosions with the shockwave ring, ships/hovercraft on water in the shell map: yes

**Not tested at all**
- Campaign missions, Generals Challenge, saved games, replays, the map editor/tools
- LAN, online or any lock-step multiplayer, and mixing this build with the retail 32-bit game in one match
- The original *Generals* (non Zero Hour) target
- 32-bit builds after the latest changes (the 32-bit + stock DXVK configuration worked earlier but was not re-verified)
- Other GPUs and drivers (AMD, Intel), Windows 10, Linux/Proton, ultrawide or very high DPI setups
- English or other language installs, Steam/disc installs
- Long play sessions (memory use, leaks, stability)

## Requirements

**To build**
- Windows 10/11, x64
- Visual Studio 2022 with the *Desktop development with C++* workload (includes CMake and the Windows SDK)
- Git
- [vcpkg](https://github.com/microsoft/vcpkg) (used for FFmpeg); set the environment variable `VCPKG_ROOT` to your checkout
- Python 3 with `pip` (only for building the patched DXVK)
- Internet access for the first build (CMake fetches the DirectX 8 headers and other dependencies, vcpkg builds FFmpeg,
  the DXVK script downloads DXVK and glslang)

**To run**
- A legitimate Zero Hour installation (1.04)
- A GPU and driver with a recent Vulkan implementation (DXVK 3.x asks for Vulkan 1.3)

## Building

All commands in PowerShell, from the repository root.

```powershell
# 1. get the sources
git clone <this repository> GeneralsZH-x64
cd GeneralsZH-x64

# 2. point the build at your vcpkg checkout (FFmpeg is built through it)
$env:VCPKG_ROOT = "C:\path\to\vcpkg"

# 3. build the game (first run takes a while: FFmpeg is compiled by vcpkg)
powershell -File scripts\build-x64.ps1 -Video
#   -Video  : with FFmpeg so the game's .bik videos play. Without it the build is smaller and videos are skipped.
#   -Clean  : rebuild everything
# Output: build\win64-vcpkg\GeneralsMD\Release\generalszh.exe (+ FFmpeg DLLs next to it)

# 4. build the patched DXVK (needs step 3 once, it reuses the DirectX 8 headers CMake fetched)
powershell -File tools\dxvk\build-dxvk.ps1
# Output: tools\dxvk\work\out\d3d8.dll d3d9.dll dxgi.dll
```

Notes
- `scripts/build-x64.ps1` creates a tiny `afxres.h` shim in `build/shim` because the resource file expects MFC's header.
- The shader headers (`Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/Shaders/*PS.h`) are committed, so you do
  not need the DirectX shader compiler. If you change the `.hlsl`, run `scripts\compile-shaders.ps1` (needs `fxc.exe` from
  the Windows SDK) and rebuild.
- `tools/dxvk/0001-*.patch` is the whole DXVK change (a few lines in `src/d3d9/d3d9_device.cpp`).

## Installing into your game folder

```powershell
powershell -File tools\install-to-game.ps1 -GameDir "D:\Games\Command and Conquer Generals Zero Hour"
```

This only **adds** things:

```
<GameDir>\x64\generalszh64.exe          the game you built
<GameDir>\x64\avcodec-*.dll ... z.dll    FFmpeg and zlib, from the build output
<GameDir>\x64\d3d8.dll d3d9.dll dxgi.dll the patched DXVK
<GameDir>\Play-x64.bat                  windowed launcher
<GameDir>\Play-x64-Fullscreen.bat       fullscreen launcher
```

Nothing of the original game is modified. To uninstall, delete the `x64` folder and the two `.bat` files.
The game must be started through the `.bat` files (or with the same arguments): they set the working directory and
`-useCwd`, which the executable needs to find loose `Data\Cursors` and `Data\Movies`.

> Do **not** copy `generalszh64.exe` over the original `generalszh.exe` and do not run it without the DXVK DLLs next to
> it. The original game files stay as they are, and without DXVK you will get a "DirectX 8 not available" style error.

## Running and configuration

### Launching

`Play-x64.bat` starts windowed, `Play-x64-Fullscreen.bat` starts fullscreen. Extra arguments are passed through, for
example `Play-x64.bat -xres 1920 -yres 1080`.

Useful command line flags (some are upstream, some are new, marked *new*):

| Flag | Meaning |
|---|---|
| `-win` | windowed mode |
| `-xres N -yres N` | resolution |
| `-useCwd` | keep the current folder as working directory (*needed by this port*, set by the launchers) |
| `-mod <folder or .big>` | load an extra archive/folder that overrides the game's data (upstream) |
| `-uncensored` | *new*: ignore localized model/texture overrides when a base version exists, see below |
| `-quickstart`, `-noshellmap`, `-map <file>` | upstream shortcuts, handy for testing |

### Options.ini keys (`%USERPROFILE%\Documents\Command and Conquer Generals Zero Hour Data\Options.ini`)

The game rewrites this file when it exits, so edit it while the game is closed.

| Key | Default | Meaning |
|---|---|---|
| `FPSLimit` | game default (limit on) | `no` = draw as fast as possible; game logic stays at 30 Hz. `FPSLimit = no` is now respected when a game starts (it used to be overridden). |
| `UpdatedWater` | `yes` | `no` = original water rendering. Also used automatically when the updated shaders cannot be created. |
| `WaterReflections` | `yes` | `no` = skip the extra mirrored render of the scene that the water reflection needs. |
| `Shockwaves` | `yes` | `no` = no shockwave ring on explosions. |
| `Resolution` | game default | Use a resolution your display really supports at fullscreen; a 4:3 mode on a 16:9 panel hung the old exact-mode fullscreen in testing. |

### The `-uncensored` flag

Some regional editions replace models and textures through localized archives (for example `Data\<Language>\Art\W3D`).
With `-uncensored` the game prefers the model from the base archives or from a mod when one exists, and ignores
localized textures whose name contains `drone`. **No assets are included or downloaded.** What the flag finds depends
entirely on the files in your own installation and on any mod you load with `-mod`. Behaviour was only examined with a
German install. Do not expect it to change anything on an install that has no such overrides.

### Frame rate

By default the game limits itself to 30 fps like the original. Set `FPSLimit = no` for uncapped rendering. Game logic
(unit movement, AI, timers) stays at 30 Hz, so game speed does not change; only drawing and camera movement get smoother.
On the author's machine the in-game counter showed roughly 450-1000 fps on the shell map and an empty skirmish map.

## Detailed description of every change

### 1. 64-bit build

| Change | Where | Why |
|---|---|---|
| x64 CMake presets `win64` and `win64-vcpkg` | `CMakePresets.json`, `CMakeLists.txt`, `cmake/dx8.cmake` | Upstream only has 32-bit presets. `dx8.cmake` additionally generates an x64 `d3d8` import library. |
| `scripts/build-x64.ps1` | new | One command build, resource-header shim, optional FFmpeg. |
| `Dependencies/D3DX8Compat` | new | The original `d3dx8.lib` exists only for x86. This library re-implements the few functions the game uses: matrix/vector math, DXT1/3/5 and uncompressed surface conversion, mip generation, texture creation. `D3DXAssembleShader` and `D3DXCreateFont` intentionally fail (not needed on this path). |
| `Dependencies/Miles/MiniMiles.cpp`, `Dependencies/miniaudio` | new | `mss32.dll` (Miles Sound System) is 32-bit only. `MiniMiles` implements the ~70 Miles calls the game uses on top of the vendored [miniaudio](https://github.com/mackron/miniaudio): WAV/ADPCM/MP3 decoding, loop counts, 3D pan and attenuation, end-of-sample callbacks. `MilesAudioManager` is almost unchanged (only handle-passing fixes). |
| Video through FFmpeg | `-DRTS_BUILD_OPTION_FFMPEG=ON`, vcpkg manifest | The Bink SDK is 32-bit only; the engine's existing FFmpeg video player is used, FFmpeg comes from vcpkg and is loaded as DLLs. |
| DDS header size | `ddsfile.h` (Generals and GeneralsMD) | A `void*` member made the legacy surface description 136 bytes instead of 124 on x64, so every DDS texture failed to load (magenta/missing textures). |
| Registry lookups | `registry.cpp`/`registry.h` (several) | Installs registered by the 32-bit EA App live under `WOW6432Node`; the x64 build reads that view (`KEY_WOW64_32KEY`) so language and install path are found. |
| GUI message data | `GameWindow.h` | `WindowMsgData` is pointer sized because the GUI passes pointers through it. |
| Pointer-size cleanups | ~40 small edits (`thread.h`, `SoundSceneObj.h`, `PartitionManager.cpp`, `IMEManager.cpp`, `GadgetListBox.cpp`, `LocalFile.cpp`, `persistfactory.h`, `huffencode.cpp`, `surfaceclass.cpp`, INI parsers, menu callbacks, ...) | Mostly `(Int)pointer` casts replaced with `(intptr_t)`/`(uintptr_t)` so values are not truncated on x64. These silence compiler warnings and remove actual truncation bugs. Some warnings remain, see [limitations](#known-problems-and-limitations). |
| Crash/stack code | `Except.cpp`, `debug_except.cpp`, `debug_stack.cpp`, `StackDump.*` | The register/stack dump code is x86-only. On x64 it records only the exception code and address. |

### 2. Frame pacing

- `FramePacer`: when the render cap is off the logic now keeps its own 30 Hz instead of speeding up with the render rate.
- `GameLogicDispatch.cpp` and `GameLOD.cpp` previously switched the 30 fps limit back on when a game started or when a detail
  preset was applied. They now check whether `Options.ini` explicitly says `FPSLimit = no` (`OptionPreferences::isFPSLimitDisabledByUser`)
  and leave the cap off in that case. `GameLOD.cpp` also applies the option at start-up.
- The in-game options menu and skirmish start code needed only the pointer-size fixes listed above.

### 3. Updated water

Files: `Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/` (`W3DWater.cpp`, `Shaders/WaterModern.hlsl` and the two
compiled headers), `W3DWater.h`, `OptionPreferences.*`, `W3DDisplay.cpp` (one line), `tools/dxvk`.

The game's own water code is kept. When the updated shaders are available they are used for the translucent water (the
water type every map in the game uses). They are pixel shader model 3.0 (`WaterModern.hlsl`, compiled in two variants:
`RIVER` for river polygons and `TRAPEZOID` for area water such as the sea).

| Feature | How it works |
|---|---|
| Depth | The terrain height of the whole map is stored in a texture (one texel per height-map vertex) and sampled in world space. Water depth is therefore known for every pixel, independent of the coarse water mesh. |
| Refraction | The back buffer is copied once per frame before the water is drawn and sampled through a wave-distorted lookup. Per-channel absorption by depth (red first) gives shallow-to-deep colour changes. |
| Waves | Five directional waves plus fine noise, with a noise-warped phase to avoid an obvious lattice. They drive the shading, sun glints and the reflection fresnel term. |
| Reflection | The existing mirror pass of the game (`renderMirror`) is reused at the real water height into a 512 px texture, with a clip plane so the sea floor is not reflected. If it is off or unavailable a sky gradient is used. From the game's usual camera angle most of the reflection is sky. |
| Foam | A band along the shoreline, bands drifting towards the beach and occasional whitecaps. |
| Units on water | Up to six ships, hovercraft or wading units nearest to the view centre disturb the surface: ripple rings around idle hulls and a V-shaped foam wake behind moving ones. Speed and heading are smoothed over about a quarter of a second to avoid flicker. Aircraft and structures are ignored. |
| Fallback | If the shaders cannot be created (stock DXVK, a real Direct3D 8 device, 32-bit launchers) the original water shaders are used, with a simple depth tint baked into the vertex colours. `UpdatedWater = no` forces this. |

Why a patched DXVK: DXVK's Direct3D 8 front end rejects pixel shaders above 1.x (log message
`CreateShaderModule: Unsupported PS version 2.0`). The patch in `tools/dxvk/` removes that one clamp. The rest of DXVK
3.1.1 is unchanged. `tools/dxvk/build-dxvk.ps1` clones DXVK at that tag, applies the patch and builds it with MSVC.

The water only changes how things are drawn. It does not touch the simulation. It was not checked for effects on
replay or multiplayer determinism (rendering is separate from game logic in this engine, but this was not tested).

### 4. Explosion shockwaves

`Shockwave.h/.cpp` (client only) and `W3DShockwave.h/.cpp`, fed from the `ViewShake` and `TerrainScorch` FX nuggets in
`FXList.cpp`. Explosions that shake the camera or leave a scorch of radius 20+ spawn a ring. The finished back buffer is
copied into a texture and the ring zone is redrawn as a displaced mesh with a small highlight. Fixed-function only.
`W3DView.cpp` calls it after the 3D scene. Disable with `Shockwaves = no`.

### 5. The `-uncensored` flag

`CommandLine.cpp` (flag), `W3DFileSystem.cpp` (model and texture lookup), `ThingTemplate.cpp`/`W3DModelDraw.cpp` only for
pointer fixes. See [above](#the--uncensored-flag).

### 6. Build/packaging additions

`tools/dxvk/` (patch + build script), `tools/install-to-game.ps1`, `scripts/build-x64.ps1`, `scripts/compile-shaders.ps1`,
`THIRD_PARTY_NOTICES.md`, `docs/PORT_NOTES.md`, this README. The upstream README is kept as `README_UPSTREAM.md`.

## Known problems and limitations

- **Multiplayer:** untested. Do not use this build online or in LAN games against other players. 32-bit and 64-bit
  builds are not guaranteed to simulate identically (floating point behaviour differs), and the `-uncensored` flag and
  frame-rate changes were never checked against lock-step play.
- **Only Zero Hour is targeted.** The `Generals/` tree received only the minimum edits needed to keep it compiling in
  principle; it has not been built or run.
- **One test machine.** Other GPUs/drivers may show glitches, especially in the water (it uses a render-to-texture copy of
  the scene, a second scene render for the reflection, and a pixel shader model that DXVK accepts through a patch).
- **Water:** reflections are mostly sky from the standard camera angle. Only one reflection plane height is used, so maps
  with several water bodies at different heights reflect correctly only for the first. Ripples/wakes cover at most six units
  near the view centre. Very large hulls produce wide rings. The look was tuned on the shell map; other maps are untested.
- **Videos:** `-Video` builds play the game's `.bik` files through FFmpeg. If you build without it there are no videos.
  Intro videos can be skipped by moving them out of the way.
- **Fullscreen:** a resolution your display does not support exactly can hang or show a black screen. Use native or
  well-supported resolutions.
- **Remaining warnings:** a number of pointer-size warnings are left (around 190 when last counted), mostly integers stored in `void*` user-data (harmless)
  and GameSpy/menu code that is not used here. `ThreadClass::handle` still truncates the thread handle.
- **Crash reports** on x64 contain only the exception code and address.
- **No updater, no installer, no binaries.** You build it yourself.

## Troubleshooting

These are notes from the author's own experience, not a help desk. No troubleshooting is offered for this project.

| Symptom | Likely cause |
|---|---|
| "DirectX 8 not available" / "Technical difficulties" at start | You started an exe that has no DXVK DLLs next to it. Use the launchers; they start `x64\generalszh64.exe`. |
| Black screen or hang in fullscreen | Unsupported exclusive-fullscreen resolution. Try windowed or a native resolution. |
| Magenta/missing textures | An old build without the DDS header fix, or damaged game archives. |
| No cursor | The game was not started with `-useCwd`. Use the launchers. |
| No videos | Built without `-Video`, or the FFmpeg DLLs are not in the `x64` folder. |
| Stuck at 30 fps | `Options.ini` has no `FPSLimit = no`, or the file was rewritten while you edited it. Close the game, edit, start again. |
| Water looks like the original | `UpdatedWater = no`, or the patched `d3d8.dll`/`d3d9.dll` are not the ones next to the exe. Set `DXVK_LOG_LEVEL=debug` and `DXVK_LOG_PATH=<folder>` and look for `Unsupported PS version`. |
| Language/path empty after reinstalling | The game is registered in a registry location the lookup does not know. Check the `WOW6432Node` entry of the EA/Generals key. |

## Repository layout of the additions

```
README.md                      this file
README_UPSTREAM.md             the upstream README
THIRD_PARTY_NOTICES.md         licenses of what is vendored or downloaded
docs/PORT_NOTES.md             technical notes, ideas, how the pieces fit together
scripts/build-x64.ps1          build the x64 game
scripts/compile-shaders.ps1    recompile the water shaders (needs fxc.exe)
tools/dxvk/                    patch + script for the patched DXVK
tools/install-to-game.ps1      copy the build into your own game folder
Dependencies/D3DX8Compat/      d3dx8 replacement
Dependencies/Miles/MiniMiles.cpp, Dependencies/miniaudio/   sound for x64
Core/GameEngine*/ ... Shockwave*   explosion ring
Core/GameEngineDevice/.../Water/Shaders/   water shaders (HLSL + compiled headers)
```

## Licenses and credits

- The game code is licensed under **GPL v3 with EA's additional terms**, see [LICENSE.md](LICENSE.md). The changes in this
  repository are released under the same license.
- Upstream: the [TheSuperHackers/GeneralsGameCode](https://github.com/TheSuperHackers/GeneralsGameCode) contributors.
  This repository is a derivative; if you want fixes and features for the game itself, contribute there.
- [DXVK](https://github.com/doitsujin/dxvk) (zlib license) - not stored in this repository; downloaded and built by
  `tools/dxvk/build-dxvk.ps1` with the patch from `tools/dxvk`.
- [miniaudio](https://github.com/mackron/miniaudio) (public domain / MIT-0) - vendored unmodified.
- FFmpeg (via vcpkg), glslang (downloaded for the DXVK build) - not stored here.
- See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- No game assets, executables, archives or third-party mods are part of this repository. Models, textures, audio and
  videos belong to their respective owners.
