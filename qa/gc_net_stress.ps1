# GameCube (Mario Kart: Double Dash) online reliability harness. Runs the REAL RetroHub room flow (host + guest RetroHub instances, own data dirs,
# RETROHUB_AUTOTEST) N times per mode against a LOCAL wrangler dev server, all on a private invisible desktop, muted, loopback only.
# Per run it records: did both Dolphins reach NetPlay "game started" (driver status stage:in-game = RenderWidget up), time to start, error text,
# and (after start) a hold period of -HoldSeconds watching for Dolphin desync reports and for either Dolphin dying.
# Modes: direct | relay | sim | simrelay | gfirst | glate | slowhost   (see $ModeDefs)
#   sim / simrelay  HUBNET_SIM_ENABLE=1 + latency/jitter/loss on every outgoing forwarder datagram (src/net/session.cpp simulator; forwarders ARE hubnet::Session)
#   gfirst          guest RetroHub is started 1 s after the host (guest ready first, host launches the instant the room is full)
#   glate           guest RetroHub is started 25 s after the host (host sits waiting)
#   slowhost        the host's Dolphin.exe is suspended for -SlowHostSeconds right after it appears (slow Dolphin start injected from the harness)
# NOTE pad input: Dolphin reads keyboard/pad through global input state that cannot be injected across desktops, and the hidden desktop is the
# rule, so there is no scripted pad input; the game idles in the boot/logo/title/attract loop (the CPU, GX and the netplay pad stream run
# every frame regardless, so timing desyncs and connection drops still show). This is stated in the report.
# usage: gc_net_stress.ps1 -Build <bin\Release> -DolphinDir <dolphin dir> -DiscDir <dir with the .ciso/.iso> [-Runs 20] [-Modes direct,relay,...]
#        [-HoldSeconds 60] [-Port 11600] [-Work C:\rhq\stress] [-Lane 0] [-Out results.jsonl]
param(
  [Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$DolphinDir, [Parameter(Mandatory)][string]$DiscDir,
  [string]$Game = "mkdd", [int]$Runs = 20, [string[]]$Modes = @("direct","relay","sim","simrelay","gfirst","glate","slowhost"),
  [int]$HoldSeconds = 60, [int]$StartTimeout = 150, [int]$SlowHostSeconds = 15,
  [int]$SimLatency = 80, [int]$SimJitter = 40, [int]$SimLoss = 3,
  [int]$Port = 11600, [string]$Work = "C:\rhq\stress", [int]$Lane = 0, [string]$Out = "", [switch]$NoServer,
  [string]$WorkerDir = "C:\Users\fatty\Desktop\SmashClassicPort\BattleShip\server\cloudflare", [string]$Tag = ""
)
$Modes = @($Modes | ForEach-Object { $_ -split ',' })
$Server = "ws://127.0.0.1:$Port"
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$ErrorActionPreference = "Stop"
$Work = "$Work$Lane"
$Desktop = "rhgc_stress$Lane"
if (-not $Out) { $Out = Join-Path $Work "results.jsonl" }
New-Item -ItemType Directory -Force $Work | Out-Null
Add-Type -TypeDefinition @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class DeskLaunch {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct SI { public int cb; public string r, desk, title; public int x,y,xs,ys,xc,yc,fill,flags; public short show, res2; public IntPtr r2,i,o,e; }
  [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr hp, ht; public int pid, tid; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateDesktop(string n, IntPtr d, IntPtr dm, int f, uint a, IntPtr sa);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inh, uint fl, IntPtr env, string cwd, ref SI si, out PI pi);
  public static void Desk(string n) { CreateDesktop(n, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero); }
  public static int Start(string exe, string cwd, string desk) {
    SI si = new SI(); si.cb = Marshal.SizeOf(si); si.desk = desk; PI pi;
    if (!CreateProcess(null, "\"" + exe + "\"", IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, cwd, ref si, out pi)) throw new Exception("CreateProcess " + Marshal.GetLastWin32Error());
    return pi.pid;
  }
}
public static class WinList {
  delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr OpenDesktop(string n, int f, bool i, uint a);
  [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr d, EP p, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  public static string Of(string desk, int pid) {
    IntPtr d = OpenDesktop(desk, 0, false, 0x1FF); var r = new List<string>();
    EnumDesktopWindows(d, (h, l) => { int p; GetWindowThreadProcessId(h, out p); if (p == pid) { var t = new StringBuilder(200); var c = new StringBuilder(100); GetWindowText(h, t, 200); GetClassName(h, c, 100); r.Add((IsWindowVisible(h) ? "V " : "h ") + c + " '" + t + "'"); } return true; }, IntPtr.Zero);
    return string.Join("; ", r);
  }
}
public static class ProcSuspend {
  [DllImport("ntdll.dll")] static extern int NtSuspendProcess(IntPtr h);
  [DllImport("ntdll.dll")] static extern int NtResumeProcess(IntPtr h);
  [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(int a, bool i, int pid);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  public static void Suspend(int pid) { IntPtr h = OpenProcess(0x800, false, pid); if (h != IntPtr.Zero) { NtSuspendProcess(h); CloseHandle(h); } }
  public static void Resume(int pid) { IntPtr h = OpenProcess(0x800, false, pid); if (h != IntPtr.Zero) { NtResumeProcess(h); CloseHandle(h); } }
}
"@
[DeskLaunch]::Desk($Desktop)

# ---- local server (own pids only; isolated wrangler registry; short persist dir) ----
$srvPid = $null
if (-not $NoServer) {
  $env:WRANGLER_REGISTRY_PATH = "C:\rhq\reg$Port"; New-Item -ItemType Directory -Force $env:WRANGLER_REGISTRY_PATH | Out-Null
  $persist = "C:\rhq\s$Port"; if (Test-Path $persist) { Remove-Item -Recurse -Force $persist }
  $psi = New-Object System.Diagnostics.ProcessStartInfo; $psi.FileName = (Get-Command node).Source; $psi.WorkingDirectory = $WorkerDir
  $psi.Arguments = "node_modules\wrangler/bin\wrangler.js dev --ip 127.0.0.1 --port $Port --persist-to $persist"
  $psi.CreateNoWindow = $true; $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
  $wr = [System.Diagnostics.Process]::Start($psi)   # no console window; drain the pipes so wrangler never blocks
  $null = $wr.StandardOutput.BaseStream.CopyToAsync([System.IO.Stream]::Null); $null = $wr.StandardError.BaseStream.CopyToAsync([System.IO.Stream]::Null)
  $srvPid = $wr.Id
  $ok = $false
  for ($i = 0; $i -lt 90; $i++) { Start-Sleep 1; try { $c = New-Object Net.Sockets.TcpClient; $c.Connect("127.0.0.1", $Port); $c.Close(); $ok = $true; break } catch {} }
  if (-not $ok) { Stop-Process -Id $srvPid -Force -ErrorAction SilentlyContinue; throw "wrangler did not come up on $Port" }
  Write-Host "local wrangler pid $srvPid on $Port"
}

# ---- one-time Dolphin template with NetPlay logging on (desync reports land in User\Logs\dolphin.log) ----
$tmpl = "$Work\dolphin_template"
if (Test-Path $tmpl) { Remove-Item -Recurse -Force $tmpl }
robocopy $DolphinDir $tmpl /E /XD _downloads User /NFL /NDL /NJH /NJS /NP | Out-Null
New-Item -ItemType Directory -Force "$tmpl\User\Config" | Out-Null
Set-Content "$tmpl\retrohub-install.txt" "autotest" -Encoding ascii
Set-Content "$tmpl\User\Config\Logger.ini" "[Options]`r`nVerbosity = 4`r`nWriteToFile = True`r`nWriteToConsole = False`r`nWriteToWindow = False`r`n[Logs]`r`nNETPLAY = True`r`nCOMMON = True`r`nCORE = True`r`nBOOT = True`r`nOSREPORT = True`r`nSERIALINTERFACE = True`r`nEXPANSIONINTERFACE = True`r`n" -Encoding ascii

$ModeDefs = @{
  direct   = @{ relay = $false; sim = $false; gap = 6;  slow = 0 }
  relay    = @{ relay = $true;  sim = $false; gap = 6;  slow = 0 }
  sim      = @{ relay = $false; sim = $true;  gap = 6;  slow = 0 }
  simrelay = @{ relay = $true;  sim = $true;  gap = 6;  slow = 0 }
  gfirst   = @{ relay = $false; sim = $false; gap = -1;  slow = 0 }
  glate    = @{ relay = $false; sim = $false; gap = 25; slow = 0 }
  slowhost = @{ relay = $false; sim = $false; gap = 6;  slow = $SlowHostSeconds }
}
$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'.ToCharArray()
function DolphinOf($d) { Get-CimInstance Win32_Process -Filter "Name='Dolphin.exe'" | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($d, [StringComparison]::OrdinalIgnoreCase) } | Select-Object -First 1 }
function ReadAll($p) { $t = ""; if (Test-Path $p) { try { $t = [System.IO.File]::ReadAllText($p) } catch { $t = "" } }; return [string]$t }
function StatusOf($d, $role) { ReadAll "$d\userdata\gcnp\gc_${role}_status.txt" }
function KillOwn() {
  $procs = Get-CimInstance Win32_Process | Where-Object { ($_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase)) -or ($_.Name -eq "powershell.exe" -and $_.CommandLine -like "*$Work*gc_netplay_driver*") }
  foreach ($p in $procs) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
}
$descRx = '(?i)desync|out of sync|possible desync|checksum mismatch|state mismatch'

function Run-One($mode, $n) {
  $m = $ModeDefs[$mode]
  KillOwn
  $code = -join (1..6 | ForEach-Object { $alpha | Get-Random })
  $runDir = "$Work\run"
  if (Test-Path $runDir) { Remove-Item -Recurse -Force $runDir }
  $pids = @(); $t0 = Get-Date
  for ($i = 0; $i -lt 2; $i++) {
    $role = if ($i -eq 0) { "host" } else { "join" }
    $d = "$runDir\inst$i"
    New-Item -ItemType Directory -Force "$d\userdata\emulators" | Out-Null
    Copy-Item "$Build\*" $d -Recurse -Include *.exe, *.dll -Exclude hub_*.exe
    robocopy $tmpl "$d\userdata\emulators\dolphin" /E /NFL /NDL /NJH /NJS /NP | Out-Null
    $v = ($d -replace '\\', '/')
    $json = '{"name":"qa-St' + $i + '","rom_folders":["' + ($DiscDir -replace '\\', '/') + '"],"server":"' + $Server + '","vault_dir":"' + $v + '/vault","sync_auto":false,"cloud_saves":false}'
    Set-Content "$d\userdata\hub.json" $json -Encoding ascii
    $env:RETROHUB_AUTOTEST = $role; $env:RETROHUB_AUTOTEST_CODE = $code; $env:RETROHUB_AUTOTEST_NAME = "qa-St$i"
    $env:RETROHUB_AUTOTEST_GAME = $Game; $env:RETROHUB_AUTOTEST_PLAYERS = "2"
    $env:RETROHUB_ALLOW_VERSION_MISMATCH = "1"; $env:RETROHUB_QA = "1"
    Remove-Item Env:RETROHUB_AUTOTEST_WRITE -ErrorAction SilentlyContinue
    if ($m.relay) { $env:HUBNET_FORCE_RELAY = "1"; Remove-Item Env:HUBNET_LOOPBACK -ErrorAction SilentlyContinue } else { $env:HUBNET_LOOPBACK = "1"; Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue }
    if ($m.sim) { $env:HUBNET_SIM_ENABLE = "1"; $env:HUBNET_SIM_LATENCY_MS = "$SimLatency"; $env:HUBNET_SIM_JITTER_MS = "$SimJitter"; $env:HUBNET_SIM_LOSS_PCT = "$SimLoss"; $env:HUBNET_SIM_SEED = "$($n + 100)" }
    else { foreach ($k in "HUBNET_SIM_ENABLE","HUBNET_SIM_LATENCY_MS","HUBNET_SIM_JITTER_MS","HUBNET_SIM_LOSS_PCT","HUBNET_SIM_SEED") { Remove-Item "Env:$k" -ErrorAction SilentlyContinue } }
    $env:RETRORUN_MUTE = "1"; $env:SSB64_MUTE = "1"
    $pids += [DeskLaunch]::Start("$d\RetroHub.exe", $d, $Desktop)
    if ($i -eq 0) { Start-Sleep ($m.gap + 2) }   # host-to-guest gap: 8 s default; gfirst 1 s (the guest retries until the host's room exists, so it is ready first); glate 27 s
  }
  $h = "$runDir\inst0"; $g = "$runDir\inst1"
  $res = [ordered]@{ mode = $mode; n = $n; code = $code; started = $false; start_s = $null; held = $false; hold_s = 0; desync = $false; reason = ""; detail = ""; host_stage = ""; guest_stage = "" }
  $startAt = $null; $suspended = $null; $slowDone = ($m.slow -le 0); $holdEnd = $null
  $end = (Get-Date).AddSeconds($StartTimeout + $HoldSeconds + 30)
  while ((Get-Date) -lt $end) {
    Start-Sleep -Milliseconds 1500
    if (-not $slowDone) {
      $hd = DolphinOf $h
      if ($hd -and -not $suspended) { [ProcSuspend]::Suspend($hd.ProcessId); $suspended = @{ pid = $hd.ProcessId; at = Get-Date }; Write-Host "  slowhost: suspended host Dolphin $($hd.ProcessId)" }
      elseif ($suspended -and ((Get-Date) - $suspended.at).TotalSeconds -ge $m.slow) { [ProcSuspend]::Resume($suspended.pid); $slowDone = $true }
    }
    $hs = StatusOf $h "host"; $gs = StatusOf $g "guest"
    if (-not $startAt) {
      if ($hs -match "stage:in-game" -and $gs -match "stage:in-game") { $startAt = Get-Date; $res.started = $true; $res.start_s = [math]::Round(($startAt - $t0).TotalSeconds, 1); $holdEnd = $startAt.AddSeconds($HoldSeconds) }
      elseif ($hs -match "(?m)^error:" -or $gs -match "(?m)^error:") { break }
      elseif (((Get-Date) - $t0).TotalSeconds -gt $StartTimeout) { break }
    } else {
      $hd = DolphinOf $h; $gd = DolphinOf $g
      if ($hs -match $descRx -or $gs -match $descRx) { $res.desync = $true }
      if (-not $hd -or -not $gd) { $res.reason = "died-during-hold"; $res.detail = "host dolphin alive=$([bool]$hd) guest dolphin alive=$([bool]$gd) after $([int]((Get-Date)-$startAt).TotalSeconds)s"; break }
      $res.hold_s = [int]((Get-Date) - $startAt).TotalSeconds
      if ((Get-Date) -ge $holdEnd) { $res.held = $true; break }
    }
  }
  $hs = StatusOf $h "host"; $gs = StatusOf $g "guest"
  $res.host_stage = (([regex]::Matches($hs, '(?m)^stage:\S+') | ForEach-Object { $_.Value.Substring(6) }) -join ">")
  $res.guest_stage = (([regex]::Matches($gs, '(?m)^stage:\S+') | ForEach-Object { $_.Value.Substring(6) }) -join ">")
  $hl = ReadAll "$h\userdata\autotest.log"; $gl = ReadAll "$g\userdata\autotest.log"
  $hdl = ReadAll "$h\userdata\emulators\dolphin\User\Logs\dolphin.log"; $gdl = ReadAll "$g\userdata\emulators\dolphin\User\Logs\dolphin.log"
  if (($hdl -match $descRx) -or ($gdl -match $descRx)) { $res.desync = $true; $res.detail += " dolphin.log: " + ((($hdl + "`n" + $gdl) -split "`n" | Where-Object { $_ -match $descRx } | Select-Object -First 2) -join " | ") }
  if ($hs -match $descRx -or $gs -match $descRx) { $res.desync = $true }
  # buffer / lag evidence from the dolphin NetPlay logs (informational)
  $res.pad_warn = ((($hdl + "`n" + $gdl) -split "`n" | Where-Object { $_ -match '(?i)pad buffer|buffer (under|over)|too many pad' }).Count)
  if (-not $res.reason) {
    if (-not $res.started) {
      $errs = @(); foreach ($s in @($hs, $gs)) { $errs += ([regex]::Matches($s, '(?m)^error:.*') | ForEach-Object { $_.Value }) }
      $bad = @(); foreach ($l in @($hl, $gl)) { $bad += ($l -split "`n" | Where-Object { $_ -match '(?i)fail|host left|forwarder lost|error|warn' } | Select-Object -First 3) }
      $wins = ""
      foreach ($dd in @($h, $g)) { $dp = DolphinOf $dd; if ($dp) { $wins += " [dolphin " + (Split-Path $dd -Leaf) + ": " + [WinList]::Of($Desktop, $dp.ProcessId) + "]" } }
      $res.detail = (($errs + $bad) -join " ; ").Trim() + $wins
      if ($errs.Count) { $res.reason = ($errs[0] -replace '^error:', '').Trim() }
      elseif ($hl -notmatch "gc_begin host") { $res.reason = "host never launched Dolphin (room/ready flow)" }
      elseif ($gl -notmatch "gc_begin guest") { $res.reason = "guest never launched Dolphin" }
      elseif ($gs -notmatch "stage:joined") { $res.reason = "guest Dolphin never joined NetPlay" }
      else { $res.reason = "timeout before both in-game" }
    } elseif ($res.desync) { $res.reason = "desync reported" }
    elseif (-not $res.held) { $res.reason = "hold cut short" }
  }
  if (-not $res.started -or $res.reason -or $res.desync) {   # keep evidence for failures
    $keep = "$Work\fail\$mode-$n"; New-Item -ItemType Directory -Force $keep | Out-Null
    foreach ($x in @(@("inst0","host"),@("inst1","guest"))) { $dd = "$runDir\$($x[0])"
      Copy-Item "$dd\userdata\autotest.log" "$keep\$($x[0]).autotest.log" -ErrorAction SilentlyContinue
      Copy-Item "$dd\userdata\gcnp\gc_$($x[1])_status.txt" "$keep\$($x[0]).status.txt" -ErrorAction SilentlyContinue
      Copy-Item "$dd\userdata\emulators\dolphin\User\Logs\dolphin.log" "$keep\$($x[0]).dolphin.log" -ErrorAction SilentlyContinue }
  }
  if ($suspended -and -not $slowDone) { [ProcSuspend]::Resume($suspended.pid) }
  KillOwn
  foreach ($p in $pids) { Stop-Process -Id $p -Force -ErrorAction SilentlyContinue }
  Start-Sleep 2
  return $res
}

try {
  foreach ($mode in $Modes) {
    for ($n = 1; $n -le $Runs; $n++) {
      try { $r = Run-One $mode $n } catch { $r = [ordered]@{ mode = $mode; n = $n; started = $false; held = $false; desync = $false; start_s = $null; reason = "harness exception: $($_.Exception.Message) @ $($_.InvocationInfo.ScriptLineNumber)"; detail = "" }; KillOwn }
      $r["tag"] = $Tag
      $ok = $r.started -and $r.held -and -not $r.desync -and -not $r.reason
      $r["pass"] = [bool]$ok
      ($r | ConvertTo-Json -Compress) | Add-Content $Out -Encoding utf8
      Write-Host ("{0,-9} {1,2}/{2}  {3}  start={4}s  {5}" -f $mode, $n, $Runs, $(if ($ok) { "PASS" } else { "FAIL" }), $r.start_s, $r.reason)
    }
  }
} finally {
  KillOwn
  if ($srvPid) { & taskkill /PID $srvPid /T /F | Out-Null }   # own wrangler tree only
}
