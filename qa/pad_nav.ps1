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
  Remove-Item (Join-Path $root "build\bin\Release\userdata\hub.json") -ErrorAction SilentlyContinue   # worktree-local settings: every run starts fresh
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name.bmp");
            HUB_SHOT_SIZE = $(if ($extra.ContainsKey("SIZE")) { $extra["SIZE"] } else { "1280x800" }); HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "3"; HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAD = $pad; HUB_SHOT_TRACE = $trace }
  foreach ($k in $extra.Keys) { if ($k -ne "SIZE") { $set[$k] = $extra[$k] } }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(120000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  $rows = @()
  if (Test-Path $trace) {
    foreach ($l in Get-Content $trace) {
      if ($l -match '^pad (\S+) page=(\S+) popup=(\d) friends=(\d) nav=([0-9a-f]+) at=(-?\d+),(-?\d+) osk=(\d) wiz=(-?\d+) name=\[(.*?)\] oskt=\[(.*?)\] sy=(\S+) win=(.*)$') {
        $rows += [pscustomobject]@{ tok = $Matches[1]; page = $Matches[2]; popup = [int]$Matches[3]; friends = [int]$Matches[4]; nav = $Matches[5]; x = [int]$Matches[6]; y = [int]$Matches[7]; osk = [int]$Matches[8]; wiz = [int]$Matches[9]; name = $Matches[10]; oskt = $Matches[11]; sy = $Matches[12]; win = $Matches[13] }
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

# 1. LB/RB cycle through every top-level page and wrap (Settings keeps LB/RB for its 6 tabs, then carries on)
$r = Run "pages" "rb,rb,rb,rb,rb,rb,rb,rb,rb,rb,rb,lb"
$seq = ($r.rows | ForEach-Object { $_.page }) -join ","
Check "rb cycles pages" ($seq -eq "online,saves,controllers,settings,settings,settings,settings,settings,settings,about,library,about") $seq
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

# 9. Setup wizard, pad only: every step, a name typed on the on-screen keyboard (B deletes there, never goes back), finish
$typed = "a,shot:w_controller,a,shot:w_games,a,shot:w_name,up,up,a,shot:w_osk," + ((1..12 | ForEach-Object { "b" }) -join ",") + ",down,a,down,a,right,a,shot:w_typed,start,shot:w_after,down,down,a,shot:w_done,a,a"
$r = Run "wizard" $typed @{ HUB_SHOT_WIZARD = "welcome"; HUB_SHOT_FAKE_PAD = "1" }
$rows = @($r.rows)
$steps = ($rows | Where-Object { $_.tok -eq "a" } | ForEach-Object { $_.wiz }) -join ","
Check "wizard steps by A only" ($steps -like "1,2,3,*4,5,-1") $steps
$open = @($rows | Where-Object { $_.tok -eq "a" -and $_.osk -eq 1 })[0]
Check "A on the name field opens the keyboard" ($null -ne $open -and $open.wiz -eq 3) "no osk row"
$ob = @($rows | Where-Object { $_.tok -eq "b" -and $_.osk -eq 1 })
Check "B in the keyboard deletes, never goes back" ($ob.Count -ge 5 -and @($ob | Where-Object { $_.wiz -ne 3 -or $_.page -ne "library" }).Count -eq 0) "rows=$($ob.Count)"
$st = @($rows | Where-Object { $_.tok -eq "start" })[0]
# Name step = the username box (3+ letters; offline here, so "Continue (claim later)" keeps the typed name once Next is pressed)
$ty = @($rows | Where-Object { $_.tok -eq "shot:w_typed" })[0]
Check "Start closes the keyboard with the typed name" ($st.osk -eq 0 -and $ty.oskt -eq "qas" -and $st.wiz -eq 3) "osk=$($st.osk) typed=$($ty.oskt) wiz=$($st.wiz)"
$dn1 = @($rows | Where-Object { $_.tok -eq "shot:w_done" })[0]
Check "Next (claim later) keeps the typed name and moves on" ($dn1.name -eq "qas" -and $dn1.wiz -eq 4) "name=$($dn1.name) wiz=$($dn1.wiz)"
$field = @($rows | Where-Object { $_.tok -eq "up" })[-1].nav
Check "focus returns to the name field" ($st.nav -eq $field -and $st.nav -ne "00000000") "nav=$($st.nav) field=$field"
$dn = @($rows | Where-Object { $_.tok -eq "down" })[-2]
Check "pad nav resumes after the keyboard" ($dn.nav -ne $field -and $dn.osk -eq 0) "nav=$($dn.nav)"
Check "wizard finishes" (@($rows)[-1].wiz -eq -1) "wiz=$(@($rows)[-1].wiz)"

# 10. Customize... goes to Controllers and B comes back to the same wizard step
$r = Run "wiz_customize" "a,up,right,a,shot:w_cust,b,shot:w_back" @{ HUB_SHOT_WIZARD = "welcome"; HUB_SHOT_FAKE_PAD = "1" }
$rows = @($r.rows)
$c = @($rows | Where-Object { $_.tok -eq "a" })[1]
Check "Customize opens Controllers, setup stays open" ($c.page -eq "controllers" -and $c.wiz -eq -2) "page=$($c.page) wiz=$($c.wiz)"
$bk = @($rows | Where-Object { $_.tok -eq "shot:w_back" })[0]   # the b row itself is sampled before the wizard seeds its focus
Check "B returns to the wizard controller step" ($bk.wiz -eq 1 -and $bk.nav -ne "00000000") "wiz=$($bk.wiz) nav=$($bk.nav)"

# 11. LT / RT scroll whole pages (short window so they overflow) and the focus follows
foreach ($pg in @(@("controllers", "rb,rb,rb"), @("settings", "rb,rb,rb,rb"), @("saves", "rb,rb"))) {
  $r = Run "scroll_$($pg[0])" "$($pg[1]),down,rt,rt,lt,lt" @{ SIZE = "1280x500" }
  $rows = @($r.rows)
  $sc = @($rows | Where-Object { $_.tok -in @("rt", "lt") } | ForEach-Object { if ($_.sy -match '^(\d+)/') { [int]$Matches[1] } else { -1 } })
  Check "LT/RT scroll $($pg[0])" ($sc.Count -eq 4 -and $sc[1] -gt $sc[0] -and $sc[3] -lt $sc[2] -and $sc[1] -gt 0) ($sc -join ",")
  Check "focus stays on the page after LT/RT ($($pg[0]))" (@($rows | Where-Object { $_.tok -in @("rt", "lt") -and $_.win -notlike "*page_*" }).Count -eq 0) "focus left the page"
}

# 12. PlayStation BIOS prompts: A opens the pad folder browser, Back closes it, B closes the prompt
foreach ($b in @("ps1bios", "ps2bios")) {
  $nav = "down,left,a"   # the first pad press only wakes nav inside the modal (headless); down,left then lands on Choose for both prompts
  $r = Run "bios_$b" "shot:b0,$nav,shot:b_browser,back,shot:b_prompt,b,shot:b_closed" @{ HUB_SHOT_EMU = $b }
  $rows = @($r.rows)
  $a = @($rows | Where-Object { $_.tok -eq "a" })[0]
  Check "${b}: A opens the folder browser" ($a.osk -eq 1) "osk=$($a.osk)"
  $bk = @($rows | Where-Object { $_.tok -eq "back" })[0]
  Check "${b}: Back closes the browser, prompt stays" ($bk.osk -eq 0 -and $bk.popup -eq 1) "osk=$($bk.osk) popup=$($bk.popup)"
  $bb = @($rows | Where-Object { $_.tok -eq "b" })[0]
  Check "${b}: B closes the prompt" ($bb.popup -eq 0 -and $bb.nav -ne "00000000") "popup=$($bb.popup) nav=$($bb.nav)"
}

# 13. Pad file browser (account file / Cemu.exe / zip / BattleShip.exe): opens, lists, Back closes
$r = Run "osk_file" "shot:f0,back,shot:f1" @{ HUB_SHOT_OSK = "file" }
$rows = @($r.rows)
Check "file browser shot opens (osk=1)" (@($rows | Where-Object { $_.osk -eq 1 }).Count -ge 1) "no osk row"
$bk = @($rows | Where-Object { $_.tok -eq "back" })
Check "Back closes the file browser" ($bk.Count -ge 1 -and $bk[0].osk -eq 0) "osk=$(if ($bk.Count) { $bk[0].osk })"

Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $png = $_.FullName -replace '\.bmp$', '.png'; $i.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
# 7. room game picker (600-game fake library): lands in the grid, d-pad moves in 2D, Y opens details, B closes them, A chooses and closes the picker
$r = Run "picker" "wait,wait,right,down,down,down,y,wait,b,wait,a,wait" @{ HUB_SHOT_FAKE_LIB = "600"; HUB_SHOT_FAKE_TITLES = "1"; HUB_SHOT_FAKE_ROOM = "1"; HUB_SHOT_ROOM_N = "3"; HUB_SHOT_ROOM_GAME = "fake:3"; HUB_SHOT_PICKER_OPEN = "1" }
$mv = Moves $r.rows
Check "picker: focus starts in the grid" ($r.rows[0].win -like "*pkpick/pickgrid*") "win=$($r.rows[0].win)"
Check "picker: d-pad reaches different games" ((Distinct $mv) -ge 3) "distinct=$(Distinct $mv)"
$y = $r.rows | Where-Object { $_.tok -eq "y" }
Check "picker: Y opens game details" ($y.win -like "*pkdetail*") "win=$($y.win)"
$b = $r.rows | Where-Object { $_.tok -eq "b" }
Check "picker: B closes details, picker stays" ($b.win -like "*pkpick*" -and $b.popup -eq 1) "win=$($b.win) popup=$($b.popup)"
$a = $r.rows | Where-Object { $_.tok -eq "a" }
Check "picker: A chooses a game and closes the picker" ($a.popup -eq 0) "popup=$($a.popup)"
Write-Host "screenshots/traces in $Out; failures: $($script:fail)"
exit $(if ($script:fail) { 1 } else { 0 })
