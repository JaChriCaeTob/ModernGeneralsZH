<#
.SYNOPSIS
  Packs both games into release_<Version>.zip: a "Zero Hour" folder and a "Generals" folder, each with the 64-bit program, the DXVK DLLs,
  the launchers and helper scripts, plus a README-INSTALL.txt in the root.

.DESCRIPTION
  The zip contains no files of the original games. Unzip it anywhere, then copy the content of the folder that matches the game
  into the game folder (see README-INSTALL.txt inside).
  Needs finished builds of both games (scriptsuild-x64.ps1 -Video, once with -Target z_generals and once with -Target g_generals)
  and the DXVK build (tools\dxvkuild-dxvk.ps1).

.EXAMPLE
  powershell -File tools\make-release.ps1 -Version 0.3.0
#>
param([Parameter(Mandatory = $true)][string]$Version, [string]$BuildRoot = "", [string]$DxvkDir = "", [string]$Out = "")
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
if (-not $BuildRoot) { $BuildRoot = Join-Path $repo "build\win64-vcpkg" }
if (-not $DxvkDir) { $DxvkDir = Join-Path $PSScriptRoot "dxvk\work\out" }
if (-not $Out) { $Out = Join-Path $repo "release_$Version.zip" }
$zhBuild = Join-Path $BuildRoot "GeneralsMD\Release"
$gBuild = Join-Path $BuildRoot "Generals\Release"
if (-not (Test-Path (Join-Path $zhBuild "generalszh.exe"))) { throw "No Zero Hour build in $zhBuild. Run scriptsuild-x64.ps1 -Video first." }
if (-not (Test-Path (Join-Path $gBuild "generalsv.exe"))) { throw "No Generals build in $gBuild. Run scriptsuild-x64.ps1 -Video -Target g_generals first." }
if (-not (Test-Path (Join-Path $DxvkDir "d3d8.dll"))) { throw "No DXVK build in $DxvkDir. Run tools\dxvkuild-dxvk.ps1 first." }

$stage = Join-Path $repo "build\release-staging"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $repo "LICENSE.md"), (Join-Path $repo "THIRD_PARTY_NOTICES.md") $stage

function New-GameFolder($name, $gameTitle, $buildDir, $exeIn, $exeOut)
{
    $dir = Join-Path $stage $name
    $x64 = Join-Path $dir "x64"
    New-Item -ItemType Directory -Force $x64, (Join-Path $dir "tools") | Out-Null
    Copy-Item (Join-Path $buildDir $exeIn) (Join-Path $x64 $exeOut)
    # the video libraries are built next to the Zero Hour program
    Get-ChildItem $zhBuild -Filter *.dll | ForEach-Object { Copy-Item $_.FullName $x64 }
    foreach ($d in "d3d8.dll", "d3d9.dll", "dxgi.dll") { Copy-Item (Join-Path $DxvkDir $d) $x64 }
    foreach ($f in "bigtool.py", "patch_options_wnd.py") { Copy-Item (Join-Path $PSScriptRoot $f) (Join-Path $dir "tools") }
    $common = "@echo off`r`nrem 64-bit $gameTitle. -useCwd keeps this folder as the working directory (loose Data\Cursors and Data\Movies are read from here).`r`ncd /d `"%~dp0`"`r`n"
    Set-Content (Join-Path $dir "Play-Updated.bat") ($common + "rem Updated graphics: native Vulkan renderer with all enhancements (Options.ini can switch single effects off).`r`n`"%~dp0x64\$exeOut`" -useCwd -win %*`r`n") -Encoding ASCII
    Set-Content (Join-Path $dir "Play-Original.bat") ($common + "rem Original rendering: Direct3D 8 through DXVK, no added effects.`r`nset GENERALS_GFX=d3d8`r`nset GENERALS_ORIGINAL=1`r`n`"%~dp0x64\$exeOut`" -useCwd -win %*`r`n") -Encoding ASCII
    Set-Content (Join-Path $dir "Install-OptionsMenu.bat") "@echo off`r`nrem Optional: adds the graphics check boxes to the in-game options menu. Needs Python 3. Reads the menu from your own game files and writes a patched copy next to them.`r`ncd /d `"%~dp0`"`r`npython tools\patch_options_wnd.py `"%~dp0.`"`r`nif errorlevel 1 echo Failed: is Python 3 installed and is this the $gameTitle folder?`r`npause`r`n" -Encoding ASCII
    # optional helpers: skip the startup movies for a quick start (moved, not deleted) and put them back
    Copy-Item (Join-Path $PSScriptRoot "release\Skip-IntroMovies.bat"), (Join-Path $PSScriptRoot "release\Restore-IntroMovies.bat") $dir
}
New-GameFolder "Zero Hour" "Zero Hour" $zhBuild "generalszh.exe" "generalszh64.exe"
New-GameFolder "Generals" "Generals" $gBuild "generalsv.exe" "generalsv64.exe"

Set-Content (Join-Path $stage "README-INSTALL.txt") @"
Generals and Zero Hour: 64-bit program + Vulkan renderer (experimental), version $Version

You need your own copy of the game(s) (Command & Conquer: Generals 1.08 and/or Zero Hour 1.04) and a graphics card with a
Vulkan 1.3 driver. This archive contains no game files. It has one folder per game:

    Zero Hour\   for Command & Conquer: Generals - Zero Hour
    Generals\    for Command & Conquer: Generals (the base game)

Install (do this for each game you want)
1. Unzip this archive anywhere.
2. Copy the CONTENT of the matching folder (x64\, tools\ and the .bat files, not the folder itself) into the game folder:
   the one with the .big files (INIZH.big for Zero Hour, INI.big for Generals).
   With the EA app the two games usually sit next to each other, for example
       ...\Command and Conquer Generals Zero Hour\Command and Conquer Generals Zero Hour      (Zero Hour)
       ...\Command and Conquer Generals Zero Hour\Command and Conquer Generals                (Generals)
   Do not mix them: the Zero Hour files go to the Zero Hour folder, the Generals files to the Generals folder.
3. Start Play-Updated.bat (updated graphics) or Play-Original.bat (original rendering, for comparison).
   Do not start the .exe in x64\ directly and do not copy it over an original game file.
4. Both start windowed. Remove -win inside the .bat for fullscreen. Set the resolution in the in-game options or in Options.ini
   (the game keeps one Options.ini per game in Documents\Command and Conquer ... Data).
5. Optional: run Install-OptionsMenu.bat (needs Python 3) to get the graphics check boxes in Options > advanced display options.
   Without it every effect can still be switched in Options.ini (see the README on GitHub).
6. Optional: run Skip-IntroMovies.bat for a quick start without the EA logo and trailer. Nothing is deleted, the movies are moved
   to IntroMovies_off; Restore-IntroMovies.bat puts them back.

Uninstall: delete the x64 and tools folders and the .bat files you copied and, if you installed it, the Window folder
(run Restore-IntroMovies.bat first if you used it).

Experimental one-time project: no support, no updates. The Zero Hour version has had far more play time; the Generals version was
added later and is tested less. Source code and all settings:
https://github.com/JaChriCaeTob/ModernGeneralsZH
Written by Claude Code. Licenses: LICENSE.md and THIRD_PARTY_NOTICES.md (DXVK: zlib license, FFmpeg: LGPL).
"@ -Encoding ASCII

if (Test-Path $Out) { Remove-Item $Out -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $Out -CompressionLevel Optimal
Write-Host ("Wrote {0} ({1:N1} MB)" -f $Out, ((Get-Item $Out).Length / 1MB))
