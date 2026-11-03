# GameCube NetPlay determinism check (QA): two Dolphins in NetPlay on a hidden desktop (muted, scripted DSU pad, hub_gc_speed), with
# FRAME-ALIGNED MEM1 hashes from qa\gc_state_hash.py (Dolphin's GDB stub: a write watchpoint on the game's VI-rate counter stops both
# instances at the same instruction of the same frame) plus Dolphin's own desync detector (host status "Possible desync detected").
# Prints one line per row: detector frame (or none) and, for every common sample frame, MATCH/DIFF of host vs guest MEM1.
# The GDB stub slows Dolphin to ~25 % (it polls its socket from a CoreTiming event), so --emu-clock drives the pad script on emulated time.
# Rows are "<tag>|<mode>|<extra hub_gc_speed args>|<env>" like qa\gc_online_speed.ps1; the default pair is the Strikers fix A/B:
#   B = --card gci  (RetroHub <= 1.31.4: the card is only GCIFolderAPathOverride, Dolphin syncs an empty folder to the guests)
#   A = --card hub  (RetroHub now: gcnp::host_card_sync_args gives Dolphin's save sync a copy of the host's card)
# usage: qa\gc_desync_check.ps1 -Build build\bin\Release -Dolphin <dolphin dir> -Disc <disc> -Game G4QE01 -Gci <save dir> [-Secs 300]
#        [-Every 1800] [-Start 1800] [-Pattern seq:...] [-Rows ...] [-Work C:\rhq\gcsync]
param(
  [Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$Dolphin, [Parameter(Mandatory)][string]$Disc, [Parameter(Mandatory)][string]$Game,
  [string]$Gci = "", [int]$Secs = 300, [int]$Every = 1800, [int]$Start = 1800, [string]$Pattern = "seq:3=a,4.5=a,6=a,7.5=a,9=a,11=a",
  [string[]]$Rows = @(), [string]$Work = "C:\rhq\gcsync", [int]$GdbPort = 27650
)
$env:SSB64_MUTE = "1"; $env:N64RUN_MUTE = "1"; $env:RETRORUN_MUTE = "1"
New-Item -ItemType Directory -Force $Work | Out-Null
$vi = Join-Path $Work "$Game.vi.txt"
if (-not (Test-Path $vi)) { Write-Host "no $vi : run hub_gc_speed --mode offline --calibrate first (see qa\gc_online_speed.ps1)"; exit 2 }
$addr = (Get-Content $vi | Select-Object -First 1).Trim()
$h = "--emu-clock --hash-addr $addr --hash-every $Every --hash-first 300 --hash-start $Start --gdb-port $GdbPort --hash-dump"
if (-not $Rows -or $Rows.Count -eq 0) { $Rows = @("B|np-direct|--card gci $h|", "A|np-direct|--card hub $h|") }
& "$PSScriptRoot\gc_online_speed.ps1" -Build $Build -Dolphin $Dolphin -Disc $Disc -Game $Game -Gci $Gci -Work $Work -Secs $Secs -Warm 5 -Pattern $Pattern -Server "" `
  -Out (Join-Path $Work "desync-$Game.txt") -Rows $Rows | Where-Object { $_ -match "RUN|FAIL" }
foreach ($r in $Rows) {
  $tag = ($r -split '\|')[0] + "-$Game"; $dir = Join-Path $Work $tag
  $st = Get-Content (Join-Path $dir "host_status.txt") -Raw -ErrorAction SilentlyContinue
  $det = if ($st -match 'desynced at frame (\d+)') { "frame $($Matches[1])" } else { "none" }
  $hh = @{}; foreach ($l in (Get-Content (Join-Path $dir "host_hash.txt") -ErrorAction SilentlyContinue)) { if ($l -match '^frame (\d+) sha1 (\w+)') { $hh[$Matches[1]] = $Matches[2] } }
  $cmp = @(); foreach ($l in (Get-Content (Join-Path $dir "guest_hash.txt") -ErrorAction SilentlyContinue)) {
    if ($l -match '^frame (\d+) sha1 (\w+)' -and $hh.ContainsKey($Matches[1])) { $cmp += "$($Matches[1])=" + $(if ($hh[$Matches[1]] -eq $Matches[2]) { "MATCH" } else { "DIFF" }) } }
  "$tag  detector: $det  MEM1 host vs guest: $($cmp -join ' ')"
}
