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

## Renderer boundary (preparation for a different graphics backend)

Goal: everything that talks to Direct3D 8 goes through `DX8Wrapper`, so a new backend only has to replace that one class.
`docs/RENDER_CALLSITES.md` (generated by `scripts/callsites.py`) lists every direct call; at the start it counted 485 direct device
calls in 15 files outside the wrapper (water 172, shader manager 110, two shadow files 143, eleven small files 60).

Done: all of them now go through the `DX8Wrapper::Raw_*` group in `dx8wrapper.h` (33 functions: render/texture-stage state, texture
binds, pixel and vertex shaders and constants, buffers, draw calls including the user-pointer variants, render targets, surface
copies, cursor and device state). These are deliberately uncached, behaviour-identical forwards, so this step changed no behaviour.
The scan now reports 0 bypass calls. A new backend implements the `Raw_*` group plus the cached `Set_DX8_*` helpers.

Not done yet (next steps): the `Raw_*` functions still take Direct3D 8 types and enums (`D3DRS_*`, `D3DTSS_*`, FVF codes and
`IDirect3D*8` handles). Replacing those with backend-neutral types is the next layer of work; so is folding the uncached `Raw_*`
state calls into the wrapper's cached tracking once the stale-state risks of the direct callers are understood.

### Neutral names (step 1, done)

Game code no longer names Direct3D 8 constants or object types. `Core/Libraries/Source/WWVegas/WW3D2/gfx_d3d8_map.h` maps
`GFX_RS_*`, `GFX_TSS_*`, `GFX_TOP_*`, `GFX_TA_*`, `GFX_BLEND_*`, `GFX_CMP_*`, `GFX_FMT_*`, `GFX_FVF_*`, `GFX_POOL_*`, `GFX_USAGE_*`, `GFX_LOCK_*`
and others (303 constants) to the Direct3D 8 values, and defines `GfxDevice`, `GfxTexture`, `GfxSurface`, `GfxVertexBuffer`,
`GfxIndexBuffer`, `GfxBaseTexture`, `GfxViewport`, `GfxLight`, `GfxMaterial`. The values are identical, so nothing changed at run time.
Not converted yet: caps flags (`D3DPTEXTURECAPS_*` ...), error codes, vertex-shader declaration tokens (`D3DVSD_*`), and the matrix/vector
types from D3DX. The object types are still the COM interfaces, so a backend implements the same member functions.

### Null backend (step 2, done)

`gfx_null.cpp` implements the Direct3D 8 object, device, surfaces, textures and buffers in system memory and draws nothing. Start the
game with the environment variable `GENERALS_GFX=null` and `DX8Wrapper::Init` uses it instead of loading `D3D8.DLL`.
`gfx_null_stubs.inl` is generated from `d3d8.h` by `scripts/gen_null_stubs.py` (every method of nine interfaces with a harmless default);
`gfx_null.cpp` overrides what has to do real work (caps, adapter modes, memory-backed resources, state read-back).

Result: the game starts, loads the shell map, accepts menu clicks, starts a skirmish and runs with no `d3d8.dll`, `d3d9.dll` or
`vulkan-1.dll` in the process. So the interfaces behind `DX8Wrapper` are the whole graphics boundary. The null device also logs its frame
rate to `gfx_null_fps.txt` (one line per 5 s). Measured on the test machine, with nothing drawn: shell scene about 610 fps, skirmish about
1,350 fps. With DXVK on the GPU the same scenes ran at about 400-800 and 1,100-1,290 fps. The game's own CPU work (simulation, scene
traversal, draw call generation) is therefore the limit, and a different graphics backend can recover at most the remaining 10-20 %.

## Open points / ideas

- A native renderer behind `DX8Wrapper` would remove the need for a patched translation layer.
- Remaining pointer-truncation warnings (mostly ints stored in `void*` user data); `ThreadClass::handle` should be `uintptr_t`.
- Lock-step multiplayer between 32-bit and 64-bit builds is untested (x87 vs SSE float behaviour differs).
- Water reflections use one plane height; per-body heights would need one mirror pass per body.
- Make effect parameters data driven (INI) so they can be tuned without rebuilding.

## Debugging tips

- DXVK log: set `DXVK_LOG_LEVEL=debug` and `DXVK_LOG_PATH=<folder>` before starting; look for `err:` lines.
- The counter in the top-left corner of the game window shows the current render frame rate.

## Native Vulkan backend (experimental)

Select with the environment variable `GENERALS_GFX=vulkan` (`null` selects the no-output test backend; unset keeps Direct3D 8 via DXVK).
Implemented in `Core/Libraries/Source/WWVegas/WW3D2/gfx_vulkan.cpp` as a class derived from the in-memory `NullDevice`; it needs Vulkan 1.3 (dynamic rendering, synchronization2, push descriptors).

What it does: swapchain, asynchronous texture upload with content hashing, per-frame ring buffer for vertices/indices/uniforms, pipeline cache keyed by FVF and render state,
and two shaders (`Shaders/gfx_fixed.vert/.frag`, compiled with `scripts/compile-vk-shaders.ps1`) that reproduce the Direct3D 8 fixed-function pipeline:
four-stage texture combiners, alpha test, vertex lighting, texture coordinate generation and transforms, pre-transformed vertices, stencil (shadow volumes).

Not implemented yet: programmable shaders (caps advertise none, so the engine uses fixed-function paths; the updated water needs them), fog. Off-screen render targets (render-to-texture, used by heat haze, shockwave and shadow decals) and back buffer read back are implemented but not yet verified in a scene that uses them.
and back-buffer readback (shockwave and heat haze effects are skipped).
The game is CPU-bound, so this backend is not faster than DXVK today; its purpose is to be the base for new rendering features.
