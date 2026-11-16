# Headless screenshots of the UI kit demo (HUB_SHOT_KITDEMO=sheet|pane|dialog, see docs/QA_SHOTS.md): page sheet, desk pane and dialog
# at 1280x720, 1920x1080, 1024x600, UI size 150 %, the Light theme, and pad runs (focus follow, dropdown, LT / RT paging, LB / RB tabs).
# Runs a SANDBOX COPY of build\bin\Release (no userdata) with a scratch vault, muted, SDL dummy driver: no window is ever shown.
# Refuses to start while a game runs. Usage: powershell -NoProfile -ExecutionPolicy Bypass -File qa\uikit_shots.ps1 [-Out dir] [-Only regex]
param([string]$Out = '', [string]$Only = '.')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
if (-not $Out) { $Out = Join-Path $root 'build\qa_uikit_out' }
$src = Join-Path $root 'build\bin\Release'
$box = Join-Path $root 'build\qa_uikit_box'
$busy = Get-Process lichii,eden-cli,hookshot,Dolphin,Cemu,azahar,pcsx2-qt,PPSSPPWindows64,retrorun,n64run -ErrorAction SilentlyContinue |
    Where-Object { -not $_.Path -or (($_.Path -notlike "$(Split-Path $root)\*") -and ($_.Path -notlike '*\Temp\*')) }
if ($busy) { Write-Output "A game is running ($($busy.Name -join ', ')): not starting."; exit 3 }
New-Item -ItemType Directory -Force $Out | Out-Null
if (Test-Path $box) { Remove-Item -Recurse -Force $box }
New-Item -ItemType Directory -Force $box | Out-Null
Get-ChildItem $src | Where-Object { $_.Name -ne 'userdata' } | ForEach-Object { Copy-Item -Recurse -Force $_.FullName $box }
Add-Type -AssemblyName System.Drawing

$d12 = (@('down') * 12) -join ','
$runs = @(
    @{ n = 'sheet_720';        size = '1280x720';  env = 'HUB_SHOT_KITDEMO=sheet' },
    @{ n = 'sheet_1080';       size = '1920x1080'; env = 'HUB_SHOT_KITDEMO=sheet' },
    @{ n = 'sheet_600';        size = '1024x600';  env = 'HUB_SHOT_KITDEMO=sheet' },
    @{ n = 'sheet_720_s150';   size = '1280x720';  env = 'HUB_SHOT_KITDEMO=sheet;HUB_UI_SCALE=150' },
    @{ n = 'sheet_720_light';  size = '1280x720';  env = 'HUB_SHOT_KITDEMO=sheet;HUB_THEME=light' },
    @{ n = 'sheet_follow';     size = '1280x720';  env = "HUB_SHOT_KITDEMO=sheet;HUB_SHOT_PAD=down,down,down,down,shot:d4,$d12,shot:d16,down,down" },
    @{ n = 'sheet_dd';         size = '1280x720';  env = 'HUB_SHOT_KITDEMO=sheet;HUB_SHOT_PAD=down,down,down,down,a,shot:open,down,down,shot:moved,b,shot:closed,a,down,a' },
    @{ n = 'sheet_page';       size = '1280x720';  env = 'HUB_SHOT_KITDEMO=sheet;HUB_SHOT_PAD=down,down,down,rt,shot:rt1,rt,shot:rt2,rt,shot:rt3,lt' },
    @{ n = 'sheet_tabs';       size = '1280x720';  env = 'HUB_SHOT_KITDEMO=sheet;HUB_SHOT_PAD=down,down,down,down,rb,shot:rb,lb,shot:lb,lb,lb' },
    @{ n = 'sheet_600_s150_follow'; size = '1024x600'; env = "HUB_SHOT_KITDEMO=sheet;HUB_UI_SCALE=150;HUB_SHOT_PAD=down,down,down,down,down,down,shot:d6,$d12" },
    @{ n = 'pane_720';         size = '1280x720';  env = 'HUB_SHOT_KITDEMO=pane' },
    @{ n = 'pane_1080';        size = '1920x1080'; env = 'HUB_SHOT_KITDEMO=pane' },
    @{ n = 'pane_600';         size = '1024x600';  env = 'HUB_SHOT_KITDEMO=pane' },
    @{ n = 'pane_720_s150';    size = '1280x720';  env = 'HUB_SHOT_KITDEMO=pane;HUB_UI_SCALE=150' },
    @{ n = 'pane_720_light';   size = '1280x720';  env = 'HUB_SHOT_KITDEMO=pane;HUB_THEME=light' },
    @{ n = 'pane_follow';      size = '1280x720';  env = "HUB_SHOT_KITDEMO=pane;HUB_SHOT_PAD=right,shot:r,down,down,down,$d12" },
    @{ n = 'dialog_720';       size = '1280x720';  env = 'HUB_SHOT_KITDEMO=dialog' },
    @{ n = 'dialog_600';       size = '1024x600';  env = 'HUB_SHOT_KITDEMO=dialog' },
    @{ n = 'dialog_600_s150';  size = '1024x600';  env = 'HUB_SHOT_KITDEMO=dialog;HUB_UI_SCALE=150;HUB_SHOT_PAD=rt,shot:rt,b' },
    @{ n = 'dialog_720_light'; size = '1280x720';  env = 'HUB_SHOT_KITDEMO=dialog;HUB_THEME=light' }
)
$fail = 0
foreach ($r in $runs) {
    if ($r.n -notmatch $Only) { continue }
    if (Test-Path "$box\userdata") { Remove-Item -Recurse -Force "$box\userdata" }
    $vault = Join-Path $box 'vault'
    if (Test-Path $vault) { Remove-Item -Recurse -Force $vault }
    New-Item -ItemType Directory -Force $vault | Out-Null
    $bmp = Join-Path $Out "$($r.n).bmp"
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $box 'RetroHub.exe'
    $psi.WorkingDirectory = $box
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    foreach ($kv in @("HUB_SCREENSHOT=$bmp", "HUB_SHOT_SIZE=$($r.size)", 'HUB_SHOT_SECS=4', "HUB_SHOT_VAULT=$vault", 'HUB_SHOT_FAKE_LIB=6',
                      'RETROHUB_QA=1', 'SSB64_MUTE=1', 'RETROHUB_NO_CLOUD=1') + ($r.env -split ';')) {
        if ($kv -match '^([^=]+)=(.*)$') { $psi.EnvironmentVariables[$Matches[1]] = $Matches[2] }
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force; Write-Output "$($r.n): TIMEOUT"; $fail++; continue }
    $made = Get-ChildItem $Out -Filter "$($r.n)*.bmp"
    foreach ($f in $made) {
        $img = [System.Drawing.Image]::FromFile($f.FullName)
        $img.Save([System.IO.Path]::ChangeExtension($f.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
        Remove-Item $f.FullName -Force
    }
    Write-Output "$($r.n): exit $($p.ExitCode), $(@($made).Count) image(s)"
    if ($p.ExitCode -ne 0 -or @($made).Count -eq 0) { $fail++ }
}
Remove-Item -Recurse -Force $box
Write-Output "PNGs in $Out; $fail failure(s)"
exit ([int]($fail -gt 0))
