# Headless screenshots for the scrolling work (Home shelves, All Games views, the empty library, the Cases menu, Play Online's friends'
# rooms, Discover's pinned filters, Saves / Controllers / Settings / Updates): static shots at 1024x600, 1280x720, 1920x1080 and UI size
# 150 %, plus wheel (HUB_SHOT_WHEEL) and LT / RT (HUB_SHOT_PAD) runs that must move the content and the scrollbar thumb.
# Runs a SANDBOX COPY of the given build output (no userdata) with a scratch vault, muted, SDL dummy driver: no window is ever shown.
# Refuses to start while a game runs.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File qa\scroll_shots.ps1 [-Bin <build output dir>] [-Tag after] [-Out dir] [-Only regex]
#   powershell -NoProfile -ExecutionPolicy Bypass -File qa\scroll_shots.ps1 -Compare <dirA>;<dirB> [-Only regex]
#
# -Bin defaults to build\bin\Release; give a frozen copy of another commit's build to capture the "before" set (e.g. -Bin build\qa_base_bin
# -Tag before). -Compare prints, per PNG present in both folders, IDENTICAL or the number of differing pixels and their bounding box.
param([string]$Bin = '', [string]$Tag = 'after', [string]$Out = '', [string]$Only = '.', [string]$Compare = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
Add-Type -AssemblyName System.Drawing
. (Join-Path $PSScriptRoot '_gamecheck.ps1')

$cmp = @($Compare -split ';' | Where-Object { $_ })
if ($Compare -and $cmp.Count -ne 2) { Write-Output 'Compare takes two folders: -Compare <dirA>;<dirB>'; exit 2 }
if ($cmp.Count -eq 2) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class ScrollShotDiff {
    public static string Diff(string a, string b) {
        using (var x = new Bitmap(a)) using (var y = new Bitmap(b)) {
            if (x.Width != y.Width || x.Height != y.Height) return "SIZE " + x.Width + "x" + x.Height + " vs " + y.Width + "x" + y.Height;
            var r = new Rectangle(0, 0, x.Width, x.Height);
            var dx = x.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            var dy = y.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            int n = x.Width * x.Height; var px = new int[n]; var py = new int[n];
            Marshal.Copy(dx.Scan0, px, 0, n); Marshal.Copy(dy.Scan0, py, 0, n);
            x.UnlockBits(dx); y.UnlockBits(dy);
            int cnt = 0, x0 = int.MaxValue, y0 = int.MaxValue, x1 = -1, y1 = -1;
            for (int i = 0; i < n; i++) if (px[i] != py[i]) { cnt++; int cx = i % x.Width, cy = i / x.Width; x0 = Math.Min(x0, cx); y0 = Math.Min(y0, cy); x1 = Math.Max(x1, cx); y1 = Math.Max(y1, cy); }
            return cnt == 0 ? "IDENTICAL" : "DIFF pixels=" + cnt + " bbox=(" + x0 + "," + y0 + ")-(" + x1 + "," + y1 + ")";
        }
    }
}
"@
    Get-ChildItem (Join-Path $cmp[0] '*.png') | Where-Object { $_.BaseName -match $Only } | ForEach-Object {
        $other = Join-Path $cmp[1] $_.Name
        if (Test-Path $other) { Write-Output ("{0}: {1}" -f $_.Name, [ScrollShotDiff]::Diff($_.FullName, $other)) }
    }
    exit 0
}

if (-not $Bin) { $Bin = Join-Path $root 'build\bin\Release' }
if (-not $Out) { $Out = Join-Path $root "build\qa_scroll_$Tag" }
$box = Join-Path $root "build\qa_x_scroll_$Tag"
$busy = Get-Process lichii,eden-cli,hookshot,Dolphin,Cemu,azahar,pcsx2-qt,PPSSPPWindows64,retrorun,n64run -ErrorAction SilentlyContinue |
    Where-Object { -not $_.Path -or (($_.Path -notlike "$(Split-Path $root)\*") -and ($_.Path -notlike '*\Temp\*')) }
