# Builds the 64-bit Zero Hour executable. Run from any shell.
#   scripts\build-x64.ps1            plain 64-bit build (no FFmpeg video), build\win64
#   scripts\build-x64.ps1 -Video     64-bit build with FFmpeg video through vcpkg (needs VCPKG_ROOT), build\win64-vcpkg
param([string]$Target = "z_generals", [string]$Config = "Release", [switch]$Clean, [switch]$Video)
$extra = if ($Clean) { "--clean-first" } else { "" }
$root = Split-Path $PSScriptRoot -Parent
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$shim = Join-Path $root "build\shim"
New-Item -ItemType Directory -Force $shim | Out-Null
Set-Content (Join-Path $shim "afxres.h") "#include <winres.h>`n"   # RTS.RC wants MFC's afxres.h
if ($Video) {
    if (-not $env:VCPKG_ROOT) { throw "-Video builds FFmpeg through vcpkg: set VCPKG_ROOT to your vcpkg checkout (see README)." }
    $preset = "win64-vcpkg"; $dir = "build/win64-vcpkg"; $opt = "-DRTS_BUILD_OPTION_FFMPEG=ON"
} else {
    $preset = "win64"; $dir = "build/win64"; $opt = ""
}
cmd /c "`"$vs\VC\Auxiliary\Build\vcvarsall.bat`" x64 >nul 2>&1 && cd /d `"$root`" && cmake --preset $preset $opt -DCMAKE_RC_FLAGS=`"-DWIN32 /I$shim`" && cmake --build $dir --config $Config --target $Target $extra -- -k 0"
