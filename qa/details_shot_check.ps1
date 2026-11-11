# Reliability check for headless game-page screenshots. Takes N shots each of two different named games' pages and counts how many
# really captured the right page (the hub writes "captured page=<p> details=<yes|no> game=<key>" into HUB_SHOT_TRACE in shot mode).
# Headless only: HUB_SCREENSHOT uses SDL's dummy video driver (no window), muted, fake library, scratch data under the exe's userdata\.
# usage: qa\details_shot_check.ps1 [-N 5] [-Mode details|keys] [-Games fake:1,fake:3] [-Exe <RetroHub.exe>] [-Out <dir>]
#   details = HUB_SHOT_DETAILS=<key> (the aid); keys = HUB_SELECT_GAME=<key> + HUB_SHOT_KEYS=i (the old workaround)
# Exit code 0 only when every shot was right. Also checks that an unknown key fails loudly (details mode, non-zero exit).
param([int]$N = 5, [ValidateSet("details", "keys")][string]$Mode = "details", [string[]]$Games = @("fake:1", "fake:3"),
      [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe", [string]$Out = "$env:TEMP\details_shot_check")
$busy = Get-Process lichii, eden-cli, hookshot, Dolphin, Cemu, azahar, pcsx2-qt, PPSSPPWindows64, retrorun, n64run -ErrorAction SilentlyContinue
if ($busy) { Write-Output "a game/emulator is running: not running QA now"; exit 3 }
New-Item -ItemType Directory -Force $Out | Out-Null
$Exe = (Resolve-Path $Exe).Path
$env:RETROHUB_NO_SHELL = "1"; $env:RETROHUB_QA = "1"; $env:SSB64_MUTE = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"
$env:HUB_SHOT_FAKE_LIB = "6"; $env:HUB_SHOT_NOWAIT = "1"; $env:HUB_SHOT_SECS = "3"; $env:HUB_SHOT_SIZE = "1280x800"
$env:HUB_SHOT_VAULT = "$Out\vault"
function Run($game, $i, $m) {
  $tag = ($game -replace '[^\w]', '_') + "_$i"
  $bmp = "$Out\$tag.bmp"; $trace = "$Out\$tag.trace"
  Remove-Item $bmp, $trace -ErrorAction SilentlyContinue
  $env:HUB_SCREENSHOT = $bmp; $env:HUB_SHOT_TRACE = $trace
  if ($m -eq "details") { $env:HUB_SHOT_DETAILS = $game } else { $env:HUB_SELECT_GAME = $game; $env:HUB_SHOT_KEYS = "i" }
  $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(60000)) { Stop-Process -Id $p.Id -Force }
  Remove-Item env:HUB_SCREENSHOT, env:HUB_SHOT_TRACE, env:HUB_SHOT_DETAILS, env:HUB_SELECT_GAME, env:HUB_SHOT_KEYS -ErrorAction SilentlyContinue
  $line = if (Test-Path $trace) { (Get-Content $trace | Where-Object { $_ -like "captured *" } | Select-Object -Last 1) } else { $null }
  return [pscustomobject]@{ Exit = $p.ExitCode; Bmp = (Test-Path $bmp); Line = $line }
}
$ok = 0; $total = 0
foreach ($g in $Games) {
  $good = 0
  for ($i = 1; $i -le $N; $i++) {
    $r = Run $g $i $Mode
    $right = $r.Bmp -and $r.Exit -eq 0 -and $r.Line -and $r.Line.Contains("details=yes") -and $r.Line.Contains("game=$g ")
    if ($right) { $good++ } else { Write-Output "  MISS $g #$i exit=$($r.Exit) bmp=$($r.Bmp) line=[$($r.Line)]" }
  }
  Write-Output "$g ($Mode): $good/$N right"
  $ok += $good; $total += $N
}
$bad = $null
if ($Mode -eq "details") {
  $r = Run "no:such-key" 0 "details"
  $loud = ($r.Exit -ne 0) -and -not $r.Bmp
  Write-Output "unknown key fails loudly (exit != 0, no bmp): $loud (exit=$($r.Exit))"
  if (-not $loud) { $bad = 1 }
}
Write-Output "TOTAL: $ok/$total right"
if ($ok -ne $total -or $bad) { exit 1 }
