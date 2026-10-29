#include "winutil.h"
#include "netplay_dolphin.h"

#include <cctype>
#include <cstdio>
#include <cstring>
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
    o += ",\"players\":" + std::to_string(s.players);
    o += ",\"disc\":" + std::to_string(s.disc) + ",\"rev\":" + std::to_string(s.rev) + "}";
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
    if (find_value(j, "disc", 0, &v, &n, &st) && !st && n >= 0 && n < 256) s.disc = (int)n;
    if (find_value(j, "rev", 0, &v, &n, &st) && !st && n >= 0 && n < 256) s.rev = (int)n;
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

DiscVer disc_version_from_header(const unsigned char* h, size_t n) {
    DiscVer d;
    if (n < 0x20 || !(h[0x1C] == 0xC2 && h[0x1D] == 0x33 && h[0x1E] == 0x9F && h[0x1F] == 0x3D)) return d;   // GameCube magic 0xC2339F3D
    for (int i = 0; i < 6; ++i) if (!isalnum(h[i])) return d;
    d.id.assign((const char*)h, 6);
    d.disc = h[6]; d.rev = h[7];
    d.ok = true;
    return d;
}

static bool read_range(const std::string& path, uint64_t off, size_t len, std::vector<unsigned char>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg((std::streamoff)off);
    out->assign(len, 0);
    f.read((char*)out->data(), (std::streamsize)len);
    return (size_t)f.gcount() == len;
}

DiscVer disc_version(const std::string& path) {
    size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    std::vector<unsigned char> h;
    if (ext == ".iso" || ext == ".gcm") { if (read_range(path, 0, 0x80, &h)) return disc_version_from_header(h.data(), h.size()); }
    else if (ext == ".ciso") {
        if (read_range(path, 0, 0x8008, &h) && std::memcmp(h.data(), "CISO", 4) == 0 && h[8] == 1 && read_range(path, 0x8000, 0x80, &h)) return disc_version_from_header(h.data(), h.size());   // block 0 holds the header
    } else if (ext == ".rvz" || ext == ".wia") {
        if (read_range(path, 0, 0x58 + 0x80, &h) && (std::memcmp(h.data(), "RVZ\x01", 4) == 0 || std::memcmp(h.data(), "WIA\x01", 4) == 0)) return disc_version_from_header(h.data() + 0x58, 0x80);
    }
    return DiscVer();
}

std::string version_diff(const Spec& host, const DiscVer& mine) {
    if (!mine.ok) return "";
    std::string host_id = host.game_id;
    if (!host_id.empty() && host_id != mine.id) return "a different game (" + mine.id + " vs " + host_id + ")";
    if (host.disc >= 0 && host.disc != mine.disc) return "a different disc of this game (disc " + std::to_string(mine.disc + 1) + " vs " + std::to_string(host.disc + 1) + ")";
    if (host.rev >= 0 && host.rev != mine.rev) return "a different version of this game (revision " + std::to_string(mine.rev) + " vs " + std::to_string(host.rev) + ")";
    return "";
}

