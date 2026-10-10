# Takes screenshots of the intro scene (the 3D scene behind the main menu) at fixed game times with the original and the updated graphics, for a side by side comparison.
# The times are game time since the scene started (30 logic frames per second), so every run captures the same moment of the scene.
#   tools\compare-screenshots.ps1 -GameDir "<Zero Hour folder>" [-Times "5,10,15,20"] [-OutDir docs\screenshots]
# Writes original_<frame>.png, updated_<frame>.png and compare_<frame>.png (original left, updated right). Needs Python with Pillow. Closes a running game first.
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [string]$Times = "5,10,15,20",
    [string]$OutDir = "",
    [hashtable]$ExtraEnvUpdated = @{}
)
$root = Split-Path $PSScriptRoot -Parent
if (-not $OutDir) { $OutDir = Join-Path $root "docs\screenshots" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$count = ($Times -split ",").Count
$shotsDir = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Command and Conquer Generals Zero Hour Data\Screenshots"

function Stop-Game { Get-Process -Name generalszh64, generals -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Seconds 2 }

function Run-Game($bat, $tag, $extra) {
    Stop-Game
    $env:GENERALS_SHOTS = $Times; $env:GENERALS_SHOTDIR = $OutDir; $env:GENERALS_SHOTTAG = $tag
    foreach ($k in $extra.Keys) { Set-Item "Env:$k" $extra[$k] }
    $start = Get-Date
    Start-Process -FilePath (Join-Path $GameDir $bat) -WorkingDirectory $GameDir
    $deadline = (Get-Date).AddSeconds(120 + 2 * ($Times -split "," | Measure-Object -Maximum).Maximum)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        if ($tag -eq "original") { $n = @(Get-ChildItem $shotsDir -Filter "sshot_*.png" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $start }).Count }
        else { $n = @(Get-ChildItem $OutDir -Filter "${tag}_*.bmp" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $start }).Count }
        if ($n -ge $count) { break }
    }
    Start-Sleep -Seconds 3
    Stop-Game
    foreach ($k in $extra.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    Remove-Item Env:GENERALS_SHOTS, Env:GENERALS_SHOTDIR, Env:GENERALS_SHOTTAG -ErrorAction SilentlyContinue
    return $start
}

# original graphics (classic Direct3D 8 path): the game writes its own screenshots to the user data folder
$start = Run-Game "Play-Original.bat" "original" @{}
$frames = ($Times -split ",") | ForEach-Object { [int]([double]$_ * 30 + 0.5) }
$files = @(Get-ChildItem $shotsDir -Filter "sshot_*.png" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $start } | Sort-Object LastWriteTime)
for ($i = 0; $i -lt [Math]::Min($files.Count, $frames.Count); $i++) {
    Move-Item $files[$i].FullName ("{0}\original_{1:0000}.png" -f $OutDir, $frames[$i]) -Force
}
# updated graphics (native Vulkan renderer)
if (-not $ExtraEnvUpdated.ContainsKey("GENERALS_SUN")) { $ExtraEnvUpdated["GENERALS_SUN"] = "215,48" }      # the sun is random for every launch: a fixed one makes the shadows comparable
Run-Game "Play-Updated.bat" "updated" $ExtraEnvUpdated | Out-Null
python (Join-Path $PSScriptRoot "make_compare.py") $OutDir

