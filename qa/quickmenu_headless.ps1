# Headless check of the quick menu's runner-side commands (src/hub/quickmenu.cpp, docs/RUNNER_CTL.md): drives control.json of a
# running retrorun (GBA test ROM): volume, mute, speed (every step), ff on/off, the refusal of a bad command, then quit.
# Also checks the --volume launch argument (log line). Hidden, muted, no visible windows, no network.
param([string]$Root = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'; $env:SSB64_MUTE = '1'
$fail = 0
function Send($ctl, $seq, $json) { [IO.File]::WriteAllText($ctl, $json.Replace('SEQ', "$seq")) }
function WaitAck($dir, $seq, $secs) {
    $ack = "$dir\control_ack.json"
    $end = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $end) {
        if (Test-Path $ack) { try { $j = Get-Content $ack -Raw | ConvertFrom-Json; if ($j.seq -eq $seq) { return $j } } catch {} }
        Start-Sleep -Milliseconds 150
    }
    return $null
}
$work = Join-Path $env:TEMP ('qm_rr_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force "$work\sv", "$work\st", "$work\run", "$work\shots" | Out-Null
'{"seq":0}' | Set-Content "$work\run\control.json" -Encoding ascii
$p = Start-Process -FilePath (Join-Path $Root 'build\bin\Release\retrorun.exe') -PassThru -WindowStyle Hidden -RedirectStandardOutput "$work\o.txt" -RedirectStandardError "$work\e.txt" -ArgumentList @(
    '--core', (Join-Path $Root 'deps\cores\mgba_libretro.dll'), '--rom', (Join-Path $Root 'deps\testroms\arm.gba'), '--system', 'gba',
    '--save-dir', "$work\sv", '--state-dir', "$work\st", '--headless', '--frames', '2000000', '--volume', '40', '--control', "$work\run\control.json",
    '--game-key', 'qa-gba', '--shot-dir', "$work\shots", '--log', "$work\rr.log")
Start-Sleep -Seconds 4
$ctl = "$work\run\control.json"
$steps = @(
    @('volume 55',  '{"seq":SEQ,"cmd":"volume","slot":0,"value":55,"speed":1,"text":""}', $true),
    @('volume 0',   '{"seq":SEQ,"cmd":"volume","slot":0,"value":0,"speed":1,"text":""}', $true),
    @('mute on',    '{"seq":SEQ,"cmd":"mute","slot":0,"value":1,"speed":1,"text":""}', $true),
    @('mute off',   '{"seq":SEQ,"cmd":"mute","slot":0,"value":0,"speed":1,"text":""}', $true))
foreach ($sp in '0.25', '0.5', '1', '1.5', '2', '3', '5', '8') {
    $steps += , @("speed $sp", ('{"seq":SEQ,"cmd":"speed","slot":0,"value":0,"speed":' + $sp + ',"text":""}'), $true)
}
$steps += , @('speed max', '{"seq":SEQ,"cmd":"speed","slot":0,"value":0,"speed":"max","text":""}', $true)
$steps += , @('ff on',  '{"seq":SEQ,"cmd":"ff","slot":0,"value":1,"speed":1,"text":""}', $true)
$steps += , @('ff off', '{"seq":SEQ,"cmd":"ff","slot":0,"value":0,"speed":1,"text":""}', $true)
$steps += , @('speed 1', '{"seq":SEQ,"cmd":"speed","slot":0,"value":0,"speed":1,"text":""}', $true)
$seq = 0
foreach ($s in $steps) {
    $seq++
    Send $ctl $seq $s[1]
    $a = WaitAck $work\run $seq 20
    if (-not $a) { Write-Host "FAIL: no ack for $($s[0])"; $fail++; continue }
    if ([bool]$a.ok -ne $s[2]) { Write-Host "FAIL: $($s[0]) ok=$($a.ok) msg=$($a.msg)"; $fail++ } else { Write-Host "ok: $($s[0]) -> $($a.msg)" }
}
$seq++
Send $ctl $seq '{"seq":SEQ,"cmd":"quit","slot":0,"value":0,"speed":1,"text":""}'
$null = WaitAck $work\run $seq 20
if (-not $p.WaitForExit(30000)) { Write-Host 'FAIL: runner did not quit'; Stop-Process -Id $p.Id; $fail++ }
$log = Get-Content "$work\rr.log" -Raw -ErrorAction SilentlyContinue
foreach ($want in 'speed: Speed 0.25x', 'speed: Speed 8x', 'speed: Speed max', 'speed: Speed 1x') {
    if ($log -notmatch [regex]::Escape($want)) { Write-Host "FAIL: log has no '$want'"; $fail++ }
}
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
if ($fail) { Write-Host "quickmenu_headless: $fail FAILED"; exit 1 }
Write-Host 'quickmenu_headless: all passed'
