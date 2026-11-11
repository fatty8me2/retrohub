# Live controller remap, headless (docs/RUNNER_CTL.md "Live controls"). Muted, hidden, no pads/keyboard are read.
# A headless retrorun (GBA test ROM) is driven by RETRORUN_PAD_SCRIPT (RETROHUB_QA=1): a scripted SDL pad holds "a" from frame 0 and the
# profile mapping turns it into a RetroPad mask (RETRORUN_PAD_LOG=1 logs every mask change). Mid-run the profile file is rewritten
# (retro "b" moves from pad "a" to pad "b", "x" takes pad "a") and `reload_controls` is sent through the hub's own sender. Expected:
#   before: mask = B bit (0x1) ; after: mask = X bit (0x200) at the frame logged by "controls: applied".
# Also: a reload with a bad/foreign profile is refused, and the same works with --net-less rollback off (offline). Online uses the same code path.
param([string]$Root = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'; $env:SSB64_MUTE = '1'; $env:RETROHUB_QA = '1'
$exe = Join-Path $Root 'build\bin\Release\retrorun.exe'
$send = Join-Path $Root 'build\bin\Release\retrorun_ctl_send.exe'
$core = @((Join-Path $Root 'deps\cores\mgba_libretro.dll'), (Join-Path $Root 'build\bin\Release\cores\mgba_libretro.dll')) | Where-Object { Test-Path $_ } | Select-Object -First 1
$rom = Join-Path $Root 'deps\testroms\arm.gba'
if (-not $core -or -not (Test-Path $rom) -or -not (Test-Path $exe) -or -not (Test-Path $send)) { Write-Host 'SKIP: retrorun, ctl sender, mgba core or arm.gba missing'; exit 0 }
$fail = 0
function Prof([string]$b, [string]$a, [string]$x) {
    # a minimal retro profile: missing controls fall back to the defaults
    return '{"version":1,"name":"qa","port_order":[],"profile":{"system":"retro","axis_threshold":16384,"buttons":{"b":"' + $b + '","a":"' + $a + '","x":"' + $x + '"},"analogs":{},"keys":{}}}'
}
function WaitAck($dir, $seq, $secs) {
    $end = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $end) {
        try { $j = Get-Content "$dir\control_ack.json" -Raw | ConvertFrom-Json; if ($j.seq -eq $seq) { return $j } } catch {}
        Start-Sleep -Milliseconds 100
    }
    return $null
}
function Send($ctl, [string[]]$a) { $o = @(& $send $ctl @a); return [long]($o | Select-Object -First 1) }
$work = Join-Path $env:TEMP ('live_ctl_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force "$work\sv", "$work\st", "$work\run" | Out-Null
$ctl = "$work\run\control.json"
'{"seq":0}' | Set-Content $ctl -Encoding ascii
$pf = "$work\controls.json"
Set-Content $pf (Prof 'a' 'b' 'y') -Encoding ascii         # retro b = pad a
Set-Content "$work\pad.txt" "0:a" -Encoding ascii            # the scripted pad holds SDL "a" from frame 0
$env:RETRORUN_PAD_SCRIPT = "$work\pad.txt"; $env:RETRORUN_PAD_LOG = '1'
$p = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden -ArgumentList @('--core', $core, '--rom', $rom, '--system', 'gba',
    '--save-dir', "$work\sv", '--state-dir', "$work\st", '--headless', '--realtime', '--frames', '2000000', '--control', $ctl,
    '--game-key', 'qa-live', '--controls', $pf, '--log', "$work\rr.log")
Remove-Item Env:RETRORUN_PAD_SCRIPT, Env:RETRORUN_PAD_LOG
function Masks { @(Select-String -Path "$work\rr.log" -Pattern 'padscript: frame (\d+) mask 0x([0-9a-f]+)' | ForEach-Object { [pscustomobject]@{ frame = [long]$_.Matches[0].Groups[1].Value; mask = [Convert]::ToInt32($_.Matches[0].Groups[2].Value, 16) } }) }
# wait for the runner to log its first scripted-pad mask (generous: a busy PC can take far longer than a fixed 3 s to start the core)
$until = (Get-Date).AddSeconds(90)
while ((Get-Date) -lt $until -and @(Masks).Count -lt 1 -and -not $p.HasExited) { Start-Sleep -Milliseconds 200 }
Start-Sleep -Milliseconds 500
$m = @(Masks)
if ($m.Count -ne 1 -or $m[0].mask -ne 0x1) { Write-Host "FAIL: before the reload expected one mask 0x1 (B), got: $($m | ConvertTo-Json -Compress)"; $fail++ } else { Write-Host "ok: before reload, pad a -> RetroPad B (0x1) from frame $($m[0].frame)" }
# 1. a foreign-system profile is refused and changes nothing
Set-Content $pf '{"version":1,"name":"qa","port_order":[],"profile":{"system":"n64","buttons":{},"analogs":{},"keys":{}}}' -Encoding ascii
$s = Send $ctl @('reload_controls'); $a = WaitAck "$work\run" $s 10
if (-not $a -or $a.ok) { Write-Host "FAIL: foreign profile should be refused: $($a | ConvertTo-Json -Compress)"; $fail++ } else { Write-Host "ok: foreign profile refused -> $($a.msg)" }
# 2. the real change
Set-Content $pf (Prof 'b' 'y' 'a') -Encoding ascii         # retro x = pad a, retro b = pad b
$s = Send $ctl @('reload_controls'); $a = WaitAck "$work\run" $s 10
if (-not $a -or -not $a.ok) { Write-Host "FAIL: reload not acked ok: $($a | ConvertTo-Json -Compress)"; $fail++ } else { Write-Host "ok: reload_controls acked -> $($a.msg)" }
$until = (Get-Date).AddSeconds(30)
while ((Get-Date) -lt $until -and @(Masks).Count -lt 2) { Start-Sleep -Milliseconds 200 }
Start-Sleep -Milliseconds 300
$m = @(Masks)
$ap = Select-String -Path "$work\rr.log" -Pattern 'controls: applied profile .* at frame (\d+)' | Select-Object -Last 1
if ($m.Count -lt 2 -or $m[-1].mask -ne 0x200 -or -not $ap) { Write-Host "FAIL: after the reload expected mask 0x200 (X) and an 'applied' line, got: $($m | ConvertTo-Json -Compress) / $($ap.Line)"; $fail++ }
else { Write-Host "ok: after reload, pad a -> RetroPad X (0x200) from frame $($m[-1].frame) ($($ap.Line.Trim()))" }
$null = Send $ctl @('quit')
if (-not $p.WaitForExit(30000)) { Stop-Process -Id $p.Id; Write-Host 'FAIL: runner did not quit'; $fail++ }
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
if ($fail) { Write-Host "live_controls: $fail FAILED"; exit 1 }
Write-Host 'live_controls: all passed'
exit 0