if ($busy) { Write-Output "A game is running ($($busy.Name -join ', ')): not starting."; exit 3 }
New-Item -ItemType Directory -Force $Out | Out-Null
if (Test-Path $box) { Remove-Item -Recurse -Force $box }
New-Item -ItemType Directory -Force $box | Out-Null
Get-ChildItem $Bin | Where-Object { $_.Name -ne 'userdata' } | ForEach-Object { Copy-Item -Recurse -Force $_.FullName $box }

$homeEnv = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_FAKE_ART=1;HUB_SHOT_CONTINUE=1'
$big = 'HUB_SHOT_FAKE_LIB=500;HUB_SHOT_FAKE_AZ=1;HUB_SHOT_FAKE_ART=1'
$cases = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_FAKE_ART=1;HUB_SHOT_CASES=browse;HUB_SHOT_PAD=start'
$noGhosts = '{"show_friend_games":false,"friend_games_v":1}'
$rooms = 'HUB_SHOT_PAGE=online;HUB_SHOT_FAKE_FRIENDS=1;HUB_SHOT_FAKE_ROOMS=10'
$runs = @(
    # 1. Home shelves: a scrollbar over the right edge, the wheel (over the shelves and over the hero), LT / RT
    @{ n = 'home_600';          size = '1024x600';  env = $homeEnv },
    @{ n = 'home_720';          size = '1280x720';  env = $homeEnv },
    @{ n = 'home_1080';         size = '1920x1080'; env = $homeEnv },
    @{ n = 'home_720_s150';     size = '1280x720';  env = "$homeEnv;HUB_UI_SCALE=150" },
    @{ n = 'home_wheel';        size = '1280x720';  env = "$homeEnv;HUB_SHOT_WHEEL=700,560,4" },
    @{ n = 'home_wheel_hero';   size = '1280x720';  env = "$homeEnv;HUB_SHOT_WHEEL=900,200,4" },
    @{ n = 'home_pad';          size = '1280x720';  env = "$homeEnv;HUB_SHOT_PAD=down,shot:d1,rt,shot:rt1,rt,shot:rt2,lt,shot:lt1,lt" },
    @{ n = 'home_pad_down';     size = '1920x1080'; env = "$homeEnv;HUB_SHOT_PAD=down,down,down,shot:d3,down,down,shot:d5,down,down,shot:d7,up,up,up" },
    # 2. All Games views (500 games)
    @{ n = 'ag_grid_600';       size = '1024x600';  env = "$big;HUB_SHOT_LIBVIEW=grid" },
    @{ n = 'ag_grid_720';       size = '1280x720';  env = "$big;HUB_SHOT_LIBVIEW=grid" },
    @{ n = 'ag_grid_1080';      size = '1920x1080'; env = "$big;HUB_SHOT_LIBVIEW=grid" },
    @{ n = 'ag_list_720';       size = '1280x720';  env = "$big;HUB_SHOT_LIBVIEW=list" },
    @{ n = 'ag_list_720_s150';  size = '1280x720';  env = "$big;HUB_SHOT_LIBVIEW=list;HUB_UI_SCALE=150" },
    @{ n = 'ag_compact_720';    size = '1280x720';  env = "$big;HUB_SHOT_LIBVIEW=compact" },
    @{ n = 'ag_showcase_720';   size = '1280x720';  env = "$big;HUB_SHOT_LIBVIEW=showcase" },
    @{ n = 'ag_shelf_720';      size = '1280x720';  env = "$big;HUB_SHOT_LIBVIEW=shelf" },
    @{ n = 'ag_grid_wheel';     size = '1280x720';  env = "$big;HUB_SHOT_LIBVIEW=grid;HUB_SHOT_WHEEL=600,450,6" },
    @{ n = 'ag_list_wheel';     size = '1280x720';  env = "$big;HUB_SHOT_LIBVIEW=list;HUB_SHOT_WHEEL=600,450,6" },
    @{ n = 'ag_shelf_wheel';    size = '1280x720';  env = "$big;HUB_SHOT_LIBVIEW=shelf;HUB_SHOT_WHEEL=600,450,6" },
    @{ n = 'ag_grid_pad';       size = '1280x720';  env = "$big;HUB_SHOT_LIBVIEW=grid;HUB_SHOT_PAD=down,rt,shot:rt1,rt,shot:rt2,lt" },
    @{ n = 'ag_grid_fits_1080'; size = '1920x1080'; env = 'HUB_SHOT_FAKE_LIB=6;HUB_SHOT_FAKE_ART=1;HUB_SHOT_LIBVIEW=grid' },
    # 3. The empty library (first run; friends' games off so nothing at all is listed)
    @{ n = 'empty_600';         size = '1024x600';  env = 'HUB_SHOT_FAKE_LIB=0'; hub = $noGhosts },
    @{ n = 'empty_600_s150';    size = '1024x600';  env = 'HUB_SHOT_FAKE_LIB=0;HUB_UI_SCALE=150'; hub = $noGhosts },
    @{ n = 'empty_720';         size = '1280x720';  env = 'HUB_SHOT_FAKE_LIB=0'; hub = $noGhosts },
    @{ n = 'empty_1080';        size = '1920x1080'; env = 'HUB_SHOT_FAKE_LIB=0'; hub = $noGhosts },
    @{ n = 'empty_wheel_s150';  size = '1024x600';  env = 'HUB_SHOT_FAKE_LIB=0;HUB_UI_SCALE=150;HUB_SHOT_WHEEL=512,420,6'; hub = $noGhosts },
    @{ n = 'empty_pad_s150';    size = '1024x600';  env = 'HUB_SHOT_FAKE_LIB=0;HUB_UI_SCALE=150;HUB_SHOT_PAD=down,down,shot:d2,rt,shot:rt1,lt'; hub = $noGhosts },
    # 4. The Cases menu (Start opens it)
    @{ n = 'casesmenu_720';     size = '1280x720';  env = $cases },
    @{ n = 'casesmenu_1080';    size = '1920x1080'; env = $cases },
    @{ n = 'casesmenu_600_s150'; size = '1024x600'; env = "$cases;HUB_UI_SCALE=150" },
    @{ n = 'casesmenu_wheel_s150'; size = '1024x600'; env = "$cases;HUB_UI_SCALE=150;HUB_SHOT_WHEEL=512,300,6" },
    @{ n = 'casesmenu_pad_s150'; size = '1024x600'; env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_FAKE_ART=1;HUB_SHOT_CASES=browse;HUB_UI_SCALE=150;HUB_SHOT_PAD=start,shot:open,rt,shot:rt1,lt' },
    @{ n = 'casesmenu_short_s200'; size = '1024x600'; env = "$cases;HUB_UI_SCALE=200" },
    @{ n = 'casesmenu_short_wheel'; size = '1024x600'; env = "$cases;HUB_UI_SCALE=200;HUB_SHOT_WHEEL=980,300,6" },
    @{ n = 'casesmenu_short_pad'; size = '1024x600'; env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_FAKE_ART=1;HUB_SHOT_CASES=browse;HUB_UI_SCALE=200;HUB_SHOT_PAD=start,shot:open,down,down,down,down,shot:d4,rt,shot:rt1,lt' },
    # 5. Play Online: every friends' room is listed (HUB_SHOT_FAKE_ROOMS adds made-up ones)
    @{ n = 'online_720';        size = '1280x720';  env = $rooms },
    @{ n = 'online_600_s150';   size = '1024x600';  env = "$rooms;HUB_UI_SCALE=150" },
    @{ n = 'online_wheel';      size = '1280x720';  env = "$rooms;HUB_SHOT_WHEEL=640,450,10" },
    @{ n = 'online_pad';        size = '1280x720';  env = "$rooms;HUB_SHOT_PAD=down,rt,shot:rt1,rt,shot:rt2,lt" },
    @{ n = 'online_fits_1080';  size = '1920x1080'; env = 'HUB_SHOT_PAGE=online;HUB_SHOT_FAKE_LIB=6' },
    # 6. Discover: the filters stay above the cards
    @{ n = 'disc_600';          size = '1024x600';  env = 'HUB_SHOT_PAGE=discover;HUB_SHOT_FAKE_LIB=6' },
    @{ n = 'disc_720';          size = '1280x720';  env = 'HUB_SHOT_PAGE=discover;HUB_SHOT_FAKE_LIB=6' },
    @{ n = 'disc_1080';         size = '1920x1080'; env = 'HUB_SHOT_PAGE=discover;HUB_SHOT_FAKE_LIB=6' },
    @{ n = 'disc_600_s150';     size = '1024x600';  env = 'HUB_SHOT_PAGE=discover;HUB_SHOT_FAKE_LIB=6;HUB_UI_SCALE=150' },
    @{ n = 'disc_wheel';        size = '1280x720';  env = 'HUB_SHOT_PAGE=discover;HUB_SHOT_FAKE_LIB=6;HUB_SHOT_WHEEL=640,560,5' },
    @{ n = 'disc_pad';          size = '1280x720';  env = 'HUB_SHOT_PAGE=discover;HUB_SHOT_FAKE_LIB=6;HUB_SHOT_PAD=down,rt,shot:rt1,rt,shot:rt2,lt' },
    # 7. The other pages
    @{ n = 'saves_600';         size = '1024x600';  env = 'HUB_SHOT_PAGE=saves;HUB_SHOT_FAKE_LIB=6' },
    @{ n = 'saves_600_s150';    size = '1024x600';  env = 'HUB_SHOT_PAGE=saves;HUB_SHOT_FAKE_LIB=6;HUB_UI_SCALE=150' },
    @{ n = 'saves_wheel_s150';  size = '1024x600';  env = 'HUB_SHOT_PAGE=saves;HUB_SHOT_FAKE_LIB=6;HUB_UI_SCALE=150;HUB_SHOT_WHEEL=500,400,6' },
    @{ n = 'saves_pad_s150';    size = '1024x600';  env = 'HUB_SHOT_PAGE=saves;HUB_SHOT_FAKE_LIB=6;HUB_UI_SCALE=150;HUB_SHOT_PAD=down,rt,shot:rt1,rt,shot:rt2,lt' },
    @{ n = 'ctl_600';           size = '1024x600';  env = 'HUB_SHOT_PAGE=controllers;HUB_SHOT_FAKE_LIB=6;HUB_SHOT_FAKE_PADS=2' },
    @{ n = 'ctl_600_s150';      size = '1024x600';  env = 'HUB_SHOT_PAGE=controllers;HUB_SHOT_FAKE_LIB=6;HUB_SHOT_FAKE_PADS=2;HUB_UI_SCALE=150' },
    @{ n = 'ctl_wheel';         size = '1024x600';  env = 'HUB_SHOT_PAGE=controllers;HUB_SHOT_FAKE_LIB=6;HUB_SHOT_FAKE_PADS=2;HUB_SHOT_WHEEL=500,400,6' },
    @{ n = 'ctl_pad';           size = '1024x600';  env = 'HUB_SHOT_PAGE=controllers;HUB_SHOT_FAKE_LIB=6;HUB_SHOT_FAKE_PADS=2;HUB_SHOT_PAD=down,rt,shot:rt1,rt,shot:rt2,lt' },
    @{ n = 'set_600';           size = '1024x600';  env = 'HUB_SHOT_PAGE=settings:general;HUB_SHOT_FAKE_LIB=6' },
    @{ n = 'set_600_s150';      size = '1024x600';  env = 'HUB_SHOT_PAGE=settings:general;HUB_SHOT_FAKE_LIB=6;HUB_UI_SCALE=150' },
    @{ n = 'set_video_600';     size = '1024x600';  env = 'HUB_SHOT_PAGE=settings:video;HUB_SHOT_FAKE_LIB=6' },
    @{ n = 'set_wheel';         size = '1024x600';  env = 'HUB_SHOT_PAGE=settings:general;HUB_SHOT_FAKE_LIB=6;HUB_SHOT_WHEEL=500,400,6' },
    @{ n = 'set_pad';           size = '1024x600';  env = 'HUB_SHOT_PAGE=settings:general;HUB_SHOT_FAKE_LIB=6;HUB_SHOT_PAD=down,rt,shot:rt1,rt,shot:rt2,lt' },
    @{ n = 'about_600_s150';    size = '1024x600';  env = 'HUB_SHOT_PAGE=about;HUB_SHOT_FAKE_LIB=6;HUB_UI_SCALE=150' },
    @{ n = 'about_wheel_s150';  size = '1024x600';  env = 'HUB_SHOT_PAGE=about;HUB_SHOT_FAKE_LIB=6;HUB_UI_SCALE=150;HUB_SHOT_WHEEL=500,400,6' },
    @{ n = 'about_pad_s150';    size = '1024x600';  env = 'HUB_SHOT_PAGE=about;HUB_SHOT_FAKE_LIB=6;HUB_UI_SCALE=150;HUB_SHOT_PAD=down,rt,shot:rt1,lt' },
    @{ n = 'about_fits_1080';   size = '1920x1080'; env = 'HUB_SHOT_PAGE=about;HUB_SHOT_FAKE_LIB=6' },
    @{ n = 'set_fits_1080';     size = '1920x1080'; env = 'HUB_SHOT_PAGE=settings:online;HUB_SHOT_FAKE_LIB=6' }
)
$fail = 0
foreach ($r in $runs) {
    if ($r.n -notmatch $Only) { continue }
    if (Test-OwnerGaming) { Write-Output 'A game started: stopping.'; break }
    if (Test-Path "$box\userdata") { Remove-Item -Recurse -Force "$box\userdata" }
    $vault = Join-Path $box 'vault'
    if (Test-Path $vault) { Remove-Item -Recurse -Force $vault }
    New-Item -ItemType Directory -Force $vault | Out-Null
    if ($r.hub) { New-Item -ItemType Directory -Force "$box\userdata" | Out-Null; [IO.File]::WriteAllText("$box\userdata\hub.json", $r.hub) }   # settings for this run only
    Get-ChildItem $Out -Filter "$($r.n)*.png" -ErrorAction SilentlyContinue | Where-Object { $_.BaseName -eq $r.n -or $_.BaseName -like "$($r.n)-*" } | Remove-Item -Force
    $bmp = Join-Path $Out "$($r.n).bmp"
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $box 'RetroHub.exe'
    $psi.WorkingDirectory = $box
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    foreach ($kv in @("HUB_SCREENSHOT=$bmp", "HUB_SHOT_SIZE=$($r.size)", 'HUB_SHOT_SECS=5', "HUB_SHOT_VAULT=$vault",
                      'RETROHUB_QA=1', 'SSB64_MUTE=1', 'RETRORUN_MUTE=1', 'N64RUN_MUTE=1', 'RETROHUB_NO_CLOUD=1') + ($r.env -split ';')) {
        if ($kv -match '^([^=]+)=(.*)$') { $psi.EnvironmentVariables[$Matches[1]] = $Matches[2] }
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force; Write-Output "$($r.n): TIMEOUT"; $fail++; continue }
    $made = Get-ChildItem $Out -Filter "$($r.n)*.bmp" | Where-Object { $_.BaseName -eq $r.n -or $_.BaseName -like "$($r.n)-*" }
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
