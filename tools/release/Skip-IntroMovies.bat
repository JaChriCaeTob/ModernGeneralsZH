@echo off
rem Optional: moves the startup movies (EA logo, trailer) out of the game folder for a quick start.
rem Nothing is deleted: the files go to IntroMovies_off and Restore-IntroMovies.bat puts them back.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$names = 'EA_LOGO.BIK','EA_LOGO640.BIK','sizzle_review.bik','sizzle_review640.bik'; $root = (Get-Location).Path; $n = 0; Get-ChildItem -Path 'Data' -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $names -contains $_.Name } | ForEach-Object { $rel = $_.FullName.Substring($root.Length + 1); $dst = Join-Path 'IntroMovies_off' $rel; New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null; Move-Item $_.FullName $dst -Force; $n++ }; Write-Host ('Moved ' + $n + ' startup movie file(s) to IntroMovies_off.')"
pause
