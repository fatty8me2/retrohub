# Headless UI QA for the Steam-style play time: game page at several window sizes and DPI scales, narrow windows, and the library
# (grid + list, Most played). Headless only (SDL dummy video driver: no window is ever shown), muted, fake library.
# Never touches a real library or vault. Kills nothing by name; every RetroHub process is one we start, and we wait on it.
# usage: qa\playtime_qa_ui.ps1 [-Out <dir>] [-Exe <RetroHub.exe>]
# Output: <Out>\<name>.png for every shot, plus contact sheets sheet_ui_pages.png, sheet_ui_narrow.png, sheet_ui_library.png.
# Exit code: 0 when every shot was produced, 1 otherwise.
param([string]$Out = "C:\Users\fatty\AppData\Local\Temp\claude\a82a6632bfc5ba1b\c9e79f3c-c9ea-4754-b3cd-65f95c7454b4\scratchpad\playtime\qa_ui",
      [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Drawing
$env:RETROHUB_NO_SHELL = '1'; $env:HUB_SHOT_NOWAIT = '1'; $env:HUB_SHOT_SECS = '3'; $env:RETROHUB_QA = '1'
$env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'
$script:made = @()
$script:failed = @()

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
    } else { $script:failed += $name; "SHOT MISSING  $name" }
}

$page = @{ HUB_SHOT_FAKE_LIB = '8'; HUB_SHOT_PLAYTIME = '1'; HUB_SHOT_KEYS = 'i' }

# 1. Game pages, scale 1.5, at 1600x1000 and 1280x800.
foreach ($sz in '1600x1000', '1280x800') {
    $w = $sz.Split('x')[0]
    foreach ($n in 0..4) {
        Shot "page_fake${n}_${w}_s15" ($page + @{ HUB_SHOT_SIZE = $sz; HUB_SCALE = '1.5'; HUB_SELECT_GAME = "fake:$n" })
    }
}

# 2. Narrow windows: fake:0 and fake:1 at 1024x640 (scale 1 and 1.5) and 960x600 (scale 1).
foreach ($n in 0, 1) {
    Shot "narrow_fake${n}_1024x640_s1"  ($page + @{ HUB_SHOT_SIZE = '1024x640'; HUB_SCALE = '1';   HUB_SELECT_GAME = "fake:$n" })
    Shot "narrow_fake${n}_1024x640_s15" ($page + @{ HUB_SHOT_SIZE = '1024x640'; HUB_SCALE = '1.5'; HUB_SELECT_GAME = "fake:$n" })
    Shot "narrow_fake${n}_960x600_s1"   ($page + @{ HUB_SHOT_SIZE = '960x600';  HUB_SCALE = '1';   HUB_SELECT_GAME = "fake:$n" })
}

# 3. Library sorted by Most played: grid and list, at 1280x800 scale 1.5 and 1024x640 scale 1.
$lib = @{ HUB_SHOT_FAKE_LIB = '60'; HUB_SHOT_PLAYTIME = '1'; HUB_START_VIEW = 'all'; HUB_SHOT_SORT = 'most_played'; HUB_SELECT_GAME = 'fake:1' }
foreach ($view in 'grid', 'list') {
    Shot "lib_${view}_1280x800_s15" ($lib + @{ HUB_SHOT_SIZE = '1280x800'; HUB_SCALE = '1.5'; HUB_SHOT_LIBVIEW = $view })
    Shot "lib_${view}_1024x640_s1"  ($lib + @{ HUB_SHOT_SIZE = '1024x640'; HUB_SCALE = '1';   HUB_SHOT_LIBVIEW = $view })
}

$sheet = Join-Path $PSScriptRoot 'contact_sheet.ps1'
$pages  = @($script:made | Where-Object { (Split-Path $_ -Leaf) -like 'page_*' })
$narrow = @($script:made | Where-Object { (Split-Path $_ -Leaf) -like 'narrow_*' })
$libs   = @($script:made | Where-Object { (Split-Path $_ -Leaf) -like 'lib_*' })
if ($pages.Count)  { & $sheet (Join-Path $Out 'sheet_ui_pages.png') @pages -Cols 2 -Full }
if ($narrow.Count) { & $sheet (Join-Path $Out 'sheet_ui_narrow.png') @narrow -Cols 2 -Full }
if ($libs.Count)   { & $sheet (Join-Path $Out 'sheet_ui_library.png') @libs -Cols 2 -Full }
"SHOTS $($script:made.Count) MISSING $($script:failed.Count)"
if ($script:failed.Count) { exit 1 }
exit 0
