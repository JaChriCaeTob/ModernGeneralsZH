<#
.SYNOPSIS
  Builds the patched DXVK d3d8.dll / d3d9.dll (x64) that the updated water shaders need.

.DESCRIPTION
  Stock DXVK refuses pixel shaders above 1.x on a Direct3D 8 device ("CreateShaderModule: Unsupported PS version 2.0").
  The patch next to this script removes that one limit. Everything else is untouched DXVK 3.1.1 (zlib license).
  Nothing is downloaded into the repository: the DXVK source and build tools go to -Work (default: tools\dxvk\work, git-ignored).

  Needs: Visual Studio 2022 (C++ workload), git, Python 3 with pip. glslang is downloaded from the Khronos GitHub releases.
  The Direct3D 8 headers (d3d8.h, d3d8caps.h, d3d8types.h) are the ones the game's own CMake build fetches, so run
  scripts\build-x64.ps1 once first, or pass -Dx8Headers.

.EXAMPLE
  powershell -File tools\dxvk\build-dxvk.ps1
#>
param(
    [string]$Work = (Join-Path $PSScriptRoot "work"),
    [string]$Dx8Headers = "",
    [string]$Tag = "v3.1.1"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Dx8Headers) {
    foreach ($candidate in @("build\win64-vcpkg\_deps\dx8-src", "build\win64\_deps\dx8-src", "build\win32\_deps\dx8-src")) {
        if (Test-Path (Join-Path $repo "$candidate\d3d8.h")) { $Dx8Headers = Join-Path $repo $candidate; break }
    }
}
if (-not $Dx8Headers -or -not (Test-Path (Join-Path $Dx8Headers "d3d8.h"))) {
    throw "d3d8.h not found. Run scripts\build-x64.ps1 once (it fetches the DirectX 8 headers) or pass -Dx8Headers <dir>."
}

New-Item -ItemType Directory -Force $Work | Out-Null
$src = Join-Path $Work "dxvk"
$tools = Join-Path $Work "tools"
$hdr = Join-Path $Work "dx8inc"   # only the three D3D8 headers, so the DirectX 8 d3dx/dinput headers do not leak into the DXVK build
New-Item -ItemType Directory -Force $tools, $hdr | Out-Null
foreach ($h in "d3d8.h", "d3d8caps.h", "d3d8types.h") { Copy-Item (Join-Path $Dx8Headers $h) $hdr -Force }

# 1. DXVK source at the pinned tag, with the patch applied
if (-not (Test-Path $src)) {
    git clone --depth 1 --branch $Tag https://github.com/doitsujin/dxvk.git $src
    git -C $src submodule update --init --recursive --depth 1
    git -C $src apply (Join-Path $PSScriptRoot "0001-d3d8-allow-shader-model-2-and-3.patch")
}

# 2. meson + ninja
python -m pip install --user --quiet meson ninja
$pyScripts = python -c "import sysconfig,os; print(sysconfig.get_path('scripts', os.name + '_user'))"

# 3. glslang (the GLSL compiler DXVK uses for its internal shaders)
$glslang = Join-Path $tools "glslang"
if (-not (Test-Path (Join-Path $glslang "bin\glslang.exe"))) {
    $zip = Join-Path $tools "glslang.zip"
    Invoke-WebRequest "https://github.com/KhronosGroup/glslang/releases/download/main-tot/glslang-main-windows-x86_64-release.zip" -OutFile $zip
    Expand-Archive $zip $glslang -Force
}

# 4. build with MSVC
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$bat = Join-Path $Work "build.bat"
@"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
set PATH=$glslang\bin;$pyScripts;%PATH%
set INCLUDE=%INCLUDE%;$hdr
cd /d "$src"
if not exist build-x64\build.ninja (
  meson setup --buildtype release --backend ninja -Denable_d3d8=true -Denable_d3d9=true -Denable_d3d10=false -Denable_d3d11=false -Denable_dxgi=true build-x64
  if errorlevel 1 exit /b 1
)
ninja -C build-x64 src/d3d8/d3d8.dll src/d3d9/d3d9.dll src/dxgi/dxgi.dll
"@ | Set-Content $bat -Encoding ASCII
cmd /c "`"$bat`""
if ($LASTEXITCODE -ne 0) { throw "DXVK build failed" }

$out = Join-Path $Work "out"
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($d in "d3d8", "d3d9", "dxgi") { Copy-Item (Join-Path $src "build-x64\src\$d\$d.dll") $out -Force }
Write-Host "Patched DXVK written to $out"
