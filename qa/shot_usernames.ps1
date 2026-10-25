# Headless screenshots of the username UI (no windows are shown: HUB_SCREENSHOT mode, muted, no network).
# Usage: powershell -File qa\shot_usernames.ps1 [-Exe build\bin\Release\RetroHub.exe] [-Out build\shots]
param([string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe", [string]$Out = "$PSScriptRoot\..\build\shots")
New-Item -ItemType Directory -Force $Out | Out-Null
$env:RETROHUB_QA = "1"; $env:SSB64_MUTE = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"; $env:HUB_SHOT_SECS = "2"
$shots = @(
  @{n="wiz-name-empty";     e=@{HUB_SHOT_WIZARD="name"; HUB_SHOT_USERNAME="empty"}},
  @{n="wiz-name-available"; e=@{HUB_SHOT_WIZARD="name"; HUB_SHOT_USERNAME="available"}},
  @{n="wiz-name-taken";     e=@{HUB_SHOT_WIZARD="name"; HUB_SHOT_USERNAME="taken"}},
  @{n="wiz-name-notallowed";e=@{HUB_SHOT_WIZARD="name"; HUB_SHOT_USERNAME="notallowed"}},
  @{n="wiz-name-invalid";   e=@{HUB_SHOT_WIZARD="name"; HUB_SHOT_USERNAME="invalid"}},
  @{n="wiz-name-offline";   e=@{HUB_SHOT_WIZARD="name"; HUB_SHOT_USERNAME="offline"}},
  @{n="wiz-returning";      e=@{HUB_SHOT_WIZARD="returning"; HUB_SHOT_ACCOUNT="redeem"}},
  @{n="settings-account";   e=@{HUB_SHOT_PAGE="settings"; HUB_SHOT_USERNAME_CLAIMED="kai_64"; HUB_SHOT_WHEEL="700,500,39"}},
  @{n="settings-code";      e=@{HUB_SHOT_PAGE="settings"; HUB_SHOT_ACCOUNT="code"; HUB_SHOT_USERNAME_CLAIMED="kai_64"; HUB_SHOT_WHEEL="700,500,54"}},
  @{n="settings-change";    e=@{HUB_SHOT_PAGE="settings"; HUB_SHOT_USERNAME_CLAIMED="kai_64"; HUB_SHOT_USERNAME_CHANGE="1"; HUB_SHOT_USERNAME="taken"; HUB_SHOT_WHEEL="700,500,39"}},
  @{n="prompt-newname";     e=@{HUB_SHOT_USERNAME_PROMPT="1"; HUB_SHOT_USERNAME="available"}},
  @{n="banner";             e=@{HUB_SHOT_USERNAME_BANNER="1"}},
  @{n="friends-add";        e=@{HUB_SHOT_FRIENDS="1"; HUB_SHOT_FAKE_FRIENDS="1"; HUB_SHOT_USERNAME_CLAIMED="kai_64"}}
)
foreach ($size in @("1280x800", "1600x1000")) {
  foreach ($s in $shots) {
    $keys = @($s.e.Keys); $keys += "HUB_SCREENSHOT", "HUB_SHOT_SIZE"
    $env:HUB_SCREENSHOT = "$Out\$($s.n)-$size.bmp"; $env:HUB_SHOT_SIZE = $size
    foreach ($k in $s.e.Keys) { Set-Item "env:$k" $s.e[$k] }
    $p = Start-Process -FilePath (Resolve-Path $Exe) -PassThru -WindowStyle Hidden -Wait
    foreach ($k in $s.e.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    "{0} {1}: exit {2}" -f $s.n, $size, $p.ExitCode
  }
}
