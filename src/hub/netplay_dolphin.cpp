#include "winutil.h"
#include "netplay_dolphin.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace gcnp {

/* ---------- tiny flat JSON (the np_gc object has only strings and numbers) ---------- */
static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

std::string spec_json(const Spec& s) {
    std::string o = "{\"mode\":\"" + esc(s.mode) + "\"";
    o += ",\"host_code\":\"" + esc(s.host_code) + "\"";
    o += ",\"addr\":\"" + esc(s.addr) + "\"";
    o += ",\"port\":" + std::to_string(s.port);
    o += ",\"game_id\":\"" + esc(s.game_id) + "\"";
    o += ",\"title\":\"" + esc(s.title) + "\"";
    o += ",\"sha256\":\"" + esc(s.sha256) + "\"";
    o += ",\"size\":" + std::to_string((unsigned long long)s.size);
    o += ",\"players\":" + std::to_string(s.players) + "}";
    return o;
}

static bool find_value(const std::string& j, const std::string& key, size_t from, std::string* str, double* num, bool* is_str) {
    std::string k = "\"" + key + "\"";
    size_t p = j.find(k, from);
    if (p == std::string::npos) return false;
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && isspace((unsigned char)j[p])) ++p;
    if (p >= j.size()) return false;
    if (j[p] == '"') {
        std::string v;
        for (++p; p < j.size() && j[p] != '"'; ++p) {
            if (j[p] == '\\' && p + 1 < j.size()) ++p;
            v += j[p];
        }
        *str = v; *is_str = true;
        return true;
    }
    *num = atof(j.c_str() + p); *is_str = false;
    return true;
}

bool spec_parse(const std::string& j, Spec* out) {
    Spec s;
    std::string v; double n = 0; bool st = false;
    if (!find_value(j, "mode", 0, &v, &n, &st) || !st) return false;
    if (v != "traversal" && v != "direct") return false;
    s.mode = v;
    if (find_value(j, "host_code", 0, &v, &n, &st) && st) s.host_code = v;
    if (find_value(j, "addr", 0, &v, &n, &st) && st) s.addr = v;
    if (find_value(j, "port", 0, &v, &n, &st) && !st && n >= 1 && n <= 65535) s.port = (int)n;
    if (!find_value(j, "game_id", 0, &v, &n, &st) || !st || v.size() < 4 || v.size() > 6) return false;
    s.game_id = v;
    for (char c : v) if (!isalnum((unsigned char)c)) return false;
    if (find_value(j, "title", 0, &v, &n, &st) && st) s.title = v;
    if (find_value(j, "sha256", 0, &v, &n, &st) && st) s.sha256 = v;
    if (find_value(j, "size", 0, &v, &n, &st) && !st && n >= 0) s.size = (uint64_t)n;
    if (find_value(j, "players", 0, &v, &n, &st) && !st && n >= 2 && n <= 4) s.players = (int)n;
    if (s.mode == "direct" && s.addr.empty()) return false;
    if (!s.host_code.empty() && !valid_host_code(s.host_code)) return false;
    *out = s;
    return true;
}

std::string launch_fragment(const Spec& s) { return ",\"np_gc\":" + spec_json(s); }

std::string add_to_launch(const std::string& launch, const Spec& s) {
    size_t e = launch.rfind('}');
    if (e == std::string::npos) return launch;
    std::string head = launch.substr(0, e);
    return head + ",\"np_gc\":" + spec_json(s) + "}";
}

std::string extract_from_launch(const std::string& j) {
    size_t p = j.find("\"np_gc\"");
    if (p == std::string::npos) return "";
    p = j.find('{', p);
    if (p == std::string::npos) return "";
    int depth = 0; bool instr = false;
    for (size_t i = p; i < j.size(); ++i) {
        char c = j[i];
        if (instr) { if (c == '\\') ++i; else if (c == '"') instr = false; continue; }
        if (c == '"') instr = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return j.substr(p, i - p + 1);
    }
    return "";
}

