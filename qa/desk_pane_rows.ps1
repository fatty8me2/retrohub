# How many whole option rows the Library desk pane shows on a game's Settings tab, with the setup banner up (game_page_layout.h
# pane_short / pane_banner_slim): Ocarina of Time (Switch 2), the pane at 1280x720 and 1024x600 (100 %), 1280x720 at 150 % and
# 1920x1080. The hub's "gs-rows <game> <n>" trace line (settings_games_ui.cpp: option rows wholly inside the scroll area) gives the count.
# Checks: at least 3 rows at 1280x720 150 %, at least 5 at 1024x600 100 %; with -Ref <older bin\Release>, the 1280x720 100 % and
# 1920x1080 shots must be pixel-identical to that build's.
# HEADLESS and muted, from a SANDBOX COPY (a fresh userdata: setup not finished, so the setup banner shows), a scratch vault; waits while the owner
# is gaming. HUB_REDUCE_MOTION=1: the focus ring on Play does not pulse, so two builds can be compared pixel for pixel.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File qa\desk_pane_rows.ps1 [-Bin <bin\Release>] [-Ref <bin\Release>] [-Out dir]
param([string]$Bin = '', [string]$Ref = '', [string]$Out = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. (Join-Path $PSScriptRoot '_gamecheck.ps1')
if (-not $Bin) { $Bin = Join-Path $root 'build\bin\Release' }
if (-not $Out) { $Out = Join-Path $root 'build\qa_pane_rows_out' }
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Drawing; using System.Drawing.Imaging;
public static class PaneRowsPix {
    public static long Diff(string a, string b) {   // differing pixels; -1 = different sizes
        using (var x = new Bitmap(a)) using (var y = new Bitmap(b)) {
            if (x.Width != y.Width || x.Height != y.Height) return -1;
            var r = new Rectangle(0, 0, x.Width, x.Height);
            var dx = x.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb); var dy = y.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            long n = 0; int len = dx.Stride * x.Height; var bx = new byte[len]; var by = new byte[len];
            System.Runtime.InteropServices.Marshal.Copy(dx.Scan0, bx, 0, len); System.Runtime.InteropServices.Marshal.Copy(dy.Scan0, by, 0, len);
            x.UnlockBits(dx); y.UnlockBits(dy);
            for (int i = 0; i < len; i += 4) if (bx[i] != by[i] || bx[i + 1] != by[i + 1] || bx[i + 2] != by[i + 2]) n++;
            return n;
        }
    }
}
'@
$sizes = @(@('1280', '1280x720', ''), @('1024', '1024x600', ''), @('s150-1280', '1280x720', '150'), @('1920', '1920x1080', ''))
function Shoot([string]$bin, [string]$tag) {
    $box = Join-Path $root "build\qa_pane_rows_$tag"
    Wait-NoGame
    & robocopy $bin $box /MIR /XD userdata vault /NFL /NDL /NJH /NJS /NP | Out-Null
    $res = @{}
    foreach ($s in $sizes) {
        Wait-NoGame
        foreach ($d in 'userdata', 'vault') { Remove-Item -Recurse -Force (Join-Path $box $d) -ErrorAction SilentlyContinue }
        New-Item -ItemType Directory -Force (Join-Path $box 'vault\_account'), (Join-Path $box 'userdata') | Out-Null
        Set-Content -Encoding ascii (Join-Path $box 'userdata\hub.json') '{"show_friend_games": false, "friend_games_v": 1}'   # no greyed pack games (their listing lands at a random moment); setup not finished: the banner shows
        $name = "$tag-$($s[0])"
        $trace = Join-Path $Out "$name.trace"; Remove-Item $trace, (Join-Path $Out "$name.bmp") -ErrorAction SilentlyContinue
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = Join-Path $box 'RetroHub.exe'; $psi.WorkingDirectory = $box; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
        $kv = @("HUB_SCREENSHOT=$Out\$name.bmp", "HUB_SHOT_SIZE=$($s[1])", 'HUB_SHOT_SECS=5', "HUB_SHOT_VAULT=$box\vault", "HUB_SHOT_TRACE=$trace", 'HUB_SHOT_LAYOUT=desk',
                'HUB_SHOT_FAKE_LIB=1', 'HUB_SHOT_HOOKSHOT=idle', 'HUB_SHOT_TAB=3', 'HUB_REDUCE_MOTION=1', 'RETROHUB_QA=1', 'SSB64_MUTE=1', 'RETRORUN_MUTE=1', 'N64RUN_MUTE=1', 'RETROHUB_NO_CLOUD=1')
        if ($s[2]) { $kv += "HUB_UI_SCALE=$($s[2])" }
        foreach ($x in $kv) { if ($x -match '^([^=]+)=(.*)$') { $psi.EnvironmentVariables[$Matches[1]] = $Matches[2] } }
        $p = [System.Diagnostics.Process]::Start($psi)
        if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id -Force }   # only the PID started here
        $lines = if (Test-Path $trace) { Get-Content $trace } else { @() }
        $last = $lines | Where-Object { $_ -like 'gs-rows *' } | Select-Object -Last 1
        $vis = $lines | Where-Object { $_ -like 'gs-visible *' } | Select-Object -Last 1
        $res[$s[0]] = @{ rows = $(if ($last -match ' (\d+)$') { [int]$Matches[1] } else { -1 }); vis = $(if ($vis -match ' (\d+)$') { [int]$Matches[1] } else { -1 }); bmp = (Join-Path $Out "$name.bmp"); code = $p.ExitCode
                         banner = [bool]($lines | Where-Object { $_ -like 'desk-banner*' }) }
        $img = [System.Drawing.Image]::FromFile($res[$s[0]].bmp); $img.Save((Join-Path $Out "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose()
    }
    Remove-Item -Recurse -Force $box -ErrorAction SilentlyContinue
    return $res
}
$now = Shoot $Bin 'new'
$old = if ($Ref) { Shoot $Ref 'ref' } else { $null }
$fail = 0
foreach ($s in $sizes) {
    $k = $s[0]; $n = $now[$k]
    $line = "{0,-10} rows {1,2} (rows + headings {2,2})  banner {3}  exit {4}" -f $k, $n.rows, $n.vis, $n.banner, $n.code
    if ($old) { $line += "  | ref: rows {0,2} (rows + headings {1,2})" -f $old[$k].rows, $old[$k].vis }
    $bad = @()
    if ($n.code -ne 0) { $bad += "exit $($n.code)" }
    if (-not $n.banner) { $bad += 'no setup banner' }
    if ($k -eq 's150-1280' -and $n.rows -lt 3) { $bad += 'fewer than 3 whole rows' }
    if ($k -eq '1024' -and $n.rows -lt 5) { $bad += 'fewer than 5 whole rows' }
    if ($old -and $k -in '1280', '1920') {
        $d = [PaneRowsPix]::Diff($n.bmp, $old[$k].bmp)
        $line += "  pixels differing from ref: $d"
        if ($d -ne 0) { $bad += 'not pixel-identical to the reference build' }
    }
    if ($bad.Count) { $fail++; $line += "  FAIL: " + ($bad -join '; ') } else { $line += '  ok' }
    Write-Output $line
}
Get-ChildItem $Out -Filter *.bmp | Remove-Item -Force
Write-Output "PNGs in $Out; $fail failure(s)"
exit ([int]($fail -gt 0))
