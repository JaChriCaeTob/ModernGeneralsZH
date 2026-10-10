<#
.SYNOPSIS
  Packs the built game into release.zip: the 64-bit program, the DXVK DLLs, the two launchers and an installer note.

.DESCRIPTION
  The zip contains no files of the original game. Unzip it into your Zero Hour folder (see README-INSTALL.txt inside).
  Needs a finished build (scripts\build-x64.ps1 -Video) and the DXVK build (tools\dxvk\build-dxvk.ps1).

.EXAMPLE
  powershell -File tools\make-release.ps1
#>
param([string]$BuildDir = "", [string]$DxvkDir = "", [string]$Out = "")
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
if (-not $BuildDir) { $BuildDir = Join-Path $repo "build\win64-vcpkg\GeneralsMD\Release" }
if (-not $DxvkDir) { $DxvkDir = Join-Path $PSScriptRoot "dxvk\work\out" }
if (-not $Out) { $Out = Join-Path $repo "release.zip" }
if (-not (Test-Path (Join-Path $BuildDir "generalszh.exe"))) { throw "No build in $BuildDir. Run scripts\build-x64.ps1 -Video first." }
if (-not (Test-Path (Join-Path $DxvkDir "d3d8.dll"))) { throw "No DXVK build in $DxvkDir. Run tools\dxvk\build-dxvk.ps1 first." }

$stage = Join-Path $repo "build\release-staging"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$x64 = Join-Path $stage "x64"
New-Item -ItemType Directory -Force $x64, (Join-Path $stage "tools") | Out-Null
Copy-Item (Join-Path $BuildDir "generalszh.exe") (Join-Path $x64 "generalszh64.exe")
Get-ChildItem $BuildDir -Filter *.dll | ForEach-Object { Copy-Item $_.FullName $x64 }
foreach ($d in "d3d8.dll", "d3d9.dll", "dxgi.dll") { Copy-Item (Join-Path $DxvkDir $d) $x64 }
foreach ($f in "bigtool.py", "patch_options_wnd.py") { Copy-Item (Join-Path $PSScriptRoot $f) (Join-Path $stage "tools") }
Copy-Item (Join-Path $repo "LICENSE.md"), (Join-Path $repo "THIRD_PARTY_NOTICES.md") $stage

$common = "@echo off`r`nrem 64-bit Zero Hour. -useCwd keeps this folder as the working directory (loose Data\Cursors and Data\Movies are read from here).`r`ncd /d `"%~dp0`"`r`n"
Set-Content (Join-Path $stage "Play-Updated.bat") ($common + "rem Updated graphics: native Vulkan renderer with all enhancements (Options.ini can switch single effects off).`r`n`"%~dp0x64\generalszh64.exe`" -useCwd -win %*`r`n") -Encoding ASCII
Set-Content (Join-Path $stage "Play-Original.bat") ($common + "rem Original rendering: Direct3D 8 through DXVK, no added effects.`r`nset GENERALS_GFX=d3d8`r`nset GENERALS_ORIGINAL=1`r`n`"%~dp0x64\generalszh64.exe`" -useCwd -win %*`r`n") -Encoding ASCII
Set-Content (Join-Path $stage "Install-OptionsMenu.bat") "@echo off`r`nrem Optional: adds the graphics check boxes to the in-game options menu. Needs Python 3. Reads the menu from your own game files and writes a patched copy next to them.`r`ncd /d `"%~dp0`"`r`npython tools\patch_options_wnd.py `"%~dp0.`"`r`nif errorlevel 1 echo Failed: is Python 3 installed and is this the Zero Hour folder?`r`npause`r`n" -Encoding ASCII
Set-Content (Join-Path $stage "README-INSTALL.txt") @"
Zero Hour x64 + Vulkan renderer (experimental)

You need your own copy of Command & Conquer: Generals - Zero Hour 1.04 and a graphics card with a Vulkan 1.3 driver.
This archive contains no game files.

Install
1. Unzip everything into your Zero Hour folder (the one with generalszh.exe and the .big files).
   You get: x64\, Play-Updated.bat, Play-Original.bat, Install-OptionsMenu.bat, tools\.
2. Start Play-Updated.bat (updated graphics) or Play-Original.bat (original rendering, for comparison).
   Do not start x64\generalszh64.exe directly and do not copy it over generalszh.exe.
3. Both start windowed. Remove -win inside the .bat for fullscreen. Set the resolution in the in-game options or in Options.ini.
4. Optional: run Install-OptionsMenu.bat (needs Python 3) to get the graphics check boxes in Options > advanced display options.
   Without it every effect can still be switched in Options.ini (see the README on GitHub).

Uninstall: delete the x64 and tools folders, the .bat files and, if you installed it, the Window folder.

Experimental one-time project: no support, no updates. Source code and all settings:
https://github.com/JaChriCaeTob/ModernGeneralsZH
Written by Claude Code. Licenses: LICENSE.md and THIRD_PARTY_NOTICES.md (DXVK: zlib license, FFmpeg: LGPL).
"@ -Encoding ASCII

if (Test-Path $Out) { Remove-Item $Out -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $Out -CompressionLevel Optimal
Write-Host ("Wrote {0} ({1:N1} MB)" -f $Out, ((Get-Item $Out).Length / 1MB))
