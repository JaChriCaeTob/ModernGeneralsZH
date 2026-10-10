# Third-party notices

This repository is a derivative of [TheSuperHackers/GeneralsGameCode](https://github.com/TheSuperHackers/GeneralsGameCode),
which is derived from the source code released by Electronic Arts under the GPL v3 with additional terms. See
[LICENSE.md](LICENSE.md). Everything the upstream repository ships keeps its own license.

## Contained in this repository

| Component | Where | License |
|---|---|---|
| miniaudio 0.11.25 (single header, unmodified) | `Dependencies/miniaudio/miniaudio.h` | Public domain or MIT-0 (see the end of the header) |
| Everything else new in this repository (`D3DX8Compat`, `MiniMiles`, shockwave, water shaders and code, scripts, patches, docs) | various | GPL v3 with EA's additional terms, same as the code it extends |

## Not contained, fetched or built by you

| Component | Used for | License | How it is obtained |
|---|---|---|---|
| DXVK 3.1.1 | Direct3D 8 -> Vulkan translation | zlib | `tools/dxvk/build-dxvk.ps1` clones it and applies `tools/dxvk/0001-*.patch` |
| glslang | GLSL compiler used while building DXVK | BSD-3-Clause / Apache-2.0 (see its repository) | downloaded by `build-dxvk.ps1` |
| FFmpeg | decoding the game's `.bik` videos | LGPL v2.1+ (vcpkg default build; check the options you build with) | built by vcpkg from `vcpkg.json` |
| zlib | dependency of FFmpeg | zlib | vcpkg |
| DirectX 8 SDK headers | compiling against Direct3D 8 | Microsoft SDK terms | fetched by the upstream `cmake/dx8.cmake` at configure time, not stored here |
| Meson, Ninja | building DXVK | Apache-2.0 | `pip install` by `build-dxvk.ps1` |

## Not part of this project

No part of the original game is included: no executables, `.big` archives, models, textures, audio, music, videos, maps
or fonts. The Miles Sound System and Bink video SDK are not included either; they are replaced by miniaudio and FFmpeg
code paths. Third-party mods (for example "uncut" content packs) are not included and not redistributed; the
`-uncensored` flag only changes which of your own files the game prefers.

*Command & Conquer*, *Generals* and *Zero Hour* are trademarks of Electronic Arts Inc.

## release.zip

`release.zip` contains the program built from this repository, the patched DXVK DLLs (zlib license, source: DXVK 3.1.1 plus `tools/dxvk/0001-*.patch`) and the FFmpeg DLLs built by vcpkg (LGPL; sources are available from the FFmpeg project and the vcpkg port). It contains no files of the original game.