Match compare_disc(const Spec& host, uint64_t my_size, const std::string& my_sha) {
    if (host.sha256.empty() && host.size == 0) return Match::Unknown;
    if (host.size != 0 && my_size != host.size) return Match::DifferentDisc;
    if (!host.sha256.empty() && !my_sha.empty() && host.sha256 != my_sha) return Match::DifferentDisc;
    return Match::Same;
}

const char* match_text(Match m) {
    switch (m) {
        case Match::Same: return "Same game file as the host.";
        case Match::DifferentDisc: return "Your game file is not identical to the host's (different dump or format). Dolphin NetPlay may refuse it or desync; unpack the same archive as the host.";
        default: return "The host sent no game fingerprint.";
    }
}

bool valid_host_code(const std::string& s) {
    if (s.size() != 8) return false;
    for (char c : s) if (!isxdigit((unsigned char)c)) return false;
    return true;
}

std::string list_item_suffix(const Spec& s) { return "(" + s.game_id + ")"; }

int guest_connect_delay_s(int slot) { return slot <= 1 ? 0 : (slot - 1) * 4; }

/* ---------- ini patch ---------- */
static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::string ini_set_all(const std::string& ini, const std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>& want) {
    std::vector<std::string> lines;
    { std::istringstream is(ini); std::string l; while (std::getline(is, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); lines.push_back(l); } }
    for (auto& sec : want) {
        std::string hdr = "[" + sec.first + "]";
        int start = -1, end = (int)lines.size();
        for (int i = 0; i < (int)lines.size(); ++i) {
            std::string t = trim(lines[i]);
            if (t == hdr) { start = i; continue; }
            if (start >= 0 && !t.empty() && t[0] == '[') { end = i; break; }
        }
        if (start < 0) { lines.push_back(hdr); start = (int)lines.size() - 1; end = (int)lines.size(); }
        for (auto& kv : sec.second) {
            bool done = false;
            for (int i = start + 1; i < end; ++i) {
                std::string t = trim(lines[i]);
                size_t eq = t.find('=');
                if (eq != std::string::npos && trim(t.substr(0, eq)) == kv.first) { lines[i] = kv.first + " = " + kv.second; done = true; break; }
            }
            if (!done) { lines.insert(lines.begin() + end, kv.first + " = " + kv.second); ++end; }
        }
    }
    std::string o;
    for (auto& l : lines) o += l + "\n";
    return o;
}

std::string ini_patch(const std::string& ini, const Spec& s, const IniParams& p) {
    typedef std::vector<std::pair<std::string, std::string>> KV;
    KV np;
    np.push_back({"Nickname", p.nickname.empty() ? "Player" : p.nickname});
    np.push_back({"TraversalChoice", s.mode == "direct" ? "direct" : "traversal"});
    if (s.mode == "direct") {
        np.push_back({"Address", s.addr});
        np.push_back({"ConnectPort", std::to_string(s.port)});
        np.push_back({"HostPort", std::to_string(s.port)});
    } else if (!p.host && !s.host_code.empty()) {
        np.push_back({"HostCode", s.host_code});
    }
    np.push_back({"UseIndex", "False"});
    np.push_back({"SyncSaves", "True"});
    np.push_back({"WriteSaveData", p.host ? "True" : "False"});
    KV gen, iface, an;
    if (!p.iso_dir.empty()) { gen.push_back({"ISOPath0", p.iso_dir}); gen.push_back({"ISOPaths", "1"}); }
    iface.push_back({"ConfirmStop", "False"});
    iface.push_back({"UsePanicHandlers", "False"});
    an.push_back({"Enabled", "False"});
    an.push_back({"PermissionAsked", "True"});
    std::vector<std::pair<std::string, KV>> want = {{"NetPlay", np}, {"General", gen}, {"Interface", iface}, {"Analytics", an}, {"AutoUpdate", KV{{"UpdateTrack", ""}}}};
    if (p.mute) { want.push_back({"DSP", KV{{"Backend", "No Audio Output"}}}); }
    return ini_set_all(ini, want);
}

