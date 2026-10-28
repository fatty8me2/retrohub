# Headless end-to-end test of the in-game quick menu (src/hub/qm_e2e.cpp). No window: SDL dummy video driver; input only from an
# SDL virtual game controller (the user's real pads are never opened). Exit code 0 = every scenario passed.
#   powershell -NoProfile -File qa\qm_e2e.ps1 [-Exe build\bin\Release\RetroHub.exe] [-Runs 1]
param([string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe", [int]$Runs = 1)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path $Exe).Path
$fails = 0
for ($r = 1; $r -le $Runs; $r++) {
    $dir = Join-Path ([IO.Path]::GetTempPath()) ("qm-e2e-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force $dir | Out-Null
    $env:HUB_SCREENSHOT = "$dir\e2e.bmp"
    $env:HUB_QM_E2E = '1'
    $env:HUB_SHOT_VAULT = "$dir\vault"
    $env:HUB_SHOT_NOWAIT = '1'
    $env:HUB_SHOT_SIZE = '960x600'
    $env:SSB64_MUTE = '1'
    $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -Wait -RedirectStandardOutput "$dir\stdout.txt" -RedirectStandardError "$dir\stderr.txt"
    Get-Content "$dir\e2e-qm-e2e.log" -ErrorAction SilentlyContinue | Where-Object { $_ -match 'FAIL|qm e2e:' }
    if ($p.ExitCode -ne 0) { $fails++; Write-Output "run $r FAILED (exit $($p.ExitCode)); log: $dir\e2e-qm-e2e.log" }
    else { Write-Output "run $r passed"; Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
}
Write-Output "qm_e2e: $($Runs - $fails)/$Runs runs passed"
exit ([int]($fails -ne 0))
