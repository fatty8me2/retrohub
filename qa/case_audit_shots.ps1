# Shoots EVERY case of a library: front (browse), back (flip) and the spine (inspect turned side-on), headless, muted, no network.
# Usage: qa\case_audit_shots.ps1 [-DataFrom <userdata dir>] [-Out <dir>] [-Consoles gc,n64] [-Jobs 3] [-Secs 6] [-Size 1280x800] [-Spin "-1.25,0"]   (negative yaw turns the spine toward the viewer; positive shows the open edge)
#   Each game is HUB_START_SYS=<console> + HUB_SHOT_CASES=<mode> + HUB_SHOT_CASES_INDEX=<n> (its place in that console tab). Files: <console>-<nn>-<mode>.png
#   RetroHub runs from -Jobs temp copies of build\bin\Release and a COPY of -DataFrom (rom-cache/logs skipped); nothing is written to -DataFrom.
#   Every process is hidden and killed by PID if it overruns.
param([string]$DataFrom = "C:\Users\fatty\AppData\Local\Temp\claude\a82a6632bfc5ba1b\1673c19d-5718-478c-a64b-ab3a8e714680\scratchpad\cov\ud",
      [string]$Out = "", [string[]]$Consoles = @(), [int]$Jobs = 3, [int]$Secs = 6, [string]$Size = "1280x800", [string]$Spin = "-1.25,0.0",
      [string[]]$Modes = @("browse", "flip", "inspect"), [hashtable]$Env = @{})
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "build\bin\Release"
if (-not $Out) { $Out = Join-Path $root "qa_out\case-audit" }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$Consoles = @($Consoles | ForEach-Object { "$_" -split "," } | Where-Object { $_ }); $Modes = @($Modes | ForEach-Object { "$_" -split "," } | Where-Object { $_ })

$counts = @{}
foreach ($m in [regex]::Matches((Get-Content (Join-Path $DataFrom "library_cache.json") -Raw), '"sys":"([a-z0-9]+)"')) { $counts[$m.Groups[1].Value] = 1 + [int]$counts[$m.Groups[1].Value] }
if ($Consoles.Count -eq 0) { $Consoles = @($counts.Keys | Sort-Object) }

$slots = @()
for ($j = 0; $j -lt $Jobs; $j++) {
  $run = Join-Path $env:TEMP ("rh_audit_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
  New-Item -ItemType Directory -Force $run | Out-Null
  Copy-Item (Join-Path $bin "RetroHub.exe"), (Join-Path $bin "SDL2.dll") $run
  foreach ($d in "data", "controls") { if (Test-Path (Join-Path $bin $d)) { Copy-Item (Join-Path $bin $d) (Join-Path $run $d) -Recurse } }
  & robocopy $DataFrom (Join-Path $run "userdata") /E /XD rom-cache logs /NFL /NDL /NJH /NJS /NP | Out-Null
  $slots += [pscustomobject]@{ Run = $run; Proc = $null; Bmp = ""; Name = ""; T0 = $null }
}
Add-Type -AssemblyName System.Drawing
function Finish($s) {
  if ($s.Proc -and -not $s.Proc.HasExited) { if (((Get-Date) - $s.T0).TotalSeconds -gt 100) { try { Stop-Process -Id $s.Proc.Id -Force } catch {}; try { [void]$s.Proc.WaitForExit(5000) } catch {} } else { return $false } }
  if ($s.Bmp -and (Test-Path $s.Bmp)) { $i = [System.Drawing.Image]::FromFile($s.Bmp); $i.Save(($s.Bmp -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose(); Remove-Item $s.Bmp }
  elseif ($s.Bmp) { Write-Host "NO SHOT: $($s.Name)" }
  $s.Proc = $null; $s.Bmp = ""; return $true
}
function Launch($s, [string]$name, [hashtable]$extra) {
  $bmp = Join-Path $Out "$name.bmp"
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; RETROHUB_QA = "1"; HUB_SCREENSHOT = $bmp; HUB_SHOT_SIZE = $Size; HUB_SHOT_SECS = "$Secs" }
  foreach ($k in $Env.Keys) { $set[$k] = $Env[$k] }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  try {
    foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
    $s.Proc = Start-Process (Join-Path $s.Run "RetroHub.exe") -WorkingDirectory $s.Run -PassThru -WindowStyle Hidden
    try { $s.Proc.PriorityClass = 'BelowNormal' } catch {}
  } finally { foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue } }
  $s.Bmp = $bmp; $s.Name = $name; $s.T0 = Get-Date
}
$queue = New-Object System.Collections.Queue
foreach ($c in $Consoles) { for ($i = 0; $i -lt [int]$counts[$c]; $i++) { foreach ($m in $Modes) { $queue.Enqueue(@($c, $i, $m)) } } }
Write-Host ("shots: " + $queue.Count)
while ($queue.Count -gt 0 -or @($slots | Where-Object { $_.Proc }).Count -gt 0) {
  foreach ($s in $slots) {
    if ($s.Proc) { [void](Finish $s) }
    if (-not $s.Proc -and $queue.Count -gt 0) {
      $q = $queue.Dequeue()
      $x = @{ HUB_SHOT_CASES = $q[2]; HUB_START_SYS = $q[0]; HUB_SHOT_CASES_INDEX = "$($q[1])" }
      if ($q[2] -eq "inspect") { $x["HUB_SHOT_SPIN"] = $Spin }
      Launch $s ("{0}-{1:D2}-{2}" -f $q[0], $q[1], $q[2]) $x
    }
  }
  Start-Sleep -Milliseconds 500
}
foreach ($s in $slots) { if ($s.Proc -and -not $s.Proc.HasExited) { try { Stop-Process -Id $s.Proc.Id -Force } catch {} }; Remove-Item $s.Run -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host "screenshots in $Out"