/* ---------- UIA driver ---------- */
const char* driver_script() {
    return R"PS(
param([string]$Role, [int]$DolphinPid, [string]$Status, [string]$Suffix, [int]$Players = 2, [int]$DelayS = 0, [int]$TimeoutS = 150, [string]$Code = "")
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$UA = [System.Windows.Automation.AutomationElement]; $TS = [System.Windows.Automation.TreeScope]
$CT = [System.Windows.Automation.ControlType]
function Say($t) { Add-Content -Encoding ascii $Status $t }
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
function Wait-For([scriptblock]$f, $secs) { $end = (Get-Date).AddSeconds($secs); while ((Get-Date) -lt $end) { $r = & $f; if ($r) { return $r }; Start-Sleep -Milliseconds 400 }; return $null }
function Click($el) { $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
function Mini($w) { try { $w.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).SetWindowVisualState([System.Windows.Automation.WindowVisualState]::Minimized) } catch {} }
try {
  Say "stage:driver-start"
  $main = Wait-For { foreach ($w in (Top $DolphinPid)) { if ($w.Current.Name -match '^Dolphin \S+$') { return $w } } } 40
  if (-not $main) { Say "error:no Dolphin main window"; exit 1 }
  if ($DelayS -gt 0) { Start-Sleep $DelayS }
  $tools = El $main "Tools" $CT::MenuItem
  Start-Sleep -Seconds 2   # game list scan
  $tools.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
  Start-Sleep -Milliseconds 500
  Click (El $tools "Start NetPlay..." $CT::MenuItem)
  $dlg = Wait-For { El $main "NetPlay Setup" $null } 10
  if (-not $dlg) { Say "error:no NetPlay setup dialog"; exit 1 }
  if ($Role -eq "host") {
    (El $dlg "Host" $CT::TabItem).GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Start-Sleep -Milliseconds 600
    $item = Wait-For { $c = New-Object System.Windows.Automation.PropertyCondition($UA::ControlTypeProperty, $CT::ListItem); $dlg.FindAll($TS::Descendants, $c) | Where-Object { $_.Current.Name -like "*$Suffix" } | Select-Object -First 1 } 20
    if (-not $item) { Say "error:game $Suffix not in Dolphin's list (is its folder scanned?)"; exit 1 }
    $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Start-Sleep -Milliseconds 400
    Click (El $dlg "Host" $CT::Button)
  } else {
    Click (El $dlg "Connect" $CT::Button)
  }
  $np = Wait-For { $w = Win $DolphinPid "NetPlay"; if ($w -and $w.Current.Name -eq "NetPlay") { $w } } 40
  if (-not $np) { Say "error:NetPlay window did not open (connection refused or timed out)"; exit 1 }
  Say "stage:netplay-open"
  Mini $np
  if ($Role -eq "host") {
    $c = New-Object System.Windows.Automation.PropertyCondition($UA::ControlTypeProperty, $CT::Text)
    $code = Wait-For { foreach ($t in $np.FindAll($TS::Descendants, $c)) { $n = $t.Current.Name; if ($n -match '^[0-9A-Fa-f]{8}$') { return $n } } } 60
    if ($code) { Say "code:$code" }
    $end = (Get-Date).AddSeconds($TimeoutS)
    $rows = 0
    while ((Get-Date) -lt $end) {
      $tbl = $np.FindFirst($TS::Descendants, (New-Object System.Windows.Automation.PropertyCondition($UA::ControlTypeProperty, $CT::Table)))
      try { $rows = $tbl.GetCurrentPattern([System.Windows.Automation.GridPattern]::Pattern).Current.RowCount } catch { $rows = 0 }
      Say "players:$rows"
      if ($rows -ge $Players) { break }
      Start-Sleep 2
    }
    Start-Sleep -Seconds 3
    Click (El $np "Start" $CT::Button)
    Say "stage:started"
  } else {
    Say "stage:joined"
  }
  # watch: a RenderWidget window means the game runs; keep the chat for desync reports
  $g = Wait-For { foreach ($w in (Top $DolphinPid)) { if ($w.Current.ClassName -eq "RenderWidget") { return $w } } } 90
  if ($g) { Say "stage:in-game" } else { Say "error:game did not start" }
  while (Get-Process -Id $DolphinPid -ErrorAction SilentlyContinue) {
    try {
      $chat = El $np "Chat" $CT::Text
      $txt = $chat.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern).DocumentRange.GetText(-1)
      if ($txt -match '(?i)desync') { Say ("desync:" + ($txt -replace '\s+', ' ')) }
    } catch {}
    Start-Sleep 5
  }
  Say "stage:ended"
} catch { Say "error:$($_.Exception.Message)" }
)PS";
}

