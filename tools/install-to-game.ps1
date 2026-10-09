<#
.SYNOPSIS
  Copies a freshly built 64-bit Zero Hour into a x64 sub folder of YOUR OWN game install and writes launcher scripts.

.DESCRIPTION
  This repository contains no game files. You need a legitimate installation of Command & Conquer: Generals - Zero Hour.
  The script only copies things that you built from this repository:
    <GameDir>\x64\generalszh64.exe          the game
    <GameDir>\x64\*.dll (FFmpeg, zlib)      next to the exe, from the build output
    <GameDir>\x64\d3d8.dll d3d9.dll dxgi.dll the patched DXVK from tools\dxvk\build-dxvk.ps1 (Direct3D 8 -> Vulkan)
    <GameDir>\Play-Updated.bat, Play-Original.bat
  Existing files of the original game are never touched. Delete the x64 folder and the two .bat files to uninstall.

.EXAMPLE
  powershell -File tools\install-to-game.ps1 -GameDir "D:\Games\Command and Conquer Generals Zero Hour"
#>
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [string]$BuildDir = "",
    [string]$DxvkDir = "",
    [string]$Mod = ""    # optional: a mod folder (relative to GameDir) with extra .big files, passed as -mod
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path (Join-Path $GameDir "generalszh.exe")) -and -not (Test-Path (Join-Path $GameDir "Data\INI"))) {
    Write-Warning "$GameDir does not look like a Zero Hour install (no generalszh.exe, no Data\INI). Continuing anyway."
}
if (-not $BuildDir) {
    foreach ($b in "build\win64-vcpkg\GeneralsMD\Release", "build\win64\GeneralsMD\Release") {
        if (Test-Path (Join-Path $repo "$b\generalszh.exe")) { $BuildDir = Join-Path $repo $b; break }
    }
}
if (-not $BuildDir -or -not (Test-Path (Join-Path $BuildDir "generalszh.exe"))) { throw "No build found. Run scripts\build-x64.ps1 -Video first." }
if (-not $DxvkDir) { $DxvkDir = Join-Path $PSScriptRoot "dxvk\work\out" }
if (-not (Test-Path (Join-Path $DxvkDir "d3d8.dll"))) { throw "No DXVK build found in $DxvkDir. Run tools\dxvk\build-dxvk.ps1 first." }

$x64 = Join-Path $GameDir "x64"
New-Item -ItemType Directory -Force $x64 | Out-Null
Copy-Item (Join-Path $BuildDir "generalszh.exe") (Join-Path $x64 "generalszh64.exe") -Force
Get-ChildItem $BuildDir -Filter *.dll | ForEach-Object { Copy-Item $_.FullName $x64 -Force }
foreach ($d in "d3d8.dll", "d3d9.dll", "dxgi.dll") { Copy-Item (Join-Path $DxvkDir $d) $x64 -Force }

$modArg = if ($Mod) { " -mod `"%~dp0$Mod`"" } else { "" }
$common = @"
@echo off
rem 64-bit Zero Hour from this repository. -useCwd keeps this folder as the working directory
rem (loose Data\Cursors and Data\Movies are read from here).
cd /d "%~dp0"
"@
# two launchers: updated graphics (default) and the original rendering; older launchers are removed
foreach ($old in "Play-x64.bat", "Play-x64-Fullscreen.bat", "Play-x64-Vulkan.bat", "Start_Native_Vulkan.bat") { Remove-Item (Join-Path $GameDir $old) -ErrorAction SilentlyContinue }
Set-Content (Join-Path $GameDir "Play-Updated.bat") ($common + "`r`nrem Updated graphics: native Vulkan renderer with all enhancements (Options.ini can switch single effects off).`r`n`"%~dp0x64\generalszh64.exe`" -useCwd -win$modArg %*`r`n") -Encoding ASCII
Set-Content (Join-Path $GameDir "Play-Original.bat") ($common + "`r`nrem Original rendering: Direct3D 8 through DXVK, no added effects (also no updated water or shockwaves).`r`nset GENERALS_GFX=d3d8`r`nset GENERALS_ORIGINAL=1`r`n`"%~dp0x64\generalszh64.exe`" -useCwd -win$modArg %*`r`n") -Encoding ASCII

# extra check boxes in the options menu (advanced pane): patched copy of the game's OptionsMenu.wnd, written as a loose file
if (Get-Command python -ErrorAction SilentlyContinue) {
    python (Join-Path $PSScriptRoot "patch_options_wnd.py") $GameDir
} else {
    Write-Host "Python not found: the extra graphics check boxes in the options menu were not installed (the Options.ini keys still work)."
}

Write-Host "Installed to $x64. Start the game with Play-Updated.bat or Play-Original.bat (both windowed: remove -win in the .bat for fullscreen)."
Write-Host "Set the resolution in the in-game options or in Options.ini (see README). Rendering is uncapped by default; FPSLimit = yes in Options.ini restores the 30 fps cap."
