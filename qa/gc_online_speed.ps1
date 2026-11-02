# GameCube online speed matrix (QA). Runs hub_gc_speed rows one after another: every Dolphin on a private hidden desktop, muted
# ("No Audio Output"), scripted DSU pad, per-second emulation speed from the game's VI-rate counter plus Dolphin's own vblank log.
# Discs and saves are read from the given paths (saves are COPIED into the scratch dir; nothing in the library is written).
# Rows (see docs/DOLPHIN.md "Online speed"):
#   A offline | A2 offline2 (two solo Dolphins at once) | B np-direct (RetroHub NetPlay config, no forwarder) | C np-direct --stock
#   D np-fw (hub forwarder, direct UDP via the local wrangler) | E np-fw + HUBNET_FORCE_RELAY | F np-fw + HUBNET_SIM latency/jitter
#   H np-trav (Dolphin traversal server, no forwarder) | I = C (Dolphin direct IP, stock)
# A row is "<tag>|<mode>|<extra hub_gc_speed args>|<env K=V;K=V>".
# usage: qa\gc_online_speed.ps1 -Build build\bin\Release -Dolphin C:\rhq\gcspeed\dolphin -Disc <disc> -Game GYQE01 -Gci <save dir>
#        [-Rows "A|offline||","B|np-direct||"] [-Secs 90] [-Warm 40] [-Work C:\rhq\gcspeed] [-Server ws://127.0.0.1:8761] [-Out results.txt]
param(
  [Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$Dolphin, [Parameter(Mandatory)][string]$Disc, [Parameter(Mandatory)][string]$Game,
  [string]$Gci = "", [string[]]$Rows = @(), [int]$Secs = 90, [int]$Warm = 40, [string]$Work = "C:\rhq\gcspeed", [string]$Server = "ws://127.0.0.1:8761",
  [string]$Pattern = "seq:3=a,4.5=a,6=a,7.5=a,9=a,11=a", [string]$Out = "", [int]$Repeat = 1
)
if ($Server) { . "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server }
$env:SSB64_MUTE = "1"; $env:N64RUN_MUTE = "1"; $env:RETRORUN_MUTE = "1"
if (-not $Rows -or $Rows.Count -eq 0) {
  $Rows = @("A|offline||", "A2|offline2||", "B|np-direct||", "C|np-direct|--stock|", "D|np-fw||", "E|np-fw||HUBNET_FORCE_RELAY=1",
            "F|np-fw||HUBNET_SIM_ENABLE=1;HUBNET_SIM_LATENCY_MS=40;HUBNET_SIM_JITTER_MS=15", "H|np-trav|--stock|")
}
if (-not $Out) { $Out = Join-Path $Work ("matrix-" + $Game + "-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".txt") }
$exe = Join-Path $Build "hub_gc_speed.exe"
foreach ($r in $Rows) {
  $p = $r -split '\|', 4
  $tag = $p[0]; $mode = $p[1]; $extra = if ($p.Count -gt 2 -and $p[2]) { $p[2] -split ' ' } else { @() }; $envs = if ($p.Count -gt 3) { $p[3] } else { "" }
  for ($k = 1; $k -le $Repeat; $k++) {
    $t = if ($Repeat -gt 1) { "$tag-$Game-$k" } else { "$tag-$Game" }
    $set = @()
    foreach ($kv in ($envs -split ';')) { if ($kv -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2]; $set += $Matches[1] } }
    $a = @("--dolphin", $Dolphin, "--disc", $Disc, "--game", $Game, "--work", $Work, "--mode", $mode, "--secs", $Secs, "--warm", $Warm, "--tag", $t, "--pattern", $Pattern, "--shot-every", "10")
    if ($Gci) { $a += @("--gci", $Gci) }
    if ($mode -eq "np-fw") { $a += @("--server", $Server) }
    $a += $extra
    $o = & $exe @a 2>&1 | Out-String
    foreach ($n in $set) { Remove-Item "env:$n" -ErrorAction SilentlyContinue }
    "===== $t ($mode $($extra -join ' ') $envs)`r`n$o" | Add-Content -Path $Out
    ($o -split "`r?`n") | Where-Object { $_ -match '^(RESULT|TRACE|FWRATE|DOLPHIN .*vblank|\[|RUN|FAIL|driver)' } | ForEach-Object { "$t  $_" }
  }
}
"full output: $Out"
