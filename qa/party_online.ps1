# Party mode online QA: N RetroHub instances (host + guests) in one room on a private hidden desktop, LOCAL wrangler only (_guard.ps1).
# Driven by RETROHUB_AUTOTEST + RETROHUB_AUTOTEST_PARTY (src/hub/party_ui.cpp partyui_qa_drive): the host turns Party mode on, plays a
# timed playlist, everyone votes "who won?" after each timer round; every PC logs "party-view ..." lines to userdata\autotest.log.
# Checks: standings identical on every PC after each round, READY reset + auto-ready between rounds, night over on every PC.
# usage: party_online.ps1 -Build <bin\Release> -Work <scratch> -Games nes:xxxx,snes:yyyy [-Players 2] [-Rounds 2] [-Secs 25] [-Server ws://127.0.0.1:8762]
# Needs deps (cores + testroms: robocopy ..\..\RetroHub\deps deps /E) and `npx wrangler dev --ip 127.0.0.1 --port 8762 --persist-to <short dir>`.
# NOT yet run end to end in this lane (see the lane report); the room messages are covered by test_party.cpp.
param([Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$Work, [Parameter(Mandatory)][string]$Games, [string]$RomDir = "",
      [int]$Players = 2, [int]$Rounds = 2, [int]$Secs = 25, [string]$Server = "ws://127.0.0.1:8762", [string]$Desktop = "rhparty")
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class PDesk {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct SI { public int cb; public string r, desk, title; public int x,y,xs,ys,xc,yc,fill,flags; public short show, res2; public IntPtr r2,i,o,e; }
  [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr hp, ht; public int pid, tid; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateDesktop(string n, IntPtr d, IntPtr dm, int f, uint a, IntPtr sa);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inh, uint fl, IntPtr env, string cwd, ref SI si, out PI pi);
  public static void Desk(string n) { CreateDesktop(n, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero); }
  public static int Start(string exe, string cwd, string desk) { SI si = new SI(); si.cb = Marshal.SizeOf(si); si.desk = desk; PI pi;
    if (!CreateProcess(null, "\"" + exe + "\"", IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, cwd, ref si, out pi)) throw new Exception("CreateProcess " + Marshal.GetLastWin32Error()); return pi.pid; }
}
"@
$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'.ToCharArray(); $Code = "QA" + (-join (1..4 | ForEach-Object { $alpha | Get-Random }))
New-Item -ItemType Directory -Force $Work | Out-Null
[PDesk]::Desk($Desktop)
$pids = @()
for ($i = 0; $i -lt $Players; $i++) {
  $d = Join-Path $Work "inst$i"; if (Test-Path $d) { Remove-Item -Recurse -Force $d }
  New-Item -ItemType Directory -Force "$d\userdata" | Out-Null
  Copy-Item "$Build\*" $d -Recurse -Include *.exe, *.dll, cores, data, emu -Exclude hub_*.exe
  $v = ($d -replace '\\', '/'); $rom = if ($RomDir) { ($RomDir -replace '\\', '/') } else { "$v/roms" }
  Set-Content "$d\userdata\hub.json" ('{"name":"qa-Party' + $i + '","rom_folders":["' + $rom + '"],"server":"' + $Server + '","vault_dir":"' + $v + '/vault","sync_auto":false,"cloud_saves":false}') -Encoding ascii
  $env:RETROHUB_AUTOTEST = if ($i -eq 0) { "host" } else { "join" }; $env:RETROHUB_AUTOTEST_CODE = $Code; $env:RETROHUB_AUTOTEST_NAME = "qa-Party$i"
  $env:RETROHUB_AUTOTEST_PARTY = if ($i -eq 0) { "host" } else { "guest" }; $env:RETROHUB_AUTOTEST_PLAYERS = "99"   # autotest.cpp never launches by itself: the party driver does
  $env:RETROHUB_PARTY_GAMES = $Games; $env:RETROHUB_PARTY_ROUNDS = "$Rounds"; $env:RETROHUB_PARTY_SECS = "$Secs"; $env:RETROHUB_PARTY_PLAYERS = "$Players"
  $env:RETROHUB_ALLOW_VERSION_MISMATCH = "1"; $env:RETROHUB_QA = "1"; $env:HUBNET_LOOPBACK = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"; $env:SSB64_MUTE = "1"
  $pids += [PDesk]::Start("$d\RetroHub.exe", $d, $Desktop); Write-Host "started inst$i pid $($pids[-1]) room $Code"
  Start-Sleep ($(if ($i -eq 0) { 8 } else { 5 }))
}
$end = (Get-Date).AddSeconds(90 + $Rounds * ($Secs + 60))
while ((Get-Date) -lt $end) { Start-Sleep 5; if ((Get-Content "$Work\inst0\userdata\autotest.log" -Raw -ErrorAction SilentlyContinue) -match "night over") { break } }
Start-Sleep 5
$fail = 0
$views = @{}
for ($i = 0; $i -lt $Players; $i++) {
  $log = Get-Content "$Work\inst$i\userdata\autotest.log" -ErrorAction SilentlyContinue
  $views[$i] = @($log | Where-Object { $_ -match "party-view" } | ForEach-Object { ($_ -replace '^\[\s*[\d.]+s\]\s*', '') -replace 'rev=\d+ ', '' })
  Write-Host "inst$i : $($views[$i].Count) standings lines, auto-ready=$(@($log | Where-Object { $_ -match 'auto-ready' }).Count), closing=$(@($log | Where-Object { $_ -match 'closing the game' }).Count)"
}
for ($i = 1; $i -lt $Players; $i++) { if (($views[0] -join "`n") -ne ($views[$i] -join "`n")) { Write-Host "FAIL standings differ between inst0 and inst$i"; $fail++ } }
if ($views[0].Count -lt $Rounds + 1) { Write-Host "FAIL expected $($Rounds + 1) standings views (rounds + the podium), got $($views[0].Count)"; $fail++ }
foreach ($p in $pids) { Stop-Process -Id $p -Force -ErrorAction SilentlyContinue }   # our own pids only
if ($fail) { Write-Host "PARTY ONLINE QA: FAIL ($fail)"; exit 1 }
Write-Host "PARTY ONLINE QA: PASS"; exit 0
