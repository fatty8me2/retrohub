#include "winutil.h"
#include "netplay_dolphin.h"
#include "helper_cmd.h"

#include <cctype>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
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

bool dual_core_allowed(const std::string& game_id) {
    /* Online play is single core (deterministic, stress-tested) unless the game is listed here:
     *  - GYQE01 / GYQP01 / GYQJ01 Mario Superstar Baseball: CPU-bound; on one core it runs far below full speed online while it is fine
     *    offline (dual core), and the slowest PC throttles the whole room.
     * Super Mario Strikers was listed in v1.31.3 but the slowdown was the pad buffer and the forwarder (docs/DOLPHIN.md "Online
     * speed"); it runs at full speed single core, so it is back on the deterministic default. */
    static const char* const ok[] = {"GYQE01", "GYQP01", "GYQJ01"};
    for (const char* id : ok) if (game_id == id) return true;
    return false;
}

int pad_buffer_for_ping(int ping_ms, bool relayed) {
    /* Measured (Mario Superstar Baseball, two Dolphins through the forwarders, session RTT ~170 ms): buffer 8 -> 75 % speed, 10 -> 92 %,
     * 12 -> 100 %, 16 -> 100 %. So about one buffer per 15 ms of round trip (Dolphin's own rule), plus one for jitter. */
    if (ping_ms < 0) return relayed ? 10 : 8;            // unknown (the hub refines it before Start and during the game)
    int b = (ping_ms + 14) / 15 + 1;
    if (relayed && b < 6) b = 6;
    return b < 4 ? 4 : (b > 30 ? 30 : b);
}

int next_pad_buffer(int cur, int want) {
    if (cur < 0 || want > cur) return want;
    return cur - want >= 3 ? want : cur;
}

