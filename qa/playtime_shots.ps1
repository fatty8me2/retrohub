# Headless screenshots of the Steam-style play time (game page header + stats strip, library sorted by Most played).
# Headless only (SDL dummy video driver: no window is ever shown), muted, fake library: never touches a real library or vault.
# usage: qa\playtime_shots.ps1 [-Out <dir>] [-Exe <RetroHub.exe>] [-Sizes 1280x800,1600x1000]
# Output: <Out>\<name>.png for every shot plus contact sheets <Out>\sheet_details.png and <Out>\sheet_library.png.
param([string]$Out = "$env:TEMP\playtime_shots", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe",
      [string[]]$Sizes = @('1280x800', '1600x1000'))
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Drawing
$env:RETROHUB_NO_SHELL = '1'; $env:HUB_SHOT_NOWAIT = '1'; $env:HUB_SHOT_SECS = '3'; $env:RETROHUB_QA = '1'
$env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'
$script:made = @()

function Shot([string]$name, [hashtable]$vars) {
    foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
    $bmp = Join-Path $Out "$name.bmp"
    $env:HUB_SCREENSHOT = $bmp
    $env:HUB_SHOT_TRACE = Join-Path $Out "$name.trace.txt"
    Remove-Item -LiteralPath $bmp -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }   # our own child, by PID
    Remove-Item env:HUB_SCREENSHOT, env:HUB_SHOT_TRACE -ErrorAction SilentlyContinue
    foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $bmp) {
        $png = Join-Path $Out "$name.png"
        $img = [System.Drawing.Image]::FromFile($bmp)
        $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
        Remove-Item -LiteralPath $bmp
        $script:made += $png
        "SHOT OK   $name"
    } else { "SHOT MISSING  $name" }
}

foreach ($sz in $Sizes) {
    $w = $sz.Split('x')[0]
    # The game page (details modal) with made-up play data: 42.6 h + other PC, 1,234 h, 0.4 h, never played, and an unplayed Switch 2 game.
    # Select the game, then "i" opens its page (HUB_SHOT_DETAILS=<key> alone falls back to the first game: the selection is reset
    # while the library loads).
    $page = @{ HUB_SHOT_SIZE = $sz; HUB_SHOT_FAKE_LIB = '8'; HUB_SHOT_PLAYTIME = '1'; HUB_SHOT_KEYS = 'i' }
    Shot "page_42h_$w" ($page + @{ HUB_SELECT_GAME = 'fake:0' })
    Shot "page_1234h_$w" ($page + @{ HUB_SELECT_GAME = 'fake:1' })
    Shot "page_04h_$w" ($page + @{ HUB_SELECT_GAME = 'fake:2' })
    Shot "page_unplayed_$w" ($page + @{ HUB_SELECT_GAME = 'fake:3' })
    Shot "page_switch2_unplayed_$w" ($page + @{ HUB_SHOT_SWITCH = '1'; HUB_SELECT_GAME = 'switch:0400000000a1b000' })
    # The library sorted by Most played (play time on a quarter of 60 made-up games).
    $lib = @{ HUB_SHOT_SIZE = $sz; HUB_SHOT_FAKE_LIB = '60'; HUB_SHOT_FAKE_AZ = '1'; HUB_START_VIEW = 'all'; HUB_SHOT_SORT = 'most_played' }
    foreach ($lay in 'grid', 'list', 'cases', 'showcase', 'shelf') { Shot "lib_${lay}_mostplayed_$w" ($lib + @{ HUB_SHOT_LIBVIEW = $lay }) }
    Shot "lib_grid_leastplayed_$w" (@{ HUB_SHOT_SIZE = $sz; HUB_SHOT_FAKE_LIB = '60'; HUB_SHOT_FAKE_AZ = '1'; HUB_START_VIEW = 'all'; HUB_SHOT_SORT = 'most_played+rev'; HUB_SHOT_LIBVIEW = 'grid' })
    Shot "lib_grid_title_$w" (@{ HUB_SHOT_SIZE = $sz; HUB_SHOT_FAKE_LIB = '60'; HUB_SHOT_FAKE_AZ = '1'; HUB_START_VIEW = 'all'; HUB_SHOT_SORT = 'title'; HUB_SHOT_LIBVIEW = 'grid' })
}

$sheet = Join-Path $PSScriptRoot 'contact_sheet.ps1'
$det = @($script:made | Where-Object { (Split-Path $_ -Leaf) -like 'page_*' })
$libs = @($script:made | Where-Object { (Split-Path $_ -Leaf) -like 'lib_*' })
if ($det.Count) { & $sheet (Join-Path $Out 'sheet_details.png') @det -Cols 2 -Full }
if ($libs.Count) { & $sheet (Join-Path $Out 'sheet_library.png') @libs -Cols 2 -Full }
"SHOTS $($script:made.Count)"