Match compare_version(const Spec& host, const DiscVer& mine) {
    if (!mine.ok) return Match::Unknown;
    return version_diff(host, mine).empty() ? Match::Same : Match::DifferentDisc;
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
    /* A name may hold spaces and symbols; keep it to one printable line (control characters would break the INI). */
    std::string nick;
    for (unsigned char c : p.nickname) if (c >= 0x20 && c != 0x7F) nick += (char)c;
    np.push_back({"Nickname", nick.empty() ? "Player" : nick});
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
    np.push_back({"WriteSaveData", p.host && p.write_save ? "True" : "False"});
    /* Dolphin copies the host's emulation settings to every client (CPU thread/core, cheats, DSP HLE, overclock, sync GPU, MMU,
     * fastmem, EXI devices...). Not host-chosen and default off: strict graphics sync (internal resolution / EFB scaling differ per
     * player in RetroHub), so force it on; Gecko codes come from the host (SyncCodes). */
    np.push_back({"StrictSettingsSync", "True"});
    np.push_back({"SyncCodes", "True"});
    KV gen, iface, an;
    if (!p.iso_dir.empty()) { gen.push_back({"ISOPath0", p.iso_dir}); gen.push_back({"ISOPaths", "1"}); }
    iface.push_back({"ConfirmStop", "False"});
    iface.push_back({"UsePanicHandlers", "False"});
    an.push_back({"Enabled", "False"});
    an.push_back({"PermissionAsked", "True"});
    KV core;
    core.push_back({"GPUDeterminismMode", "auto"});   // the one emulation setting NetPlay does not copy from the host
    /* Deterministic emulation on every PC (the host's values are copied to the guests, the guests' are ignored but kept equal):
     * single CPU thread (dual core is the classic NetPlay desync), JIT64 on all, no overclock, no sync-GPU/MMU/fast disc speed surprises. */
    core.push_back({"CPUThread", "False"});
    core.push_back({"CPUCore", "1"});
    core.push_back({"OverclockEnable", "False"});
    core.push_back({"VIOverclockEnable", "False"});
    core.push_back({"MMU", "False"});
    core.push_back({"SyncGPU", "False"});
    core.push_back({"FastDiscSpeed", "False"});
    core.push_back({"DSPHLE", "True"});
    std::vector<std::pair<std::string, KV>> want = {{"NetPlay", np}, {"Core", core}, {"General", gen}, {"Interface", iface}, {"Analytics", an}, {"AutoUpdate", KV{{"UpdateTrack", ""}}}};
    if (p.mute) { want.push_back({"DSP", KV{{"Backend", "No Audio Output"}}}); }
    return ini_set_all(ini, want);
}

/* ---------- UIA driver ---------- */
const char* driver_script() {
    return R"PS(
param([string]$Role, [int]$DolphinPid, [string]$Status, [string]$Suffix, [int]$Players = 2, [int]$DelayS = 0, [int]$TimeoutS = 150, [string]$Code = "", [int]$NoCode = 0, [string]$GoFile = "")
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
  while (Get-Process -Id $DolphinPid -ErrorAction SilentlyContinue) {
    $beat += 5
    if ($beat % 15 -eq 0) { Say ("alive:" + [int]((Get-Date) - $t0).TotalSeconds) }   # heartbeat: seconds since the game started
    try {
      $chat = El $np "Chat" $CT::Text
      $txt = $chat.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern).DocumentRange.GetText(-1)
      if ($txt -match '(?i)desync|out of sync') { Say ("desync:" + ($txt -replace '\s+', ' ')) }
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
    /* CREATE_NO_WINDOW: the NetPlay driver is powershell.exe, a console program. Without it Windows allocates a console, and on
     * PCs where Windows Terminal is the default console host that pops a visible terminal window on the user's screen (even with
     * -WindowStyle Hidden, and even when lpDesktop points elsewhere). GUI programs (Dolphin) ignore the flag. */
    if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) return false;
    *pid = pi.dwProcessId;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

bool start(bool host, const std::string& dolphin_exe, const std::string& user_dir, const Spec& s,
           int my_slot, const std::string& work_dir, const std::string& desktop, const std::string& extra_args, Run* out) {
    CreateDirectoryA(work_dir.c_str(), nullptr);
    std::string script = work_dir + "\\gc_netplay_driver.ps1";
    { std::ofstream f(script, std::ios::binary); f << driver_script(); }
    out->go_path = work_dir + "\\gc_guest_go.txt";
    DeleteFileA(out->go_path.c_str());
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
                       list_item_suffix(s) + "\" -Players " + std::to_string(s.players) + " -DelayS " + std::to_string(host ? 0 : guest_connect_delay_s(my_slot)) + (s.mode == "direct" ? " -NoCode 1" : "") + (!host && out->wait_go ? " -GoFile \"" + out->go_path + "\"" : "");
    if (!spawn(L"powershell.exe " + W(args), W(work_dir), wd, false, &out->driver_pid)) { out->error = "could not start the NetPlay driver"; return false; }
    return true;
}

void signal_go(const Run& r) {
    if (r.go_path.empty()) return;
    HANDLE h = CreateFileA(r.go_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
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
