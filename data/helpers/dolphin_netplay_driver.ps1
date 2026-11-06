param([string]$Role, [int]$DolphinPid, [string]$Status, [string]$Suffix, [int]$Players = 2, [int]$DelayS = 0, [int]$TimeoutS = 150, [string]$Code = "", [int]$NoCode = 0, [string]$GoFile = "", [string]$BufFile = "")
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$UA = [System.Windows.Automation.AutomationElement]; $TS = [System.Windows.Automation.TreeScope]
$CT = [System.Windows.Automation.ControlType]
function Say($t) {   # RetroHub reads this file while we append: retry on a sharing violation, a lost "stage:" line would stall the whole room
  for ($k = 0; $k -lt 40; $k++) { try { [System.IO.File]::AppendAllText($Status, $t + "`r`n"); return } catch { Start-Sleep -Milliseconds 100 } }
}
function Top($pid_) {
  $pc = New-Object System.Windows.Automation.PropertyCondition($UA::ProcessIdProperty, $pid_)
  $UA::RootElement.FindAll($TS::Children, $pc)
}
function Win($pid_, $name) { foreach ($w in (Top $pid_)) { if ($w.Current.Name -eq $name -or $w.Current.Name -like "$name*") { return $w } } return $null }
function El($root, $name, $type) {
  $c = New-Object System.Windows.Automation.PropertyCondition($UA::NameProperty, $name)
  if ($type) { $c = New-Object System.Windows.Automation.AndCondition($c, (New-Object System.Windows.Automation.PropertyCondition($UA::ControlTypeProperty, $type))) }
  $root.FindFirst($TS::Descendants, $c)
}
$SW = [System.Diagnostics.Stopwatch]::StartNew()
function Mark($what) { Say ("t:" + $what + " " + [int]$SW.Elapsed.TotalMilliseconds) }   # timing evidence (ms since the driver started)
function Wait-For([scriptblock]$f, $secs) { $end = (Get-Date).AddSeconds($secs); $ms = 60; while ((Get-Date) -lt $end) { $r = $null; try { $r = & $f } catch {}; if ($r) { return $r }; Start-Sleep -Milliseconds $ms; if ($ms -lt 300) { $ms = [int]($ms * 1.6) } }; return $null }   # fast polls first, backing off to 300 ms
function Find-Main { foreach ($w in (Top $DolphinPid)) { if ($w.Current.ClassName -eq "MainWindow" -or $w.Current.Name -match '^Dolphin \S+$') { return $w } } }
function Click($el) { $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
$script:bufSet = ""
function Set-Buf {   # host: type the hub's pad buffer (BufFile) into the NetPlay window's Buffer box when it changed; Dolphin tells every player
  if (-not $BufFile -or -not $np -or -not (Test-Path -LiteralPath $BufFile)) { return }
  try {
    $b = [int]([System.IO.File]::ReadAllText($BufFile).Trim())
    if ("$b" -eq $script:bufSet) { return }
    $sp = $np.FindFirst($TS::Descendants, (New-Object System.Windows.Automation.PropertyCondition($UA::ControlTypeProperty, $CT::Spinner)))
    if (-not $sp) { $sp = $np.FindFirst($TS::Descendants, (New-Object System.Windows.Automation.PropertyCondition($UA::ControlTypeProperty, $CT::Edit))) }
    $done = $false
    try { $sp.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern).SetValue([double]$b); $done = $true } catch {}
    if (-not $done) { $sp.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue("$b") }
    $script:bufSet = "$b"
    Say "buffer:$b"
  } catch { Say "buffer:unset" }
}
function Mini($w) { try { $w.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).SetWindowVisualState([System.Windows.Automation.WindowVisualState]::Minimized) } catch {} }
try {
  Say "stage:driver-start"
  function Need($el, $what) { if (-not $el) { throw "Dolphin's $what was not found (Dolphin changed or was still starting)" }; $el }
  if ($DelayS -gt 0) { Start-Sleep $DelayS }
  # Wait up to 120 s for the main window AND its Tools menu; the window can be recreated while Dolphin starts (title/class change, a splash), so re-find it every poll. A dead Dolphin ends the wait.
  $main = $null; $tools = $null
  $endM = (Get-Date).AddSeconds(120); $msM = 60
  while ((Get-Date) -lt $endM) {
    if (-not (Get-Process -Id $DolphinPid -ErrorAction SilentlyContinue)) { Say "error:Dolphin closed while starting"; exit 1 }
    try { $main = Find-Main; if ($main) { $tools = El $main "Tools" $CT::MenuItem; if ($tools) { break } } } catch { $main = $null; $tools = $null }
    Start-Sleep -Milliseconds $msM; if ($msM -lt 400) { $msM = [int]($msM * 1.5) }
  }
  if (-not $main) { Say "error:no Dolphin main window"; exit 1 }
  Need $tools "Tools menu" | Out-Null
  Mark "tools-menu"
  $dlg = $null
  for ($try = 1; $try -le 3 -and -not $dlg; $try++) {
    try { $tools.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand() } catch {}
    $it = Wait-For { El $tools "Start NetPlay..." $CT::MenuItem } 8
    if ($it) { Click $it }
    $dlg = Wait-For { El $main "NetPlay Setup" $null } 10
    if (-not $dlg) { Say "retry:open-netplay-dialog $try"; try { $main = Find-Main; $tools = El $main "Tools" $CT::MenuItem } catch {} }
  }
  Mark "setup-dialog"
  if ($Role -ne "host" -and $GoFile) { if (-not (Wait-For { Test-Path -LiteralPath $GoFile } 180)) { Say "error:the host's NetPlay server never became ready"; exit 1 }; Mark "go" }
  if (-not $dlg) { Say "error:Dolphin's NetPlay Setup dialog did not open"; exit 1 }
  if ($Role -eq "host") {
    (Need (El $dlg "Host" $CT::TabItem) "Host tab").GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Mark "host-tab"
    $item = Wait-For { $c = New-Object System.Windows.Automation.PropertyCondition($UA::ControlTypeProperty, $CT::ListItem); $dlg.FindAll($TS::Descendants, $c) | Where-Object { $_.Current.Name -like "*$Suffix" } | Select-Object -First 1 } 20
    if (-not $item) { Say "error:game $Suffix not in Dolphin's list (is its folder scanned?)"; exit 1 }
    $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Mark "game-selected"
    Click (Need (El $dlg "Host" $CT::Button) "Host button")
    $np = Wait-For { $w = Win $DolphinPid "NetPlay"; if ($w -and $w.Current.Name -eq "NetPlay") { $w } } 40
  } else {
    # The host's NetPlay server may not be forwarding yet (or the first UDP packets get lost): retry the connect a few times
    $np = $null
    for ($try = 1; $try -le 4 -and -not $np; $try++) {
      if ($try -gt 1) {
        Say "retry:connect $try"
        foreach ($w in (Top $DolphinPid)) { if ($w.Current.Name -ne "NetPlay Setup" -and $w.Current.ClassName -ne "MainWindow") { $ok = El $w "OK" $CT::Button; if ($ok) { try { Click $ok } catch {} } } }   # dismiss Dolphin's "failed to connect" box
        Start-Sleep -Milliseconds 700
        $dlg = Wait-For { El $main "NetPlay Setup" $null } 5
        if (-not $dlg) { $tools.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand(); Click (Need (El $tools "Start NetPlay..." $CT::MenuItem) "Start NetPlay item"); $dlg = Wait-For { El $main "NetPlay Setup" $null } 10 }
      }
      if (-not $dlg) { break }
      Click (Need (El $dlg "Connect" $CT::Button) "Connect button")
      # fail fast: Dolphin's "failed to connect" box (any extra top-level window with an OK button) ends the wait for this attempt
      $np = Wait-For { $w = Win $DolphinPid "NetPlay"; if ($w -and $w.Current.Name -eq "NetPlay") { return $w }; foreach ($x in (Top $DolphinPid)) { if ($x.Current.Name -ne "NetPlay Setup" -and $x.Current.ClassName -ne "MainWindow" -and $x.Current.Name -ne "NetPlay") { if (El $x "OK" $CT::Button) { return "ERR" } } } } 20
      if ($np -is [string]) { $np = $null }
    }
  }
  if (-not $np) { Say "error:Dolphin could not connect to the host's NetPlay server (it did not answer; the host may have closed it)"; exit 1 }
  Say "stage:netplay-open"
  Mark "netplay-open"
  Mini $np
  Mark "minimized"
  if ($Role -eq "host") {
    $c = New-Object System.Windows.Automation.PropertyCondition($UA::ControlTypeProperty, $CT::Text)
    if (-not $NoCode) {   # direct mode has no host code to read
      $code = Wait-For { foreach ($t in $np.FindAll($TS::Descendants, $c)) { $n = $t.Current.Name; if ($n -match '^[0-9A-Fa-f]{8}$') { return $n } } } 60
      if ($code) { Say "code:$code" }
    }
    $end = (Get-Date).AddSeconds($TimeoutS)
    $rows = 0; $lastRows = -1
    while ((Get-Date) -lt $end) {
      $tbl = $np.FindFirst($TS::Descendants, (New-Object System.Windows.Automation.PropertyCondition($UA::ControlTypeProperty, $CT::Table)))
      try { $rows = $tbl.GetCurrentPattern([System.Windows.Automation.GridPattern]::Pattern).Current.RowCount } catch { $rows = 0 }
      if ($rows -ne $lastRows) { Say "players:$rows"; $lastRows = $rows }
      if ($rows -ge $Players) { break }
      Start-Sleep -Milliseconds 300
    }
    Mark "players-ready"
    try {   # evidence for pad-port order: the player table (name, ping, pad mapping) in connection order
      $tbl2 = $np.FindFirst($TS::Descendants, (New-Object System.Windows.Automation.PropertyCondition($UA::ControlTypeProperty, $CT::Table)))
      $cells = @(); foreach ($e in $tbl2.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)) { if ($e.Current.Name) { $cells += $e.Current.Name } }
      Say ("table:" + ($cells -join "|"))
    } catch { Say "table:unreadable" }
    if ($rows -lt $Players) { Say "error:only $rows of $Players players joined the NetPlay lobby, not starting"; exit 1 }
    Start-Sleep -Milliseconds 800   # let the last client finish its join handshake (Start before that excludes it)
    Set-Buf   # pad buffer from the session's measured ping (the host's value is the room's)
    Click (Need (El $np "Start" $CT::Button) "Start button")
    Say "stage:started"
  } else {
    Say "stage:joined"
    Mark "joined"
  }
  # watch: a RenderWidget window means the game runs; keep the chat for desync reports
  $g = Wait-For { foreach ($w in (Top $DolphinPid)) { if ($w.Current.ClassName -eq "RenderWidget") { return $w } } } 90
  if ($g) { Say "stage:in-game"; Mark "in-game" } else { Say "error:the game did not start in Dolphin (disc mismatch or the host did not press Start)" }
  $t0 = Get-Date
  $beat = 0
  $lastBuf = ""
  while (Get-Process -Id $DolphinPid -ErrorAction SilentlyContinue) {
    $beat += 5
    if ($beat % 15 -eq 0) { Say ("alive:" + [int]((Get-Date) - $t0).TotalSeconds) }   # heartbeat: seconds since the game started
    if ($Role -eq "host") { Set-Buf }   # the ping changed during the game: Dolphin applies a new buffer at once
    try {
      $chat = El $np "Chat" $CT::Text
      $txt = $chat.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern).DocumentRange.GetText(-1)
      if ($txt -match '(?i)desync|out of sync') { Say ("desync:" + ($txt -replace '\s+', ' ')) }
      $bm = [regex]::Matches($txt, 'Buffer size changed to (\d+)'); if ($bm.Count -gt 0) { $bv = $bm[$bm.Count - 1].Groups[1].Value; if ($bv -ne $lastBuf) { $lastBuf = $bv; Say ("padbuffer:" + $bv) } }   # the room's real pad buffer (Dolphin's chat)
    } catch {}
    Start-Sleep 5
  }
  Say "stage:ended"
} catch { Say "error:$($_.Exception.Message)" }
