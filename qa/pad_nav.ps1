# Headless gamepad-navigation walk of every RetroHub page / panel / modal (no visible window, no network, fake library + friends).
# Usage: qa\pad_nav.ps1 [outdir]      Exit code 1 if any page is unreachable or a modal cannot be closed with B.
# Injects pad presses via HUB_SHOT_PAD=<tokens> (down,up,left,right,a,b,x,y,lb,rb,lt,rt,start,back,wait,shot:<name>) and asserts on the
# HUB_SHOT_TRACE lines:  pad <tok> page=<p> popup=<0|1> friends=<0|1> nav=<id> at=<x>,<y> win=<imgui window>
param([string]$Out = "$env:TEMP\pad_nav")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$script:fail = 0
function Run($name, $pad, $extra = @{}) {
  $trace = Join-Path $Out "$name.trace"
  Remove-Item $trace -ErrorAction SilentlyContinue
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name.bmp");
            HUB_SHOT_SIZE = "1280x800"; HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "3"; HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAD = $pad; HUB_SHOT_TRACE = $trace }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(120000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  $rows = @()
  if (Test-Path $trace) {
    foreach ($l in Get-Content $trace) {
      if ($l -match '^pad (\S+) page=(\S+) popup=(\d) friends=(\d) nav=([0-9a-f]+) at=(-?\d+),(-?\d+) win=(.*)$') {
        $rows += [pscustomobject]@{ tok = $Matches[1]; page = $Matches[2]; popup = [int]$Matches[3]; friends = [int]$Matches[4]; nav = $Matches[5]; x = [int]$Matches[6]; y = [int]$Matches[7]; win = $Matches[8] }
      }
    }
  }
  $other = if (Test-Path $trace) { @(Get-Content $trace | Where-Object { $_ -notmatch '^pad ' }) } else { @() }
  return @{ rows = $rows; other = $other }
}
function Check($name, $ok, $detail) {
  if ($ok) { Write-Host "PASS $name" } else { Write-Host "FAIL $name  ($detail)"; $script:fail++ }
}
function Moves($rows) { @($rows | Where-Object { $_.tok -in @("up", "down", "left", "right") }) }
function Distinct($rows) { @($rows | Where-Object { $_.nav -ne "00000000" } | Select-Object -ExpandProperty nav -Unique).Count }

# 1. LB/RB cycle through every top-level page and wrap
$r = Run "pages" "rb,rb,rb,rb,rb,rb,lb"
$seq = ($r.rows | ForEach-Object { $_.page }) -join ","
Check "rb cycles pages" ($seq -eq "online,saves,controllers,settings,about,library,about") $seq
Check "page switch keeps a focus" (@($r.rows | Where-Object { $_.nav -eq "00000000" }).Count -eq 0) "focus lost after a page switch"

# 2. every page: first focus exists and D-pad reaches several different items
$pages = @(@("online", "rb"), @("saves", "rb,rb"), @("controllers", "rb,rb,rb"), @("settings", "rb,rb,rb,rb"), @("about", "rb,rb,rb,rb,rb"))
foreach ($pg in $pages) {
  $r = Run "reach_$($pg[0])" "$($pg[1]),down,right,down,down,down,down,up"
  $mv = Moves $r.rows
  $d = Distinct $r.rows
  $need = if ($pg[0] -eq "about") { 2 } elseif ($pg[0] -eq "online") { 3 } else { 4 }
  Check "reach $($pg[0])" ($d -ge $need -and @($r.rows | Where-Object { $_.nav -eq "00000000" }).Count -eq 0) "distinct focus targets=$d (need $need)"
}

# 3. library: grid / shelves navigation, details modal on Y, B closes it and keeps a focus
$r = Run "library" "down,down,right,down,down,right,left,up,y,b,b"
$d = Distinct ($r.rows | Where-Object { $_.tok -in @("down", "right", "left", "up") })
Check "library d-pad reaches tiles" ($d -ge 3) "distinct=$d"
$y = $r.rows | Where-Object { $_.tok -eq "y" }
Check "Y opens the details modal" ($y.popup -eq 1 -and $y.win -like "*details*") "popup=$($y.popup) win=$($y.win)"
$b = @($r.rows | Where-Object { $_.tok -eq "b" })[0]
Check "B closes details and refocuses" ($b.popup -eq 0 -and $b.nav -ne "00000000") "popup=$($b.popup) nav=$($b.nav)"

# 4. B goes back to the library from any page
$r = Run "back" "rb,rb,rb,b"
Check "B returns to the library" (@($r.rows)[-1].page -eq "library") (@($r.rows)[-1].page)

# 5. friends panel: toggle with View, reach rows, row menu (A), close it with B, toggle off
$r = Run "friends" "back,back,down,down,down,down,down,down,down,down,a,down,b,b,b" @{ HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FRIENDS = "1" }
$rows = @($r.rows)
Check "View toggles the friends panel" ($rows[1].friends -eq 1 -and $rows[1].win -like "*friends*") "friends=$($rows[1].friends) win=$($rows[1].win)"
Check "d-pad reaches friend rows" (@($rows | Where-Object { $_.win -like "*frlist*" }).Count -ge 3) "rows reached"
$a = $rows | Where-Object { $_.tok -eq "a" }
Check "A on a friend opens the actions menu" ($a.popup -eq 1) "popup=$($a.popup)"
$bs = @($rows | Where-Object { $_.tok -eq "b" })
Check "B closes the actions menu" ($bs[0].popup -eq 0 -and $bs[0].friends -eq 1) "popup=$($bs[0].popup) friends=$($bs[0].friends)"
Check "B closes the friends panel and refocuses" ($bs[-1].friends -eq 0 -and $bs[-1].nav -ne "00000000") "friends=$($bs[-1].friends) nav=$($bs[-1].nav)"

# 6. invite toast: Start joins it
$r = Run "toast" "start,wait" @{ HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FRIENDS = "1" }
Check "Start joins the invite toast" (@($r.other | Where-Object { $_ -like "join-toast *" }).Count -ge 1) ($r.other -join "|")

# 7. Add-games dialog: reachable, B closes it
$r = Run "addgames" "down,down,b" @{ HUB_SHOT_ADDGAMES = "dialog" }
$rows = @($r.rows)
Check "Add games dialog traps focus" ($rows[0].popup -eq 1 -and $rows[1].win -like "*addgames*") "win=$($rows[1].win)"
Check "B closes Add games" ($rows[-1].popup -eq 0 -and $rows[-1].nav -ne "00000000") "popup=$($rows[-1].popup) nav=$($rows[-1].nav)"

# 8. online room page (fake room): reachable
$r = Run "room" "down,right,down,down,down" @{ HUB_SHOT_FAKE_ROOM = "1"; HUB_SHOT_ROOM_N = "3" }
$d = Distinct $r.rows
Check "room page reachable" ($d -ge 3) "distinct=$d"

Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $png = $_.FullName -replace '\.bmp$', '.png'; $i.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots/traces in $Out; failures: $($script:fail)"
exit $(if ($script:fail) { 1 } else { 0 })
