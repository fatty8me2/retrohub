# Pre-release QA driver: runs the headless QA scripts one after another against a LOCAL worker and prints a PASS/FAIL table.
#   powershell -File qa\prerelease_run.ps1 -Server ws://127.0.0.1:9900 [-Rom <n64 rom>]
param([string]$Server = 'ws://127.0.0.1:9900', [string]$Rom = 'C:\Users\fatty\Desktop\Everything\Games\N64\Mario Kart 64 (U) [!].z64',
      [string]$Only = '', [string]$Cf = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'qa-124-bs\server\cloudflare'))
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'
$rows = @()
function Step([string]$name, [scriptblock]$sb) {
    if ($Only -and $name -notmatch $Only) { return }
    $t0 = Get-Date
    $out = @(& $sb 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    $res = if ($code -eq 0) { 'PASS' } elseif ($code -eq 77) { 'SKIP' } else { 'FAIL' }
    $script:rows += [pscustomobject]@{ Step = $name; Result = $res; Code = $code; Secs = [int]((Get-Date) - $t0).TotalSeconds; Tail = (($out | Select-Object -Last 2) -join ' | ') }
    $out | Out-File (Join-Path $root "qa\out\pre_$($name -replace '[^A-Za-z0-9]','_').log") -Encoding utf8
    Write-Host ("{0,-34} {1} (code {2})" -f $name, $res, $code)
}
$ps = (Get-Command powershell).Source
function RunPS([string[]]$a) { & $ps -NoProfile -ExecutionPolicy Bypass -File @a }
Step 'retrorun direct 2p'   { RunPS @("qa\retrorun_online.ps1", '-Core', 'mgba_libretro.dll', '-Server', $Server, '-Tag', 'pre_direct') }
Step 'retrorun relay 2p'    { $env:HUBNET_FORCE_RELAY = '1'; RunPS @("qa\retrorun_online.ps1", '-Core', 'mgba_libretro.dll', '-Server', $Server, '-Tag', 'pre_relay'); Remove-Item Env:HUBNET_FORCE_RELAY }
Step 'retrorun 3p kill slot0' { RunPS @("qa\retrorun_online.ps1", '-Core', 'mgba_libretro.dll', '-Server', $Server, '-Players', '3', '-Ports', '0,1,2,-1', '-KillSlot', '0', '-Frames', '60000', '-KillAfterSec', '4', '-Tag', 'pre_k0') }
Step 'retrorun 3p drop slot1' { RunPS @("qa\retrorun_online.ps1", '-Core', 'mgba_libretro.dll', '-Server', $Server, '-Players', '3', '-Ports', '0,1,2,-1', '-KillSlot', '1', '-ExpectDrop', '-Frames', '60000', '-KillAfterSec', '4', '-Tag', 'pre_k1') }
Step 'n64run hostkill 2p'   { RunPS @("qa\n64run_hostkill.ps1", '-Server', $Server, '-Rom', $Rom) }
Step 'n64run drop 3p'       { RunPS @("qa\n64run_hostkill.ps1", '-Server', $Server, '-Rom', $Rom, '-Players', '3', '-KillSlot', '1', '-ExpectDrop') }
Step 'ps1_online quick'     { RunPS @("qa\ps1_online.ps1", '-Server', $Server) }
Step 'ps1_discs'            { RunPS @("qa\ps1_discs.ps1") }
Step 'link_trade_gb'        { RunPS @("qa\link_trade_gb.ps1", '-Server', $Server) }
Step 'cheats_headless gba'  { RunPS @("qa\cheats_headless.ps1") }
Step 'cheats_n64_headless'  { RunPS @("qa\cheats_n64_headless.ps1", '-Rom', $Rom) }
Step 'cheats_ps1_headless'  { RunPS @("qa\cheats_ps1_headless.ps1") }
Step 'pad_nav'              { RunPS @("qa\pad_nav.ps1") }
Step 'friends_clicks'       { RunPS @("qa\friends_clicks.ps1") }
Step 'saves_two_pc'         { RunPS @("qa\saves_two_pc.ps1") }
Step 'saves_cloud'          { RunPS @("qa\saves_cloud.ps1", '-WorkerDir', $Cf) }
$rows | Format-Table -AutoSize -Wrap
$rows | Export-Csv (Join-Path $root 'qa\out\prerelease.csv') -NoTypeInformation
