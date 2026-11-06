param([int]$AzPid, [string]$Status, [string]$Nick, [string]$Pw = "", [string]$Addr = "127.0.0.1", [int]$Port = 24872, [int]$TimeoutS = 120)
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$UA = [System.Windows.Automation.AutomationElement]; $TS = [System.Windows.Automation.TreeScope]
$CT = [System.Windows.Automation.ControlType]
Add-Type -Namespace W -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);'
Add-Type -Namespace W -Name P -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetWindowPos(System.IntPtr h, System.IntPtr a, int x, int y, int cx, int cy, uint f);'
# SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE: park a window far off-screen without ever activating it
function Park($el) { try { $h = [IntPtr]$el.Current.NativeWindowHandle; if ($h -ne [IntPtr]::Zero) { [void][W.P]::SetWindowPos($h, [IntPtr]::Zero, -32000, -32000, 0, 0, 0x0215) } } catch {} }
function Read-Edit($e) { try { return $e.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value } catch { return $null } }
# Qt's QLineEdit often rejects ValuePattern.SetValue: fall back to focus + WM_CHAR posted to the dialog's own HWND
# (works on a hidden desktop, unlike SendInput which only reaches the input desktop).
function Set-Edit($dlg, $e, [string]$v) {
  if ((Read-Edit $e) -eq $v) { return $true }   # prefilled from the seeded qt-config.ini
  try { $e.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($v) } catch {}
  if ((Read-Edit $e) -eq $v) { return $true }
  $h = [IntPtr]$dlg.Current.NativeWindowHandle
  if ($h -eq [IntPtr]::Zero) { return $false }
  try { $e.SetFocus() } catch {}
  Start-Sleep -Milliseconds 150
  1..48 | ForEach-Object { [void][W.U]::PostMessage($h, 0x102, [IntPtr]8, [IntPtr]0) }
  foreach ($c in $v.ToCharArray()) { [void][W.U]::PostMessage($h, 0x102, [IntPtr][int]$c, [IntPtr]0) }
  Start-Sleep -Milliseconds 250
  return ((Read-Edit $e) -eq $v)
}
function Say($t) { Add-Content -Encoding ascii $Status $t }
function Prop($p, $v) { New-Object System.Windows.Automation.PropertyCondition($p, $v) }
function Main() {
  foreach ($w in $UA::RootElement.FindAll($TS::Children, (Prop $UA::ProcessIdProperty $AzPid))) {
    if ($w.Current.ClassName -eq "GMainWindow") { return $w }
  }
  return $null
}
function Wait-For([scriptblock]$f, $secs) { $end = (Get-Date).AddSeconds($secs); while ((Get-Date) -lt $end) { $r = & $f; if ($r) { return $r }; Start-Sleep -Milliseconds 500 }; return $null }
function Dlg($main, $cls) { foreach ($w in $main.FindAll($TS::Children, (Prop $UA::ControlTypeProperty $CT::Window))) { if ($w.Current.ClassName -eq $cls) { return $w } } return $null }
try {
  Say "stage:driver-start"
  # the menu is only usable once a game is booted (title "Azahar x | Game")
  $main = Wait-For { $m = Main; if ($m -and $m.Current.Name -match '\|') { $m } } $TimeoutS
  if (-not $main) { Say "error:Azahar did not boot a game"; exit 1 }
  Start-Sleep -Seconds 3
  $mp = $main.FindFirst($TS::Descendants, (Prop $UA::NameProperty "Multiplayer"))
  if (-not $mp) { Say "error:no Multiplayer menu"; exit 1 }
  $mp.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
  Start-Sleep -Milliseconds 600
  $it = $mp.FindFirst($TS::Descendants, (Prop $UA::NameProperty "Direct Connect to Room"))
  if (-not $it) { Say "error:no Direct Connect item"; exit 1 }
  $it.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  $dlg = Wait-For { Dlg $main "DirectConnectWindow" } 15
  if (-not $dlg) { Say "error:Direct Connect dialog did not open"; exit 1 }
  Park $dlg
  $edits = @($dlg.FindAll($TS::Descendants, (Prop $UA::ControlTypeProperty $CT::Edit)))
  if ($edits.Count -lt 4) { Say "error:Direct Connect dialog layout changed"; exit 1 }
  Say ("stage:nickname=" + (Read-Edit $edits[2]))
  $vals = @($Addr, "$Port", $Nick, $Pw)
  # the nickname box is read-only in Azahar (it shows the 3DS username); the password box is only touched when needed
  for ($i = 0; $i -lt 4; $i++) { if ($i -eq 2 -or ($i -eq 3 -and -not $Pw)) { continue }; if (-not (Set-Edit $dlg $edits[$i] $vals[$i])) { Say "stage:edit$i-unset" } }
  Start-Sleep -Milliseconds 300
  $btn = $dlg.FindFirst($TS::Descendants, (Prop $UA::NameProperty "Connect"))
  $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  Say "stage:connect-clicked"
  # joined when "Leave Room" becomes enabled; a failure raises a message box child window
  $ok = Wait-For {
    $m = Main
    if (-not $m) { return "gone" }
    $l = $m.FindFirst($TS::Descendants, (Prop $UA::NameProperty "Leave Room"))
    if ($l -and $l.Current.IsEnabled) { return "joined" }
    foreach ($w in $m.FindAll($TS::Children, (Prop $UA::ControlTypeProperty $CT::Window))) {
      if ($w.Current.Name -match 'members\) - connected') { return "joined" }   # the multiplayer room window: "RetroHub (2/8 members) - connected"
      if ($w.Current.ClassName -ne "DirectConnectWindow") { Park $w; return ("dialog:" + $w.Current.Name) }
    }
  } 40
  if ($ok -eq "joined") { Say "joined" } else {
    # report what Azahar's own error box says (it may be a top-level window rather than a child of the main window)
    $txt = @()
    foreach ($w in $UA::RootElement.FindAll($TS::Children, (Prop $UA::ProcessIdProperty $AzPid))) {
      if ($w.Current.Name -eq "Error") { Park $w; foreach ($t in $w.FindAll($TS::Descendants, (Prop $UA::ControlTypeProperty $CT::Text))) { $txt += $t.Current.Name } }
    }
    $why = if ($ok) { "$ok" } else { "join timed out" }
    if ($txt.Count) { $why += " [" + ($txt -join " | ") + "]" }
    Say "error:$why"
  }
} catch { Say "error:$($_.Exception.Message)" }
