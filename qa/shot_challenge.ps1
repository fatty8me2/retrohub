# Headless screenshots of friend challenges (HUB_SCREENSHOT mode: no window is shown, muted, no network, no game launched).
# Usage: powershell -File qa\shot_challenge.ps1 [-Exe build\bin\Release\RetroHub.exe] [-Out build\shots]
param([string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe", [string]$Out = "$PSScriptRoot\..\build\shots")
New-Item -ItemType Directory -Force $Out | Out-Null
$env:RETROHUB_QA = "1"; $env:SSB64_MUTE = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"; $env:HUB_SHOT_SECS = "2"
$exe = (Resolve-Path $Exe).Path
$shots = @(
  @{n="challenge-card";     e=@{HUB_SHOT_CHALLENGE="card"}},
  @{n="challenge-results";  e=@{HUB_SHOT_CHALLENGE="results"}},
  @{n="challenge-cant";     e=@{HUB_SHOT_CHALLENGE="cant"}},
  @{n="challenge-menu-row"; e=@{HUB_SHOT_QM="main"}},
  @{n="challenge-menu-online"; e=@{HUB_SHOT_QM="main"; HUB_SHOT_QM_ONLINE="1"}},
  @{n="challenge-pick";     e=@{HUB_SHOT_QM="challenge"}}
)
foreach ($s in $shots) {
  $env:HUB_SCREENSHOT = "$Out\$($s.n).bmp"; $env:HUB_SHOT_SIZE = "1280x800"
  foreach ($k in $s.e.Keys) { Set-Item "env:$k" $s.e[$k] }
  & $exe | Out-Null
  "{0}: exit {1}" -f $s.n, $LASTEXITCODE
  foreach ($k in $s.e.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
}
Remove-Item env:HUB_SCREENSHOT, env:HUB_SHOT_SIZE -ErrorAction SilentlyContinue
