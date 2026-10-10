# ModDB page text

(Plain text with light formatting, ready to paste into the ModDB description box. Screenshots: `docs/screenshots` after running `tools\compare-screenshots.ps1`.)

---

## Zero Hour x64 + Vulkan renderer (experimental)

This project replaces the DirectX 8 rendering of *Command & Conquer: Generals - Zero Hour* with a native Vulkan renderer and adds a set of optional graphics enhancements on top. It also builds the game as a 64-bit program. Gameplay, balance, units and maps are not touched: it is a graphics and engine project, not a content mod.

It is based on the community fix version of the game's source code (the community project behind the "Community Patch" builds). All bug fixes and the retail compatibility come from that project; this fork only adds the renderer, the effects and some tooling.

**IMPORTANT: one-time project, no active development.**
I do not plan to work on this actively. There is no support, no bug fixing on request and no updates. Feel free to fork it and work on it yourself, take parts of it, or ignore it. The source is on GitHub (GPL v3, like the original code release): https://github.com/JaChriCaeTob/ModernGeneralsZH

**Credit:** this project was vibecoded. All of the code, shaders, scripts and documentation were written by Claude Code (Anthropic's AI coding agent). I only had the idea, gave the prompts and had the patience to test it. The game code underneath, and its fixes, belong to EA's original release and the community project.

### Features

Everything below is optional and can be switched on or off in the in-game options (Graphics enhancements group in the advanced display options) or in `Options.ini`. Everything is on by default except visible clouds.

- **Native Vulkan renderer** instead of DirectX 8 (needs a GPU and driver with Vulkan 1.3). If Vulkan 1.3 is not available the game falls back to the original rendering.
- **64-bit executable.**
- **Unlocked frame rate**, decoupled from the game logic (no game speed-up). A frame limit can be switched back on.
- **Dynamic soft shadows** that replace the old hard stencil shadows: the shadow gets softer the further it is from the object, and the sun direction is random on every launch (or fixed through a setting).
- **Dynamic cloud shadows** drifting over the map (replace the game's own cloud texture). Optional visible clouds exist when you zoom out very far, but they are off by default because they cost performance.
- **Completely reworked water shaders** for rivers and the sea: depth based colour and absorption, refraction, caustics, waves and ripples around ships.
- **New shockwave effects** and heat haze (for example behind the Microwave Tank).
- **Ambient occlusion, bloom, colour grading and edge anti-aliasing (FXAA).**
- **Soft particles:** smoke and fire fade where they meet the ground instead of cutting off.
- **Dynamic lights:** explosions and other light pulses of the game light the ground and units around them, wash out the sun's shadows nearby, and the strongest light casts rough shadows of its own.
- **Per-pixel lighting** of units and buildings with a soft sun highlight and a faint rim light.
- **Anisotropic texture filtering** (16x): terrain and roads stay sharp at shallow angles.
- A raised **camera zoom limit** (default 450 instead of 310, adjustable with `MaxCameraHeight`).
- **Two launchers:** `Play-Updated.bat` (everything above) and `Play-Original.bat` (the original rendering through Direct3D 8 on DXVK, none of the added effects).
- Developer extras: a timed screenshot comparison tool for the intro scene (original against updated), and Blender tooling to export and re-import game models.

### Installation

There is **no installer and no prebuilt download in the repository**: you build it yourself, or use a build somebody attached to this page. You need your own, legitimate copy of Zero Hour 1.04. Nothing of the original game is modified or redistributed.

If you have a build (a folder with `x64`, `Play-Updated.bat`, `Play-Original.bat` and `Window`):
1. Copy the contents into your Zero Hour game folder (the one with the `.big` files such as `INIZH.big`).
2. Start the game with `Play-Updated.bat` (or `Play-Original.bat` to compare). Do not start `x64\generalszh64.exe` directly and do not copy it over an original game file: the `.bat` files set the working directory the game needs.
3. Both launchers start windowed. Remove `-win` inside the `.bat` for fullscreen. Set your resolution in the in-game options or in `Options.ini`.
4. To uninstall, delete the `x64` folder, the two `.bat` files and the loose `Window` folder.

If you want to build it yourself (Windows 10/11, x64):
1. Install Visual Studio 2022 with the *Desktop development with C++* workload, Git, vcpkg (set `VCPKG_ROOT`) and Python 3.
2. Clone the repository and run `scripts\build-x64.ps1 -Video` (the first build takes a while because FFmpeg is built through vcpkg).
3. Run `tools\install-to-game.ps1 -GameDir "<your Zero Hour folder>"`.
4. Start the game with `Play-Updated.bat`.

The full instructions and every setting, flag and environment variable are in the README of the repository. A log of the renderer is written to `gfx_vulkan.log` in the game folder.

### What was tested

Tested on one machine only: Windows 11 Pro, NVIDIA GeForce RTX 4070 Ti, Zero Hour 1.04 from the EA App, German edition.

- Main menu and the animated intro (shell map), windowed and fullscreen, with both renderers.
- Skirmish games on several maps (Bitter Winter, Bombardment Beach, Flooded Plains) up to base building, with HUD, minimap, snow, units, buildings, trees, water and shadows.
- Heat haze, shockwaves, snow, sound, cursor and videos.
- The options menu and the new graphics settings, switched on and off.

### Not tested

- Campaign, Generals Challenge, saved games and replays.
- LAN and online multiplayer. **Do not use this build in multiplayer.** The graphics changes are meant not to touch the game simulation, but this was never checked for lock-step play, and the 64-bit build is not guaranteed to stay in sync with 32-bit players.
- Other GPUs and drivers (AMD, Intel), Windows 10, Linux/Proton, ultrawide screens and high-DPI setups.
- English and other language installs, Steam and disc versions.
- Long play sessions (memory use and stability).

### Known bugs

- Some audio clips play back weirdly.
- The dynamic shadows are drawn above some other effects.
- Light shadows of explosions are rough and only cover objects near the light; small lights over water have no visible effect.
- The visible clouds and an extreme zoom-out can cost a lot of performance, and a black screen was seen at extreme zoom while testing, so both are off or limited by default.
- Shadows only come from opaque objects; very distant objects can show edge artifacts.
- Water reflections are mostly sky; the water was tuned on a few maps only.
- A fullscreen resolution that your display does not support exactly can hang or show a black screen.
- Only Zero Hour is targeted, not the original Generals.
- Other GPUs and drivers may show glitches. If something looks wrong, set `GraphicsMode = classic` in `Options.ini` or start `Play-Original.bat`.

### Links

Source code, README and all settings: https://github.com/JaChriCaeTob/ModernGeneralsZH

Not affiliated with, or endorsed by, Electronic Arts. *Command & Conquer*, *Generals* and *Zero Hour* are trademarks of their owners.
