# Zero Hour x64 + native Vulkan renderer (experimental fork)

An experimental, personal fork of [TheSuperHackers/GeneralsGameCode](https://github.com/TheSuperHackers/GeneralsGameCode)
(the community-maintained source of *Command & Conquer: Generals* and *Zero Hour*, originally released by EA under the
GPL v3). **This version is based on the community fix version**: it is the community's bug-fix code base (the one behind the
"Community Patch" builds) with the changes below on top. All fixes and the retail-compatibility work come from that project,
not from this fork.

This fork builds **Zero Hour** as a **64-bit program** and gives it its own **Vulkan renderer** with updated effects:
water, soft shadows with a random sun angle, ambient occlusion, bloom, colour grading, anti-aliasing, heat haze and
shockwaves. Everything is **on by default**, and one setting (`GraphicsMode = classic`) switches back to the original
rendering.

> **Credit:** all of the actual work in this repository (code, build scripts, shaders, patches, documentation) was done by
> **Claude Code** (Anthropic's AI coding agent). The person who set the repository up only gave prompts and tested the
> results; they did not write or engineer any of it. The underlying game code and its fixes come from the community project
> and EA's original release, see [Licenses and credits](#licenses-and-credits).

> **One-time project: no support, no updates.**
> This was done once, as a personal experiment. It will **not** be troubleshot, answered in issues, or kept up to date with
> upstream or with new tools. Fork it, change it, take pieces of it, do what you like within the license. Treat it as a
> starting point, not as a maintained product.

> **Please read this first.**
> This is a hobby project with one test machine. It works there in the situations listed under [What was tested](#what-was-tested).
> Everything else is unknown. Expect rough edges and crashes. It does **not** contain the game: you need your own legitimate
> copy of *Command & Conquer: Generals - Zero Hour*.

Not affiliated with, or endorsed by, Electronic Arts. *Command & Conquer*, *Generals* and *Zero Hour* are trademarks of their owners.

---

## Contents

1. [What you get](#what-you-get)
2. [The two renderers](#the-two-renderers)
3. [What was tested](#what-was-tested)
4. [Requirements](#requirements)
5. [Building](#building)
6. [Installing into your game folder](#installing-into-your-game-folder)
7. [Running](#running)
8. [All settings and flags](#all-settings-and-flags)
9. [How the enhancements work](#how-the-enhancements-work)
10. [Other changes](#other-changes)
11. [Known problems and limitations](#known-problems-and-limitations)
12. [Troubleshooting](#troubleshooting)
13. [Repository layout of the additions](#repository-layout-of-the-additions)
14. [Licenses and credits](#licenses-and-credits)

---

## What you get

| Area | What it does |
|---|---|
| 64-bit | Zero Hour builds as x64 (`scripts/build-x64.ps1`) with replacements for the 32-bit-only pieces: a DirectX 8 helper library, the Miles sound system and the Bink video player. |
| Native Vulkan renderer | The game's Direct3D 8 calls are answered by an own backend that draws with Vulkan 1.3. It is the default. |
| Updated water | Terrain-depth based refraction, absorption (red disappears first), peaked waves, Fresnel reflection, sun glints, caustics in the shallows, crest glow, shoreline foam and wakes around units. |
| Soft shadows | One shadow map from the sun. Shadows are sharp next to the object that casts them and get softer with distance (contact hardening). The sun angle is random on every launch. The engine's own stencil, decal and tree-blob shadows are not drawn. |
| Ambient occlusion | Screen-space contact darkening in creases and under objects. |
| Bloom and grading | Bright fire, explosions and glints glow; a soft highlight roll-off prevents hard clipping; slight saturation and contrast. |
| Anti-aliasing | FXAA on the 3D scene only. The interface, text and videos are not filtered. |
| Heat haze | The Microwave Tank's and the GPS Scrambler's distortion effect (the only two systems in the game that use it) works. |
| Shockwaves | A screen-space ring on explosions that shake the camera or scorch the ground. |
| Frame rate | Drawing is no longer tied to the 30 Hz game logic. By default frames are drawn as fast as the GPU allows; the simulation stays at 30 Hz. |
| Content filter | A `-uncensored` command line flag that ignores localized (for example German) model/texture overrides when a base version exists. No assets are included. |
| Housekeeping | Many 64-bit pointer-size fixes, registry lookup for 32-bit registered installs, crash-handler stubs for x64, DDS header size fix. |

## The two renderers

| | **Enhanced** (default) | **Classic** |
|---|---|---|
| How | `generalszh64.exe` draws through its own Vulkan backend (`gfx_vulkan.cpp`). | The original Direct3D 8 code runs on a lightly patched [DXVK](https://github.com/doitsujin/dxvk) (Direct3D 8 translated to Vulkan). |
| Look | All enhancements listed above. | The original rendering. Water still has the updated shaders (set `UpdatedWater = no` for the original water). |
| Select | nothing to do | `GraphicsMode = classic` in `Options.ini` |
| Needs | A Vulkan 1.3 driver (`vulkan-1.dll`). If it is missing or too old the game uses Classic automatically. | the DXVK DLLs next to the exe (installed by the install script) |

The native renderer is a software emulation of the Direct3D 8 fixed-function pipeline in shaders, plus the extra passes for
shadows and post processing. It is **not** an engine rewrite. The game logic is untouched. What the native backend does
not implement is listed under [Known problems](#known-problems-and-limitations).

## What was tested

Everything below was done by the author, on one machine. Nothing else was tested.

**Machine:** Windows 11 Pro (build 26200), x64, NVIDIA GeForce RTX 4070 Ti, Visual Studio 2022 (MSVC 14.44), CMake, vcpkg (FFmpeg).

**Game install:** *Zero Hour* 1.04 from the **EA App**, **German** edition.

**Run and looked at**
- Main menu and the animated shell map, windowed (1600x900) and fullscreen (1920x1080), on both renderers
- Skirmish games (several maps: Bitter Winter, Bombardment Beach, Flooded Plains) up to base building, with HUD, minimap, snow, units, buildings, trees, water, shadows
- Heat haze from the Microwave Tank, shockwaves, snow, sound, cursor, videos

**Not tested at all**
- Campaign, Generals Challenge, saved games, replays, the map editor/tools
- LAN, online or any lock-step multiplayer
- The original *Generals* (non Zero Hour) target
- Other GPUs and drivers (AMD, Intel), Windows 10, Linux/Proton, ultrawide or very high DPI setups
- English or other language installs, Steam/disc installs
- Long play sessions (memory use, leaks, stability)

## Requirements

**To build**
- Windows 10/11, x64
- Visual Studio 2022 with the *Desktop development with C++* workload (includes CMake and the Windows SDK)
- Git, and [vcpkg](https://github.com/microsoft/vcpkg) (used for FFmpeg); set `VCPKG_ROOT` to your checkout
- Python 3 with `pip` (only for building the patched DXVK)
- Internet access for the first build (CMake fetches the DirectX 8 and Vulkan headers, vcpkg builds FFmpeg, the DXVK script downloads DXVK and glslang)

**To run**
- A legitimate Zero Hour installation (1.04)
- A GPU and driver with Vulkan 1.3 (dynamic rendering, synchronization2, push descriptors). Without it the game runs Classic.

## Building

All commands in PowerShell, from the repository root.

```powershell
# 1. get the sources
git clone <this repository> GeneralsZH-x64
cd GeneralsZH-x64

# 2. vcpkg (FFmpeg is built through it). Skip the first two lines if you already have a checkout.
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
$env:VCPKG_ROOT = "C:\vcpkg"      # set it in every new PowerShell window before step 3

# 3. build the game (the first run takes a while: FFmpeg is compiled by vcpkg)
powershell -File scripts\build-x64.ps1 -Video
#   -Video : with FFmpeg so the game's .bik videos play. Without it videos are skipped.
#   -Clean : rebuild everything
# Output: build\win64-vcpkg\GeneralsMD\Release\generalszh.exe (+ FFmpeg DLLs next to it)

# 4. build the patched DXVK (only needed for the Classic renderer; reuses the headers CMake fetched in step 3)
powershell -File tools\dxvk\build-dxvk.ps1
# Output: tools\dxvk\work\out\d3d8.dll d3d9.dll dxgi.dll
```

Notes
- `scripts/build-x64.ps1` creates a tiny `afxres.h` shim in `build/shim` because the resource file expects MFC's header.
- The compiled shaders are committed (`gfx_vk_shaders.h` for the native renderer, `Water/Shaders/*PS.h` for the classic water), so
  no shader compiler is needed to build. After editing the GLSL in `Core/Libraries/Source/WWVegas/WW3D2/Shaders/` run
  `scripts\compile-vk-shaders.ps1` (needs `glslang`, which `tools\dxvk\build-dxvk.ps1` downloads); after editing the classic
  water HLSL run `scripts\compile-shaders.ps1` (needs `fxc.exe` from the Windows SDK).
- `tools/dxvk/0001-*.patch` is the whole DXVK change (a few lines in `src/d3d9/d3d9_device.cpp`).

## Installing into your game folder

```powershell
powershell -File tools\install-to-game.ps1 -GameDir "D:\Games\Command and Conquer Generals Zero Hour"
```

This only **adds** things:

```
<GameDir>\x64\generalszh64.exe            the game you built
<GameDir>\x64\avcodec-*.dll ... z.dll      FFmpeg and zlib, from the build output
<GameDir>\x64\d3d8.dll d3d9.dll dxgi.dll   the patched DXVK (used by the Classic renderer)
<GameDir>\Play-x64.bat                    windowed launcher
<GameDir>\Play-x64-Fullscreen.bat         fullscreen launcher
```

Nothing of the original game is modified. To uninstall, delete the `x64` folder and the two `.bat` files. Start the game
through the `.bat` files (or with the same arguments): they set the working directory and `-useCwd`, which the executable
needs to find loose `Data\Cursors` and `Data\Movies`. Do not copy `generalszh64.exe` over the original `generalszh.exe`.

## Running

`Play-x64.bat` starts windowed, `Play-x64-Fullscreen.bat` fullscreen. Extra arguments are passed through, for example
`Play-x64.bat -xres 1920 -yres 1080`. The enhanced renderer is used unless you choose otherwise (see below). A log of the
native renderer is written to `gfx_vulkan.log` in the game folder.

## All settings and flags

### Options.ini (`%USERPROFILE%\Documents\Command and Conquer Generals Zero Hour Data\Options.ini`)

The game rewrites this file when it exits, so edit it while the game is closed. Keys that are missing use the default.

**Renderer and enhancements**

| Key | Default | Meaning |
|---|---|---|
| `GraphicsMode` | `enhanced` | `classic` = original rendering through Direct3D 8 on DXVK, none of the native effects below. Any other value, or no key: native Vulkan (falls back to classic if the driver is too old). |
| `PostProcessing` | `yes` | `no` = draw the 3D scene straight to the screen: no bloom, grading, AO, FXAA and no shadows. |
| `SoftShadows` | `yes` | `no` = no sun shadows at all (the engine's own shadows are not used on the native renderer either). |
| `AmbientOcclusion` | `yes` | `no` = no contact darkening. |
| `Bloom` | `yes` | `no` = no glow. |
| `AntiAliasingFXAA` | `yes` | `no` = no edge smoothing. |
| `SunAzimuth` | random | Compass direction of the sun in degrees (0 to 360). With no value the sun is random on every launch. |
| `SunElevation` | random | Sun height in degrees (about 10 low and long shadows, 80 almost overhead). Only used together with `SunAzimuth`; without it the elevation is random between 35 and 62. |

The same effect switches are in the game's options menu: **Options > Detail: Custom** opens the advanced pane with *Soft shadows (sun)*,
*Ambient occlusion*, *Bloom (glow)*, *Anti-aliasing*, *New water (restart)* and *Shockwaves*, grouped under "Graphics enhancements". They apply immediately and are written to `Options.ini`. The new group comes
from a patched copy of `OptionsMenu.wnd` that `tools\install-to-game.ps1` writes as a loose file into `<GameDir>\Window\Menus\` (generated from your own
`WindowZH.big`; needs Python). Changing them in the middle of a game is limited by the game itself, so use the main menu.

**Effects shared by both renderers**

| Key | Default | Meaning |
|---|---|---|
| `UpdatedWater` | `yes` | `no` = original water. Also used automatically if the updated shaders cannot be created. |
| `WaterReflections` | `yes` | `no` = skip the extra mirrored render the water reflection needs. |
| `Shockwaves` | `yes` | `no` = no shockwave ring on explosions. |
| `HeatEffects` | game setting | `no` = no heat distortion (also in the game's options menu). |
| `FPSLimit` | `no` (unlimited) | `yes` = the original 30 fps cap. The logic always runs at 30 Hz. |
| `Resolution` | game default | Use a resolution your display really supports at fullscreen. |

### Environment variables (override Options.ini; mostly for development)

| Variable | Effect |
|---|---|
| `GENERALS_GFX` | `vulkan` forces the native renderer, `null` runs without any output (test mode); any other non-empty value forces Classic. |
| `GENERALS_POST` | `0` disables the post processing chain (and so shadows), `1` enables it. |
| `GENERALS_SHADOWS` | `0` disables shadows. |
| `GENERALS_SUN` | `azimuth,elevation` in degrees, for example `135,40`. |
| `GENERALS_BLOOM` | bloom strength (default 0.16, `0` = off). |
| `GENERALS_AO` | ambient occlusion strength (default 0.85, `0` = off). |
| `GENERALS_FXAA` | `0` disables FXAA. |

### Command line flags

| Flag | Meaning |
|---|---|
| `-win` | windowed mode |
| `-xres N -yres N` | resolution |
| `-useCwd` | keep the current folder as working directory (*needed by this port*, set by the launchers) |
| `-mod <folder or .big>` | load an extra archive/folder that overrides the game's data (upstream) |
| `-uncensored` | *new*: ignore localized model/texture overrides when a base version exists, see [Other changes](#other-changes) |
| `-quickstart`, `-noshellmap`, `-map <file>` | upstream shortcuts, handy for testing (`-quickstart` disables the menu background animation) |

## How the enhancements work

### Native Vulkan renderer

All Direct3D 8 access of the engine goes through one wrapper (`DX8Wrapper`), and the game was changed to use neutral
names (`GFX_*`) so the backend can be swapped. `gfx_null.cpp` is an in-memory backend that keeps all state and resources
(it also runs the game with no GPU, for tests); `gfx_vulkan.cpp` derives from it and draws:

- Vulkan 1.3 with dynamic rendering, push descriptors, a per-frame ring buffer for vertices/indices/uniforms and a pipeline cache
- textures uploaded on demand with content hashing, 24-bit and DXT formats handled
- two shaders (`Shaders/gfx_fixed.vert/.frag`) that reproduce the fixed-function pipeline: 4-stage texture combiners, alpha test,
  vertex lighting, texture-coordinate generation and transforms, pre-transformed 2D vertices, point sprites with distance scaling
- stencil, render-to-texture, back buffer read-back (heat haze, shockwave)
- a half-pixel offset so Direct3D pixel centres line up with Vulkan

### Scene, interface and post processing

The engine tells the backend where the 3D scene starts and ends (`W3DDisplay.cpp`). The scene is drawn into a 16-bit
floating point image, goes through the post chain and lands in the swap chain; the interface, text and videos are then
drawn on top, sharp and unfiltered.

- **Bloom:** bright pass, 5-level down/up sample chain, added back with a soft threshold
- **Highlight roll-off and grading:** values above 0.9 compress smoothly instead of clipping; a touch of saturation and contrast
- **Ambient occlusion:** 20 samples per pixel around the scene depth, depth-aware blur in two passes
- **FXAA:** the classic luma-based edge filter

### Soft shadows

Opaque geometry drawn in the scene is remembered and replayed from the sun into a 4096x4096 depth map that is fitted to the
visible area and snapped to its texel grid so shadows do not shimmer when the camera moves. A full-screen pass rebuilds the
world position of every pixel from the scene depth and the real 3D viewport, searches the map for blockers, estimates the
distance to the nearest blocker and filters over a width that grows with it (percentage-closer soft shadows). That is why a
shadow is crisp at the foot of a tree or unit and fades out along its length. The engine's stencil volumes, projected decals
and the round blob under trees are not drawn on this renderer. The sun angle is chosen randomly at start-up (see
`SunAzimuth` / `SunElevation`).

### Updated water

Files: `Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/` (`W3DWater.cpp`, `Shaders/WaterModern.hlsl`, compiled headers),
`Core/Libraries/Source/WWVegas/WW3D2/Shaders/gfx_water.frag` (native version), `OptionPreferences.*`.

| Feature | How it works |
|---|---|
| Depth | The terrain height of the whole map is stored in a texture and sampled in world space; water depth is known per pixel. |
| Refraction and absorption | The scene is copied once per frame and sampled through a wave-distorted lookup. Per-channel absorption by depth (red first) turns shallow turquoise into deep navy. |
| Waves | Six peaked waves plus noise with a warped phase; they drive shading, glints and the reflection term. |
| Reflection | The game's mirror pass is reused, with a sky gradient as fallback. From the usual camera angle most of the reflection is sky. |
| Caustics, crest glow | Moving light patterns on the seabed in shallow water; sunlight shining through thin wave crests. |
| Foam | A swash band that runs up the beach, foam clouds on the crests, and wakes. |
| Units on water | The six ships, hovercraft or wading units nearest to the view centre disturb the surface: ripples around idle hulls, a V-shaped wake behind moving ones. |
| Fallback | If the shaders cannot be created the original water shaders are used, with a depth tint baked into the vertex colours. |

The classic renderer runs the HLSL version through a patched DXVK; its Direct3D 8 front end rejects pixel shaders above 1.x
and the patch in `tools/dxvk/` removes that one clamp. The native renderer recognises the engine's two water shaders and runs
the GLSL port instead.

### Heat haze and shockwaves

Heat haze is the game's own screen-space distortion; the native renderer provides the back-buffer read-back it needs.
Shockwaves (`Shockwave.cpp`, `W3DShockwave.cpp`, fed from the `ViewShake` and `TerrainScorch` FX nuggets) copy the finished
frame into a texture and redraw the ring zone as a displaced mesh. Disable with `Shockwaves = no`.

## Other changes

### 64-bit build

| Change | Where | Why |
|---|---|---|
| x64 CMake presets `win64` and `win64-vcpkg` | `CMakePresets.json`, `CMakeLists.txt`, `cmake/dx8.cmake` | Upstream only has 32-bit presets. |
| `Dependencies/D3DX8Compat` | new | `d3dx8.lib` exists only for x86; this re-implements what the game uses (matrix math, DXT conversion, mip generation, texture creation). |
| `Dependencies/Miles/MiniMiles.cpp`, `Dependencies/miniaudio` | new | `mss32.dll` is 32-bit only. `MiniMiles` implements the ~70 Miles calls on top of [miniaudio](https://github.com/mackron/miniaudio). |
| Video through FFmpeg | `-DRTS_BUILD_OPTION_FFMPEG=ON`, vcpkg manifest | The Bink SDK is 32-bit only; the engine's existing FFmpeg player is used. |
| DDS header size | `ddsfile.h` | A `void*` member made the legacy header 136 bytes instead of 124 on x64, so every DDS texture failed to load. |
| Registry lookups | `registry.cpp`/`.h` | Installs registered by the 32-bit EA App live under `WOW6432Node`. |
| Pointer-size cleanups | ~40 small edits | `(Int)pointer` casts replaced with `intptr_t`/`uintptr_t`. |
| Shutdown crashes | `dx8wrapper.cpp`, `WinMain.cpp` | The graphics library was unloaded before textures were released, and the C runtime ran static destructors of the memory pools after the memory manager was gone. |
| Crash/stack code | `Except.cpp`, `debug_stack.cpp`, `StackDump.*` | The register/stack dump is x86-only; on x64 only the exception code and address are recorded. |

### Frame pacing

`FramePacer` keeps the logic at 30 Hz when the render cap is off. `GameLogicDispatch.cpp` and `GameLOD.cpp` no longer switch the
30 fps limit back on when a game starts or a detail preset is applied; `FPSLimit = yes` still restores it.

### The `-uncensored` flag

Some regional editions replace models and textures through localized archives (for example `Data\<Language>\Art\W3D`). With
`-uncensored` the game prefers the model from the base archives or from a mod when one exists, and ignores localized textures
whose name contains `drone`. **No assets are included or downloaded.** What it finds depends on your own installation and any
mod loaded with `-mod`. Only examined with a German install.

## Known problems and limitations

- **Multiplayer:** untested. Do not use this build online or in LAN games against other players. Graphics changes are meant
  not to touch the simulation, but this was never checked for lock-step play.
- **Only Zero Hour is targeted.** The `Generals/` tree received only the minimum edits to keep it compiling in principle.
- **One test machine.** Other GPUs/drivers may show glitches, especially in the native renderer.
- **Native renderer gaps:** no programmable shaders other than the two water shaders, no fixed-function fog (the game keeps fog disabled everywhere), no cube/volume textures, no
  multisampling, no gamma ramp. Menus, HUD and the 3D game are covered; anything that needs those paths is skipped or looks
  different. If something looks wrong set `GraphicsMode = classic`.
- **Post chain:** the ambient occlusion and shadow strengths are tuned by eye on a few maps. The shadow map covers the
  visible area only, so very distant objects and very large views may show edge artifacts.
- **Shadows:** only opaque geometry casts them; alpha-blended objects cast none. The sun is fixed for a session (the map's
  time-of-day light changes are ignored).
- **Water:** reflections are mostly sky; only one reflection plane height is used; ripples and wakes cover at most six units
  near the view centre. Tuned on the shell map and a few skirmish maps.
- **Videos:** `-Video` builds play `.bik` files through FFmpeg. Without it there are no videos. Intro videos can be skipped by moving them away.
- **Fullscreen:** a resolution your display does not support exactly can hang or show a black screen.
- **Remaining warnings:** pointer-size warnings remain in places (integers stored in `void*` user-data, GameSpy/menu code).
- **Crash reports** on x64 contain only the exception code and address.
- **No updater, no installer, no binaries.** You build it yourself.

## Troubleshooting

Notes from the author's own experience, not a help desk.

| Symptom | Likely cause |
|---|---|
| "DirectX 8 not available" at start | You started an exe without the DXVK DLLs next to it, in Classic mode. Use the launchers. |
| Black screen or hang in fullscreen | Unsupported exclusive-fullscreen resolution. Try windowed or a native resolution. |
| Something looks wrong in the enhanced renderer | Try `GraphicsMode = classic`, or turn single effects off (`SoftShadows`, `AmbientOcclusion`, `Bloom`, `AntiAliasingFXAA`, `PostProcessing`). Look into `gfx_vulkan.log`. |
| It runs Classic although you did not ask | The Vulkan driver is older than 1.3 or `vulkan-1.dll` is missing. |
| Magenta/missing textures | An old build without the DDS header fix, or damaged game archives. |
| No cursor | The game was not started with `-useCwd`. Use the launchers. |
| No videos | Built without `-Video`, or the FFmpeg DLLs are not in the `x64` folder. |
| Stuck at 30 fps | `Options.ini` contains `FPSLimit = yes`. |
| Language/path empty after reinstalling | The game is registered in a registry location the lookup does not know. Check the `WOW6432Node` entry. |

## Repository layout of the additions

```
README.md                      this file
README_UPSTREAM.md             the upstream README
THIRD_PARTY_NOTICES.md         licenses of what is vendored or downloaded
docs/PORT_NOTES.md             technical notes
docs/RENDER_CALLSITES.md       every place the engine talks to the graphics API
scripts/build-x64.ps1          build the x64 game
scripts/compile-vk-shaders.ps1 recompile the native renderer's GLSL shaders (needs glslang)
scripts/compile-shaders.ps1    recompile the classic water shaders (needs fxc.exe)
scripts/gen_null_stubs.py, scripts/callsites.py   helpers that generated parts of the backend interface
tools/bigtool.py               list/extract EA .big archives
tools/patch_options_wnd.py     adds the graphics check boxes to the options menu (run by the install script)
tools/dxvk/                    patch + script for the patched DXVK (Classic renderer)
tools/install-to-game.ps1      copy the build into your own game folder
Dependencies/D3DX8Compat/      d3dx8 replacement
Dependencies/Miles/MiniMiles.cpp, Dependencies/miniaudio/   sound for x64
Core/Libraries/Source/WWVegas/WW3D2/gfx_*.cpp/.h/.inl   neutral graphics types, null backend, native Vulkan backend, post chain, shadows
Core/Libraries/Source/WWVegas/WW3D2/Shaders/            GLSL for the native renderer
Core/GameEngineDevice/.../Water/Shaders/                water shaders for the classic renderer (HLSL + compiled headers)
Core/GameEngine*/ ... Shockwave*                        explosion ring
```

## Licenses and credits

- **Who did the work here:** Claude Code (Anthropic) wrote the changes, scripts, shaders, patches and documentation in this
  repository; the repository owner only supplied prompts and testing feedback. Mistakes in it are this project's own, not the
  upstream authors'.
- The game code is licensed under **GPL v3 with EA's additional terms**, see [LICENSE.md](LICENSE.md). The changes in this
  repository are released under the same license.
- Upstream: the [TheSuperHackers/GeneralsGameCode](https://github.com/TheSuperHackers/GeneralsGameCode) contributors. If you
  want fixes and features for the game itself, contribute there.
- [DXVK](https://github.com/doitsujin/dxvk) (zlib license): not stored here; downloaded and built by `tools/dxvk/build-dxvk.ps1`
  with the patch from `tools/dxvk`.
- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (Apache-2.0 / MIT): fetched by CMake at build time.
- [miniaudio](https://github.com/mackron/miniaudio) (public domain / MIT-0): vendored unmodified.
- FFmpeg (via vcpkg), glslang (downloaded by the DXVK script): not stored here.
- See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- No game assets, executables, archives or third-party mods are part of this repository.