#ifdef _WIN32
static std::wstring W(const std::string& s) { return winutil::to_wide(s); }

static bool spawn(const std::wstring& cmd, const std::wstring& cwd, const std::wstring& desktop, bool minimized, DWORD* pid) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    std::wstring desk = desktop;
    if (!desk.empty()) si.lpDesktop = &desk[0];
    if (minimized) { si.dwFlags |= STARTF_USESHOWWINDOW; si.wShowWindow = SW_SHOWMINNOACTIVE; }
    PROCESS_INFORMATION pi{};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW * 0, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) return false;
    *pid = pi.dwProcessId;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

bool start(bool host, const std::string& dolphin_exe, const std::string& user_dir, const Spec& s,
           int my_slot, const std::string& work_dir, const std::string& desktop, const std::string& extra_args, Run* out) {
    CreateDirectoryA(work_dir.c_str(), nullptr);
    std::string script = work_dir + "\\gc_netplay_driver.ps1";
    { std::ofstream f(script, std::ios::binary); f << driver_script(); }
    out->status_path = work_dir + std::string(host ? "\\gc_host_status.txt" : "\\gc_guest_status.txt");
    DeleteFileA(out->status_path.c_str());
    std::wstring wd;
    if (!desktop.empty()) {
        wd = W(desktop);
        CreateDesktopW(wd.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    }
    std::string exe_dir = dolphin_exe.substr(0, dolphin_exe.find_last_of("\\/"));
    std::wstring cmd = L"\"" + W(dolphin_exe) + L"\" -u \"" + W(user_dir) + L"\" " + W(extra_args);
    if (!spawn(cmd, W(exe_dir), wd, desktop.empty(), &out->dolphin_pid)) { out->error = "could not start Dolphin"; return false; }
    std::string args = std::string("-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"") + script + "\" -Role " + (host ? "host" : "guest") +
                       " -DolphinPid " + std::to_string(out->dolphin_pid) + " -Status \"" + out->status_path + "\" -Suffix \"" +
                       list_item_suffix(s) + "\" -Players " + std::to_string(s.players) + " -DelayS " + std::to_string(host ? 0 : guest_connect_delay_s(my_slot));
    if (!spawn(L"powershell.exe " + W(args), W(work_dir), wd, false, &out->driver_pid)) { out->error = "could not start the NetPlay driver"; return false; }
    return true;
}

std::string read_status(const Run& r) {
    std::ifstream f(r.status_path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

std::string read_code(const Run& r) {
    std::string st = read_status(r), code;
    std::istringstream is(st); std::string l;
    while (std::getline(is, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l.rfind("code:", 0) == 0 && valid_host_code(l.substr(5))) code = l.substr(5);
    }
    return code;
}
#endif

} // namespace gcnp