std::string ini_patch(const std::string& ini, const Spec& s, const IniParams& p) {
    typedef std::vector<std::pair<std::string, std::string>> KV;
    KV np;
    /* A name may hold spaces and symbols; keep it to one printable line and drop the INI-syntax characters ; # [ ] = (control
     * characters would break the file, the others can be read as comments, sections or key/value separators). */
    std::string nick;
    for (unsigned char c : p.nickname) if (c >= 0x20 && c != 0x7F && c != ';' && c != '#' && c != '[' && c != ']' && c != '=') nick += (char)c;
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
    np.push_back({"BufferSize", std::to_string(pad_buffer_for_ping(p.ping_ms, p.relayed))});   // host's pad buffer (Dolphin default 5 stalls relayed rooms)
    KV gen, iface, an;
    if (!p.iso_dir.empty()) { gen.push_back({"ISOPath0", p.iso_dir}); gen.push_back({"ISOPaths", "1"}); }
    iface.push_back({"ConfirmStop", "False"});
    iface.push_back({"UsePanicHandlers", "False"});
    an.push_back({"Enabled", "False"});
    an.push_back({"PermissionAsked", "True"});
    KV core;
    core.push_back({"GPUDeterminismMode", "auto"});   // the one emulation setting NetPlay does not copy from the host
    /* Deterministic emulation on every PC (the host's values are copied to the guests, the guests' are ignored but kept equal):
     * single CPU thread by default (dual core is the classic NetPlay desync; the 140/140 GameCube stress run passed this way), except
     * games on dual_core_allowed(), JIT64 on all, no overclock, no sync-GPU/MMU/fast disc speed surprises. */
    core.push_back({"CPUThread", dual_core_allowed(s.game_id) ? "True" : "False"});
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

static std::string host_card_sync_impl(const std::string& card_dir, const std::string& sync_root, std::string* err) {
    namespace fs = std::filesystem;
    auto fail = [&](const std::string& why) { if (err) *err = why; return std::string(); };
    if (card_dir.empty() || sync_root.size() < 4) return fail("no card folder");
#ifdef _WIN32
    const fs::path src(winutil::to_wide(card_dir)), root(winutil::to_wide(sync_root));
#else
    const fs::path src(card_dir), root(sync_root);
#endif
    /* Only ever wipe our own scratch folder: absolute, named host-sync, not a link, not the card itself or above it. */
    if (!root.is_absolute() || root.filename() != L"host-sync") return fail("unsafe sync folder " + sync_root);
    const std::wstring rs = root.lexically_normal().wstring() + L"\\", cs = src.lexically_normal().wstring() + L"\\";
    if (_wcsnicmp(cs.c_str(), rs.c_str(), rs.size()) == 0) return fail("sync folder contains the card");
#ifdef _WIN32
    const DWORD ra = GetFileAttributesW(root.c_str());
    if (ra != INVALID_FILE_ATTRIBUTES && (ra & FILE_ATTRIBUTE_REPARSE_POINT)) return fail("sync folder is a link");
#endif
    std::error_code ec;
    fs::remove_all(root, ec);   // stale copies must not reach the guests
    if (ec) return fail("cannot clear " + sync_root + ": " + ec.message());
    std::vector<fs::path> gci;
    std::error_code de;
    if (fs::is_directory(src, de))   // a missing card folder (first game) is fine: empty card for everyone
        for (const auto& e : fs::directory_iterator(src, ec)) {
            std::error_code fe;
            if (!e.is_regular_file(fe)) continue;
            std::wstring x = e.path().extension().wstring();
            for (auto& c : x) c = (wchar_t)towlower(c);
            if (x == L".gci") gci.push_back(e.path());
        }
    if (ec) return fail("cannot list " + card_dir + ": " + ec.message());
    for (const char* region : {"USA", "EUR", "JPN"}) {
        const fs::path dir = root / region;
        fs::create_directories(dir, ec);
        if (ec) return fail("cannot create " + winutil::to_utf8(dir.wstring()) + ": " + ec.message());
        for (const fs::path& f : gci) {
            fs::copy_file(f, dir / f.filename(), fs::copy_options::overwrite_existing, ec);
            if (ec) return fail("cannot copy " + winutil::to_utf8(f.filename().wstring()) + ": " + ec.message());
            fs::permissions(dir / f.filename(), fs::perms::owner_write, fs::perm_options::add, ec);   // a read-only save must not block the next wipe
            ec.clear();
        }
    }
    return " -C \"Dolphin.Core.GCIFolderAPath=" + sync_root + "\"";
}

std::string host_card_sync_args(const std::string& card_dir, const std::string& sync_root, std::string* err) {
    try {
        return host_card_sync_impl(card_dir, sync_root, err);
    } catch (const std::exception& x) {   // directory_iterator increments can throw
        if (err) *err = x.what();
        return "";
    }
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

/* Paths are UTF-8 (hub::data_dir()): an accented Windows user name breaks every ANSI call, so file work here is wide. */
static std::filesystem::path P8(const std::string& s) { return std::filesystem::path(W(s)); }

bool start(bool host, const std::string& dolphin_exe, const std::string& user_dir, const Spec& s,
           int my_slot, const std::string& work_dir, const std::string& desktop, const std::string& extra_args, Run* out) {
    CreateDirectoryW(W(work_dir).c_str(), nullptr);
    out->go_path = work_dir + "\\gc_guest_go.txt";
    DeleteFileW(W(out->go_path).c_str());
    out->buf_path = work_dir + "\\gc_host_buffer.txt";
    DeleteFileW(W(out->buf_path).c_str());
    out->status_path = work_dir + std::string(host ? "\\gc_host_status.txt" : "\\gc_guest_status.txt");
    DeleteFileW(W(out->status_path).c_str());
    std::wstring wd;
    if (!desktop.empty()) {
        wd = W(desktop);
        CreateDesktopW(wd.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    }
    std::string exe_dir = dolphin_exe.substr(0, dolphin_exe.find_last_of("\\/"));
    std::wstring cmd = L"\"" + W(dolphin_exe) + L"\" -u \"" + W(user_dir) + L"\" " + W(extra_args);
    if (!spawn(cmd, W(exe_dir), wd, desktop.empty(), &out->dolphin_pid)) { out->error = "could not start Dolphin"; return false; }
    std::string args = std::string("-Role ") + (host ? "host" : "guest") +
                       " -DolphinPid " + std::to_string(out->dolphin_pid) + " -Status \"" + out->status_path + "\" -Suffix \"" +
                       list_item_suffix(s) + "\" -Players " + std::to_string(s.players) + " -DelayS " + std::to_string(host ? 0 : guest_connect_delay_s(my_slot)) + (s.mode == "direct" ? " -NoCode 1" : "") + (!host && out->wait_go ? " -GoFile \"" + out->go_path + "\"" : "") + (host ? " -BufFile \"" + out->buf_path + "\"" : "");
    if (!spawn(W(hubhelper::run_command(rhhelper::kScriptDolphinDriver, args)), W(work_dir), wd, false, &out->driver_pid)) { out->error = "could not start the NetPlay driver"; return false; }
    return true;
}

void set_host_buffer(const Run& r, int frames) {
    if (r.buf_path.empty() || frames < 1) return;
    std::ofstream f(P8(r.buf_path), std::ios::binary | std::ios::trunc);
    f << frames;
}

void signal_go(const Run& r) {
    if (r.go_path.empty()) return;
    HANDLE h = CreateFileW(W(r.go_path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

std::string read_status(const Run& r) {
    std::ifstream f(P8(r.status_path), std::ios::binary);
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
