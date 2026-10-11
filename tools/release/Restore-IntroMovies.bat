@echo off
rem Puts the startup movies moved by Skip-IntroMovies.bat back where they were.
cd /d "%~dp0"
if not exist IntroMovies_off (
  echo Nothing to restore.
  pause
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -Command "$base = (Join-Path (Get-Location).Path 'IntroMovies_off'); $n = 0; Get-ChildItem -Path $base -Recurse -File | ForEach-Object { $rel = $_.FullName.Substring($base.Length + 1); New-Item -ItemType Directory -Force (Split-Path $rel) | Out-Null; Move-Item $_.FullName $rel -Force; $n++ }; Remove-Item $base -Recurse -Force; Write-Host ('Restored ' + $n + ' file(s).')"
pause
