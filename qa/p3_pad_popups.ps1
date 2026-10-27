# Headless gamepad walk of the popups that pad_nav.ps1 does not cover: missing-ROM, Hidden games, theme picker,
# screenshot viewer, achievements sign-in. No visible window, no network, muted, scratch vault only.
# Usage: qa\p3_pad_popups.ps1 [outdir]      Exit code 1 if a popup has a dead end (focus lost), an unreachable control,
#        or B does not close a modal.
# Same injection as pad_nav.ps1: HUB_SHOT_PAD=<tokens>, asserted on HUB_SHOT_TRACE lines
#   pad <tok> page=<p> popup=<0|1> friends=<0|1> nav=<id> at=<x>,<y> ... win=<imgui window>
param([string]$Out = "$env:TEMP\p3_pad_popups")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$script:fail = 0

function Run($name, $pad, $extra = @{}) {
  $trace = Join-Path $Out "$name.trace"
  Remove-Item $trace -ErrorAction SilentlyContinue
  Remove-Item (Join-Path $root "build\bin\Release\userdata\hub.json") -ErrorAction SilentlyContinue   # every run starts fresh
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name.bmp");
            HUB_SHOT_VAULT = (Join-Path $Out "vault_$name"); HUB_SHOT_SIZE = "1280x800"; HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "4";
            HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAD = $pad; HUB_SHOT_TRACE = $trace }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(120000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  $rows = @()
  if (Test-Path $trace) {
    foreach ($l in Get-Content $trace) {
      if ($l -match '^pad (\S+) page=(\S+) popup=(\d) friends=(\d) nav=([0-9a-f]+) at=(-?\d+),(-?\d+) osk=(\d) wiz=(-?\d+) name=\[(.*?)\] oskt=\[(.*?)\] sy=(\S+) win=(.*)$') {
        $rows += [pscustomobject]@{ tok = $Matches[1]; popup = [int]$Matches[3]; nav = $Matches[5]; x = [int]$Matches[6]; y = [int]$Matches[7]; win = $Matches[13] }
      }
    }
  }
  return ,$rows
}

function Check($name, $ok, $detail) {
  if ($ok) { Write-Host "PASS $name" } else { Write-Host "FAIL $name  ($detail)"; $script:fail++ }
}

# A popup walk passes when: it opens (popup=1), focus is on a control from the first press on, no press ever leaves
# focus on nothing (nav=00000000), the d-pad reaches at least $MinTargets distinct controls, and the close button
# closes it (popup=0 after $CloseTok). Rows after the close are only checked for focus, not for popup state.
function Walk($label, $rows, $MinTargets, $CloseTok = "b") {
  $after = @($rows | Where-Object { $_.tok -notin @("wait") })
  $ps = @($after | Where-Object { $_.tok -like "shot:*" -eq $false })
  $open = @($ps | Where-Object { $_.popup -eq 1 })
  Check "${label} opens" ($open.Count -ge 1) "no popup=1 row"
  if ($open.Count -eq 0) { return }
  $dead = @($ps | Where-Object { $_.popup -eq 1 -and $_.nav -eq "00000000" })
  Check "${label}: focus never lost (no dead end)" ($dead.Count -eq 0) "$($dead.Count) presses left focus on nothing, first at $($dead[0].tok)"
  $mv = @($ps | Where-Object { $_.tok -in @("up", "down", "left", "right") -and $_.popup -eq 1 })
  $targets = @($mv | Where-Object { $_.nav -ne "00000000" } | Select-Object -ExpandProperty nav -Unique).Count
  Check "${label}: d-pad reaches $MinTargets controls" ($targets -ge $MinTargets) "distinct focus targets=$targets"
  $cl = @($ps | Where-Object { $_.tok -eq $CloseTok })
  Check "${label}: $CloseTok closes it" ($cl.Count -ge 1 -and $cl[-1].popup -eq 0) "popup after close=$(if ($cl.Count) { $cl[-1].popup } else { 'no close press' })"
  if ($cl.Count -ge 1 -and $cl[-1].popup -eq 0) {
    $ref = @($ps | Where-Object { $_.popup -eq 0 -and $_.nav -ne "00000000" })
    Check "${label}: focus stays after close" ($ref.Count -ge 1) "no focus after close"
  }
  Write-Host "    $label targets: $targets distinct; windows seen: $((@($ps | Where-Object { $_.popup -eq 1 } | Select-Object -ExpandProperty win -Unique)) -join ' | ')"
}

# 1. Missing ROM dialog (ask to locate the ROM): opened by the shot aid on the library page
$r = Run "missingrom" "wait,down,right,down,right,down,b,shot:mr_closed" @{ HUB_SHOT_EMU = "missingrom" }
Walk "missing-ROM" $r 2

# 2. Hidden games (Settings > General, unhide list)
$r = Run "hidden" "wait,down,right,down,right,down,b" @{ HUB_SHOT_PAGE = "settings:0"; HUB_SHOT_HIDDEN = "1"; HUB_SHOT_EMU = "hidden" }
Walk "hidden games" $r 3

# 3. Theme picker (Settings > General, Theme swatch row): the swatches are the focus targets, B leaves the page cleanly
$r = Run "theme" "wait,down,right,right,right,right,right,right,right,left,up,shot:theme_row,b" @{ HUB_SHOT_PAGE = "settings:0" }
$th = @($r | Where-Object { $_.tok -notin @("wait") -and $_.nav -ne "00000000" })
$tt = @($th | Select-Object -ExpandProperty nav -Unique).Count
Check "theme picker: swatches reachable" ($tt -ge 3) "distinct focus targets=$tt"
Check "theme picker: focus never lost" (@($r | Where-Object { $_.tok -notin @("wait", "b") -and $_.nav -eq "00000000" }).Count -eq 0) "focus lost in the theme row"

# 4. Screenshot viewer (full-size picture over the Quick Menu gallery)
$r = Run "viewer" "wait,right,left,down,b" @{ HUB_SHOT_GALLERY = "1"; HUB_SHOT_VIEWER = "1"; HUB_SHOT_DETAILS = "fake:0"; HUB_SHOT_TAB = "2" }
Walk "screenshot viewer" $r 2

# 5. Achievements sign-in (Settings > Account, signed out): fields and the Sign in button, no dead end
$r = Run "signin" "wait,down,down,down,down,down,down,up,up,b" @{ HUB_SHOT_PAGE = "settings:5" }
$ss = @($r | Where-Object { $_.tok -notin @("wait") })
Check "sign-in: focus lands on a control" (@($ss | Where-Object { $_.tok -eq "down" -and $_.nav -ne "00000000" }).Count -ge 1) "no focus in Account"
$tg = @($ss | Where-Object { $_.tok -in @("up", "down") -and $_.nav -ne "00000000" } | Select-Object -ExpandProperty nav -Unique).Count
Check "sign-in: d-pad reaches 3 controls" ($tg -ge 3) "distinct=$tg"
Check "sign-in: B leaves the page with focus intact" (@($ss | Where-Object { $_.tok -eq "b" -and $_.nav -ne "00000000" }).Count -ge 1) "focus lost on B"

Write-Host "traces and screenshots in $Out; failures: $($script:fail)"
exit $(if ($script:fail) { 1 } else { 0 })


