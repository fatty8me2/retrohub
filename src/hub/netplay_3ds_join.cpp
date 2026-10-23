#include "netplay_3ds_join.h"

#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hubnp3ds {

const char* join_driver_script() {
    return R"PS(
param([int]$AzPid, [string]$Status, [string]$Nick, [string]$Pw = "", [string]$Addr = "127.0.0.1", [int]$Port = 24872, [int]$TimeoutS = 120)
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$UA = [System.Windows.Automation.AutomationElement]; $TS = [System.Windows.Automation.TreeScope]
$CT = [System.Windows.Automation.ControlType]
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
  $edits = @($dlg.FindAll($TS::Descendants, (Prop $UA::ControlTypeProperty $CT::Edit)))
  if ($edits.Count -lt 4) { Say "error:Direct Connect dialog layout changed"; exit 1 }
  $vals = @($Addr, "$Port", $Nick, $Pw)
  for ($i = 0; $i -lt 4; $i++) { $edits[$i].GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($vals[$i]) }
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
      if ($w.Current.ClassName -ne "DirectConnectWindow") { return ("dialog:" + $w.Current.Name) }
    }
  } 40
  if ($ok -eq "joined") { Say "joined" } elseif ($ok) { Say "error:$ok" } else { Say "error:join timed out" }
} catch { Say "error:$($_.Exception.Message)" }
)PS";
}

std::string join_driver_args(unsigned long pid, const std::string& status, const std::string& nick, const std::string& pw,
                             const std::string& host, int port, int timeout_s) {
    return "-AzPid " + std::to_string(pid) + " -Status \"" + status + "\" -Nick \"" + nick + "\" -Pw \"" + pw + "\" -Addr \"" + host +
           "\" -Port " + std::to_string(port) + " -TimeoutS " + std::to_string(timeout_s);
}

std::string join_state(const std::string& text) {
    std::istringstream is(text);
    std::string l, st;
    while (std::getline(is, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l == "joined" || l.rfind("error:", 0) == 0) st = l;
    }
    return st;
}

#ifdef _WIN32
unsigned long start_join_driver(unsigned long azahar_pid, const std::string& work_dir, const std::string& nick, const std::string& pw,
                                const std::string& host, int port, const std::wstring& desktop, std::string* status_file) {
    CreateDirectoryA(work_dir.c_str(), nullptr);
    std::string script = work_dir + "\\azahar_join.ps1", status = work_dir + "\\azahar_join_" + std::to_string(azahar_pid) + ".txt";
    { std::ofstream f(script, std::ios::binary); f << join_driver_script(); }
    DeleteFileA(status.c_str());
    if (status_file) *status_file = status;
    std::string args = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + script + "\" " +
                       join_driver_args(azahar_pid, status, nick, pw, host, port, 120);
    std::wstring cmd(args.begin(), args.end()), desk = desktop;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    if (!desk.empty()) si.lpDesktop = &desk[0];
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pi.dwProcessId;
}
#endif

}  // namespace hubnp3ds
