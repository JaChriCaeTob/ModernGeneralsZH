# Port notes

Technical notes behind the changes described in the [README](../README.md). Written for someone who wants to continue the
work or understand why something is the way it is. Everything here describes the state of one test machine (see the
README for what was and was not tested).

## How the pieces fit

```
generalszh64.exe  (x64, built here)
   |-- Direct3D 8 calls (DX8Wrapper) --> d3d8.dll (DXVK, patched) --> d3d9 front end --> Vulkan
   |-- audio: Miles API --> Dependencies/Miles/MiniMiles.cpp --> miniaudio
   |-- video: engine FFmpegVideoPlayer --> FFmpeg DLLs (vcpkg)
   `-- d3dx8 helper functions --> Dependencies/D3DX8Compat
```

All Direct3D goes through `DX8Wrapper` (`Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.*`). That is the single place to
replace if a native Vulkan/D3D12/other renderer is ever wanted; nothing in this repository started that.

## Variants that were built

| Variant | Build | Renderer | State |
|---|---|---|---|
| 64-bit + patched DXVK | `scripts/build-x64.ps1 -Video` (preset `win64-vcpkg`) | D3D8 -> Vulkan, DXVK 3.1.1 + patch | the one that was tested |
| 64-bit without video | `scripts/build-x64.ps1` (preset `win64`) | same | builds, not run recently |
| 32-bit + stock DXVK | preset `win32` (+ `-DRTS_D3DX8_COMPAT=ON`) | D3D8 -> Vulkan | ran earlier, not re-verified after the latest changes |

## 64-bit specifics

- `Dependencies/D3DX8Compat`: replacement for the `d3dx8` functions the game uses. `cmake/dx8.cmake` generates an x64
  `d3d8.lib` import library; at run time `d3d8.dll` is DXVK's.
- `MiniMiles.cpp` implements the Miles calls used by `MilesAudioManager` (about 70) on miniaudio. Handles passed to
  callbacks are truncated 32-bit ids on purpose (the manager stores them in 32-bit fields); `MilesAudioManager.cpp` got a small
  `handleId` helper for that.
- The legacy DDS surface description (`ddsfile.h`) must be 124 bytes. A `void*` member made it 136 on x64, which made every DDS
  texture fail.
- Registry: EA App registers under `WOW6432Node`; the lookups use `KEY_WOW64_32KEY`.
- The exe changes its working directory to its own folder unless `-useCwd` is passed; the launchers pass it so loose
  `Data\Cursors` and `Data\Movies` are found.
- `RTS.RC` wants MFC's `afxres.h`; the build script writes a forwarding shim to `build/shim` and passes it via `CMAKE_RC_FLAGS`.

## Frame pacing

`FramePacer` has separate logic and render rates. With the render cap off the logic keeps its native rate. The cap flag
`GlobalData::m_useFpsLimit` is global and was re-enabled by two code paths (game start message, detail-level application);
both now respect an explicit `FPSLimit = no`. Note that `-noFPSLimit` on the command line is overridden by `Options.ini`.

## Water in detail

Shaders: `Water/Shaders/WaterModern.hlsl`, compiled by `scripts/compile-shaders.ps1` (fxc, `ps_3_0`) into
`WaterModernRiverPS.h` / `WaterModernTrapezoidPS.h`. `ps_2_b` was tried first and ran out of the 32 constant registers.

Texture stages used by the shader: 0 water texture, 1 sparkle, 2 noise, 3 river edge (river) or shroud (area water),
4 copy of the back buffer, 5 terrain height, 6 mirrored scene. Stages 4-6 get their coordinates from texture-coordinate
generation (camera-space position through a texture matrix), so no vertex shader is needed.

Constants (`setupUpdatedWaterStages`): c1 effect strengths, c2 base alpha / glint / depth scale, c3 water height and time
and world size of the height texture, c4 height decode, c5 viewport scale for the back buffer copy, c6 mirror available,
c8-c10 world axes in camera space, c11 sun direction, c12-c23 up to six units on the water.

Things that were learnt the hard way:
- DXVK's Direct3D 8 front end checks the shader model in `D3D9DeviceEx::CreateShaderModule`; for D3D8 devices it clamps pixel
  shaders to 1.x. That one expression is what `tools/dxvk/0001-*.patch` changes.
- The area water ("sea" in the shell map) is drawn by `drawTrapezoidWater` with its own shroud-aware shader, not the river
  shader. The deforming grid mesh (`renderWaterMesh`) intentionally keeps the original shader.
- The vertex alpha of the original water does not control its opacity in the river shader (the texture alpha does). The
  updated shaders therefore compute opacity from depth.
- The mirror image of a camera reflected at the water plane lines up with the real camera's screen coordinates, so the
  reflection is sampled at the pixel's own screen position.
- Unit positions for the wake come from the *drawable* (interpolated every rendered frame), not the logic object (moves 30
  times a second); otherwise the effect jitters at high frame rates.
- `WaterType 2` (the original shader/reflection sea with a global plane, `drawSea`) is unused by the game data (GameData sets
  `WaterType 0`). It was tried as a base and dropped; the original code is untouched.

Tuning constants live in `setupUpdatedWaterStages` (C++) and at the top of the shader.

## Explosion shockwaves

`ShockwaveList` (client only) is fed by the `ViewShake` and `TerrainScorch` FX nuggets in `FXList.cpp`. `W3DShockwave` copies
the back buffer into a texture after the 3D scene and redraws the ring zone as an elliptical polar mesh with radially displaced
UVs. A render-target based approach did not work because render-target textures are disabled for the default filter.

## Content filter flag

Regional installs can override models and textures through localized archives. `-uncensored` (`CommandLine.cpp`,
`W3DFileSystem.cpp`) makes the lookup prefer the base/mod file when it exists and skips localized textures named `*drone*`.
It has no effect without such files, and it was only examined with a German install.

## Open points / ideas

- A native renderer behind `DX8Wrapper` would remove the need for a patched translation layer.
- Remaining pointer-truncation warnings (mostly ints stored in `void*` user data); `ThreadClass::handle` should be `uintptr_t`.
- Lock-step multiplayer between 32-bit and 64-bit builds is untested (x87 vs SSE float behaviour differs).
- Water reflections use one plane height; per-body heights would need one mirror pass per body.
- Make effect parameters data driven (INI) so they can be tuned without rebuilding.

## Debugging tips

- DXVK log: set `DXVK_LOG_LEVEL=debug` and `DXVK_LOG_PATH=<folder>` before starting; look for `err:` lines.
- The counter in the top-left corner of the game window shows the current render frame rate.
