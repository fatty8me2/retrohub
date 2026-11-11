#include "session_journal.h"

#include "durable_write.h"
#include "emu_saves.h"
#include "library.h"
#include "lichii.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <cwctype>

namespace hubjournal {

namespace fs = std::filesystem;

namespace {

fs::path u8(const std::string& s) { return fs::u8path(s); }
std::string to_u8(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }

std::string esc(const std::string& v) {
    std::string o;
    for (char c : v) {
        if (c == '\\') o += "\\\\";
        else if (c == '\t') o += "\\t";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o.push_back(c);
    }
    return o;
}
bool unesc(const std::string& v, std::string* out) {
    out->clear();
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '\\') { out->push_back(v[i]); continue; }
        if (++i >= v.size()) return false;
        switch (v[i]) {
            case '\\': out->push_back('\\'); break;
            case 't': out->push_back('\t'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            default: return false;
        }
    }
    return true;
}
std::string hexs(unsigned long long v) { char b[24]; std::snprintf(b, sizeof b, "%llx", v); return b; }
bool parse_hex(const std::string& s, unsigned long long* v) {
    if (s.empty() || s.size() > 16) return false;
    char* end = nullptr;
    *v = std::strtoull(s.c_str(), &end, 16);
    return end && *end == 0;
}
bool parse_int(const std::string& s, long long* v) {
    if (s.empty()) return false;
    char* end = nullptr;
    *v = std::strtoll(s.c_str(), &end, 10);
    return end && *end == 0;
}

/* A relative save folder from a journal: never absolute, never leaving the emulator folder / vault. */
bool safe_rel(const std::string& r) {
    if (r.empty() || r[0] == '/' || r[0] == '\\' || r.find(':') != std::string::npos) return false;
    size_t i = 0;
    while (i <= r.size()) {
        size_t j = r.find_first_of("/\\", i);
        if (j == std::string::npos) j = r.size();
        if (r.compare(i, j - i, "..") == 0) return false;
        i = j + 1;
    }
    return true;
}

std::mutex g_live_mu;
std::multimap<std::string, int> g_live;   // id -> emulator kind of the sessions this process is running right now
bool live_kind(int kind) {
    std::lock_guard<std::mutex> lk(g_live_mu);
    for (const auto& kv : g_live) if (kv.second == kind) return true;
    return false;
}
void live_add(const Entry& e) {
    std::lock_guard<std::mutex> lk(g_live_mu);
    if (g_live.find(e.id) == g_live.end()) g_live.insert({e.id, e.emu_kind});
}
void live_drop(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live.erase(id);
}

std::string file_of(const std::string& dir, const std::string& id) { return to_u8(u8(dir) / u8(id + ".session")); }

bool write_entry(const std::string& dir, const Entry& e) {
    std::error_code ec;
    fs::create_directories(u8(dir), ec);
    return durable::write_file_durable(u8(file_of(dir, e.id)).wstring(), serialize(e));
}

void remove_entry(const std::string& dir, const std::string& id) {
    std::error_code ec;
    fs::remove(u8(file_of(dir, id)), ec);
}

void set_aside(const fs::path& p, const wchar_t* suffix) {
    std::error_code ec;
    fs::path to = p;
    to += suffix;
    for (int n = 1; fs::exists(to, ec) && n < 1000; ++n) { to = p; to += suffix + std::to_wstring(n); }
    fs::rename(p, to, ec);
    if (ec) fs::remove(p, ec);   // cannot even rename: it must not be trusted (or re-parsed) next time; a journal is never a save
}

std::string new_id() {
    static std::atomic<unsigned> counter{0};
    char b[64];
    std::snprintf(b, sizeof b, "%llx-%lx-%x", (unsigned long long)GetTickCount64() ^ ((unsigned long long)std::time(nullptr) << 20), (unsigned long)GetCurrentProcessId(), counter.fetch_add(1));
    return b;
}

} // namespace

std::string pending_dir(const std::string& data_dir) {
    std::string d = data_dir;
    if (!d.empty() && d.back() != '\\' && d.back() != '/') d.push_back('\\');
    return d + "pending_sessions\\";
}

std::string serialize(const Entry& e) {
    std::string o = "RHJ1\n";
    auto kv = [&](const char* k, const std::string& v) { o += k; o += '\t'; o += esc(v); o += '\n'; };
    const hubemu::SaveSession& s = e.sess;
    kv("id", e.id);
    kv("kind", std::to_string(e.emu_kind));
    kv("exe", e.emu_exe);
    kv("inst", e.inst_dir);
    kv("inst_managed", e.inst_managed ? "1" : "0");
    kv("system", e.system_short);
    kv("game", e.game_key);
    kv("title", e.title);
    if (!e.game_path.empty()) kv("gamepath", e.game_path);
    kv("pid", std::to_string(e.pid));
    kv("created", hexs(e.created));
    kv("attempts", std::to_string(e.attempts));
    kv("active", s.active ? "1" : "0");
    kv("emu_root", s.emu_root);
    kv("battery", s.battery);
    kv("backup_dir", s.backup_dir);
    kv("manifest", s.manifest_path);
    for (const auto& r : s.rels) kv("rel", r);
    for (const auto& r : s.vault_rels) kv("vrel", r);
    kv("wiiu_unknown", s.wiiu_unknown_title ? "1" : "0");
    kv("wiiu_parent", s.wiiu_save_parent);
    kv("started", std::to_string((long long)s.started_unix));
    kv("own_install", s.own_install ? "1" : "0");
    kv("snapshot_emu", s.snapshot_emu);
    kv("psp_disc", s.psp_disc_id);
    kv("nx_dir", e.nx_install_dir);
    kv("nx_tid", hexs(e.nx_title_id));
    kv("cemu_user", e.cemu_user);
    kv("cemu_tid", hexs(e.cemu_tid));
    kv("revert_profile", e.revert_profile ? "1" : "0");
    kv("revert_settings", e.revert_own_settings ? "1" : "0");
    o += "end\n";
    return o;
}

bool parse(const std::string& text, Entry* out) {
    Entry e;
    hubemu::SaveSession& s = e.sess;
    size_t pos = 0;
    auto next = [&](std::string* line) {
        if (pos >= text.size()) return false;
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) return false;   // every line, the last included, ends in a newline: a cut-off file is corrupt
        *line = text.substr(pos, nl - pos);
        pos = nl + 1;
        return true;
    };
    std::string line;
    if (!next(&line) || line != "RHJ1") return false;
    bool ended = false, have_id = false, have_kind = false;
    while (next(&line)) {
        if (line == "end") { ended = true; break; }
        size_t t = line.find('\t');
        if (t == std::string::npos) return false;
        const std::string k = line.substr(0, t);
        std::string v;
        if (!unesc(line.substr(t + 1), &v)) return false;
        unsigned long long h = 0;
        long long n = 0;
        auto flag = [&](bool* b) { if (v != "0" && v != "1") return false; *b = v == "1"; return true; };
        if (k == "id") { if (v.empty() || v.find_first_of("\\/:") != std::string::npos || v.find("..") != std::string::npos) return false; e.id = v; have_id = true; }
        else if (k == "kind") { if (!parse_int(v, &n) || n < 0 || n > 1000) return false; e.emu_kind = (int)n; have_kind = true; }
        else if (k == "exe") e.emu_exe = v;
        else if (k == "inst") e.inst_dir = v;
        else if (k == "inst_managed") { if (!flag(&e.inst_managed)) return false; }
        else if (k == "system") e.system_short = v;
        else if (k == "game") e.game_key = v;
        else if (k == "title") e.title = v;
        else if (k == "gamepath") e.game_path = v;
        else if (k == "pid") { if (!parse_int(v, &n) || n < 0) return false; e.pid = (unsigned long)n; }
        else if (k == "created") { if (!parse_hex(v, &h)) return false; e.created = h; }
        else if (k == "attempts") { if (!parse_int(v, &n) || n < 0) return false; e.attempts = (int)n; }
        else if (k == "active") { if (!flag(&s.active)) return false; }
        else if (k == "emu_root") s.emu_root = v;
        else if (k == "battery") s.battery = v;
        else if (k == "backup_dir") s.backup_dir = v;
        else if (k == "manifest") s.manifest_path = v;
        else if (k == "rel") { if (!safe_rel(v)) return false; s.rels.push_back(v); }
        else if (k == "vrel") { if (!safe_rel(v)) return false; s.vault_rels.push_back(v); }
        else if (k == "wiiu_unknown") { if (!flag(&s.wiiu_unknown_title)) return false; }
        else if (k == "wiiu_parent") s.wiiu_save_parent = v;
        else if (k == "started") { if (!parse_int(v, &n)) return false; s.started_unix = n; }
        else if (k == "own_install") { if (!flag(&s.own_install)) return false; }
        else if (k == "snapshot_emu") s.snapshot_emu = v;
        else if (k == "psp_disc") s.psp_disc_id = v;
        else if (k == "nx_dir") e.nx_install_dir = v;
        else if (k == "nx_tid") { if (!parse_hex(v, &h)) return false; e.nx_title_id = h; }
        else if (k == "cemu_user") e.cemu_user = v;
        else if (k == "cemu_tid") { if (!parse_hex(v, &h)) return false; e.cemu_tid = h; }
        else if (k == "revert_profile") { if (!flag(&e.revert_profile)) return false; }
        else if (k == "revert_settings") { if (!flag(&e.revert_own_settings)) return false; }
        // unknown keys: a newer hub's addition, ignored
    }
    if (!ended || !have_id || !have_kind) return false;
    if (!s.vault_rels.empty() && s.vault_rels.size() != s.rels.size()) return false;
    if (s.active && (s.battery.empty() || s.emu_root.empty() || s.manifest_path.empty())) return false;   // would sync into a nameless place
    *out = e;
    return true;
}

bool begin(const std::string& dir, Entry& e) {
    if (e.id.empty()) e.id = new_id();
    live_add(e);
    return write_entry(dir, e);
}

bool update(const std::string& dir, const Entry& e) { return write_entry(dir, e); }

std::mutex& session_lock() {
    static std::mutex m;
    return m;
}

namespace {
std::function<void(bool, const std::string&)> g_log;
std::function<void()> g_replay_hook;
void log_line(bool warning, const std::string& m) { if (g_log) g_log(warning, m); }
}

void set_log(std::function<void(bool, const std::string&)> log) { g_log = std::move(log); }
void set_replay_hook(std::function<void()> hook) { g_replay_hook = std::move(hook); }

bool merge_nx_map(hubemu::SaveSession& s, const hubemu::NxSaveMap& m) {
    bool added = false;
    if (s.vault_rels.size() < s.rels.size()) for (size_t i = s.vault_rels.size(); i < s.rels.size(); ++i) s.vault_rels.push_back(s.rels[i]);
    for (size_t i = 0; i < m.rels.size(); ++i) {
        if (std::find(s.rels.begin(), s.rels.end(), m.rels[i]) != s.rels.end()) continue;
        s.rels.push_back(m.rels[i]);
        s.vault_rels.push_back(i < m.vault_rels.size() ? m.vault_rels[i] : m.rels[i]);
        added = true;
    }
    return added;
}

void run_exit(hubemu::SaveSession& s, const Entry& meta) {
    if (!meta.cemu_user.empty()) {   // put back everything RetroHub changed in the user's Cemu for this session (both are no-ops when already reverted)
        if (meta.revert_profile) hubemu::revert_cemu_game_profile(meta.cemu_user, meta.cemu_tid);
        if (meta.revert_own_settings) hubemu::revert_cemu_own_settings(meta.cemu_user);
    }
    if (s.active && meta.nx_title_id && !meta.nx_install_dir.empty()) {   // Switch: folders the emulator created during the session (nand\user\save\account\...) count too
        hubemu::NxSaveMap nm = hubemu::nx_save_map(meta.nx_install_dir, meta.nx_title_id, s.battery, false);
        /* only for the same user as at launch: a changed profile must not redirect the copy-back to another folder */
        if (nm.error.empty() && nm.user_root == s.emu_root && !nm.rels.empty() && !s.rels.empty() && nm.rels[0] == s.rels[0]) merge_nx_map(s, nm);
        else log_line(true, "save mapping at exit differs from the one at launch (" + meta.game_key + "): the launch-time folders are used");
    }
    hubemu::end_session(s);
}

void finish(hubemu::SaveSession& s, const Entry& meta, const std::string& dir) {
    run_exit(s, meta);
    /* file first, then the live entry: the other way round leaves a window where a replay could see the journal with no live entry */
    if (s.error.empty() && !meta.id.empty()) remove_entry(dir, meta.id);   // an error keeps it: the next start retries (the copy is idempotent)
    live_drop(meta.id);
}

void abort_unspawned(const Entry& meta, const std::string& dir, bool revert) {
    if (revert && !meta.cemu_user.empty()) {
        if (meta.revert_profile) hubemu::revert_cemu_game_profile(meta.cemu_user, meta.cemu_tid);
        if (meta.revert_own_settings) hubemu::revert_cemu_own_settings(meta.cemu_user);
    }
    if (!meta.id.empty()) remove_entry(dir, meta.id);
    live_drop(meta.id);
}

void forget_live() {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live.clear();
}

namespace { bool same_path(const std::string& a, const std::string& b); }

bool process_running(unsigned long pid, unsigned long long created, const std::string& exe, long long started_unix) {
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;   // there but cannot be inspected: assume it is the emulator
    FILETIME c, x, k, u;
    bool running;
    if (!GetProcessTimes(h, &c, &x, &k, &u)) running = true;
    else if (!created) {
        /* no creation time recorded: compare what can be compared, and never call it "not running" while it might be the emulator */
        bool decided = false;
        running = true;
        if (!exe.empty()) {
            wchar_t buf[2048];
            DWORD n = (DWORD)(sizeof buf / sizeof buf[0]);
            if (QueryFullProcessImageNameW(h, 0, buf, &n)) { running = same_path(to_u8(fs::path(std::wstring(buf, n))), exe); decided = true; }
        }
        if (decided) { if (running) running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT; }
        else if (started_unix > 0 && (long long)time(nullptr) - started_unix > 24 * 3600) running = false;   // nothing to compare and the journal is old: a recycled pid
        else running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    } else {
        const unsigned long long now_created = ((unsigned long long)c.dwHighDateTime << 32) | c.dwLowDateTime;
        if (created && now_created != created) running = false;   // the pid was recycled by another program
        else running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    }
    CloseHandle(h);
    return running;
}

namespace {

std::wstring norm_path(const std::string& p) {
    std::wstring w = u8(p).wstring();
    for (auto& c : w) { if (c == L'/') c = L'\\'; c = (wchar_t)towlower(c); }
    while (w.size() > 3 && w.back() == L'\\') w.pop_back();
    return w;
}
bool same_path(const std::string& a, const std::string& b) { return !a.empty() && !b.empty() && norm_path(a) == norm_path(b); }
bool path_under(const std::string& p, const std::string& dir) {
    std::wstring d = norm_path(dir), w = norm_path(p);
    if (d.empty()) return false;
    if (d.back() != L'\\') d.push_back(L'\\');
    return w.size() > d.size() && w.compare(0, d.size(), d) == 0;
}

} // namespace

bool image_running(const std::string& exe, const std::string& managed_dir) {
    if (exe.empty() && managed_dir.empty()) return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    const DWORD me = GetCurrentProcessId();
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == me || pe.th32ProcessID == 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t buf[2048];
        DWORD n = (DWORD)(sizeof buf / sizeof buf[0]);
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
            const std::string img = to_u8(fs::path(std::wstring(buf, n)));
            if ((!exe.empty() && same_path(img, exe)) || (!managed_dir.empty() && path_under(img, managed_dir))) found = true;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

namespace {

std::string join_path(const std::string& a, const std::string& b) { return to_u8(u8(a) / u8(b)); }
std::string strip_sep(std::string s) { while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back(); return s; }

/* Everything the replay will touch, derived again from the CURRENT configuration exactly as begin_session does. The journal only names identity.
 * `why` says what disagrees. */
bool derive(const Entry& e, const Config& cfg, Entry* out, std::string* why) {
    *out = e;
    hubemu::SaveSession& s = out->sess;
    std::string dir;
    bool managed = false;
    if (!cfg.install || !cfg.install(e, &dir, &managed) || dir.empty()) { *why = "the emulator is not installed any more"; return false; }
    if (!same_path(dir, e.inst_dir)) { *why = "the emulator now lives somewhere else"; return false; }
    out->inst_managed = managed;
    if (!e.nx_install_dir.empty() && !same_path(e.nx_install_dir, dir)) { *why = "the recorded Switch folder is not the install folder"; return false; }
    if (!e.cemu_user.empty()) {
        const std::string want = managed ? join_path(dir, "portable") : hubemu::cemu_user_data_dir(dir, cfg.appdata);
        if (!same_path(e.cemu_user, want)) { *why = "Cemu's data folder is not where it was"; return false; }
        out->cemu_user = want;
    }
    if (!s.active) return true;   // nothing is copied: only the reverts above
    if (!cfg.battery_dir) { *why = "no vault configuration"; return false; }
    const std::string battery = strip_sep(cfg.battery_dir(e.system_short, e.game_key));
    if (battery.empty() || !same_path(battery, s.battery)) { *why = "the save vault is not where it was"; return false; }
    const fs::path game = u8(battery).parent_path(), sysdir = game.parent_path(), vault = sysdir.parent_path();
    std::string suffix;   // "-fritchii" etc.: what follows "emu-<system>-<game>" in the recorded manifest name
    {
        const std::string name = to_u8(u8(s.manifest_path).filename());
        const std::string head = "emu-" + to_u8(sysdir.filename()) + "-" + to_u8(game.filename());
        if (name.size() < head.size() + 5 || name.compare(0, head.size(), head) != 0 || name.compare(name.size() - 5, 5, ".json") != 0) { *why = "the recorded manifest does not belong to this game"; return false; }
        suffix = name.substr(head.size(), name.size() - head.size() - 5);
        if (suffix.size() > 24 || suffix.find_first_not_of("-abcdefghijklmnopqrstuvwxyz0123456789") != std::string::npos) { *why = "the recorded manifest name is not valid"; return false; }
    }
    std::string root;
    const hub::System sys = hub::system_from_short(e.system_short);
    if (e.emu_kind == 100) root = dir;                               // Hookshot: its user folder
    else if (sys == hub::System::N3DS) root = hub::n3ds_sd_root(join_path(dir, "user"));
    else if (sys == hub::System::WiiU) {
        root = managed ? join_path(join_path(dir, "portable"), "mlc01") : hub::cemu_dirs_for(dir, cfg.appdata, hubemu::wiiu_probe()).mlc;
        s.wiiu_save_parent = join_path(root, "usr\\save\\00050000");
    }
    else if (sys == hub::System::PS2) root = dir;
    else if (sys == hub::System::PSP) root = join_path(dir, "memstick\\PSP");
    else if (sys == hub::System::Switch) root = hubemu::lichii_user_dir(dir);
    else if (sys == hub::System::GC) root = hubemu::dolphin_card_root(dir, e.game_key);
    if (root.empty() || !same_path(root, s.emu_root)) { *why = "the emulator's save folder is not where it was"; return false; }
    s.emu_root = root;
    s.battery = battery;
    s.backup_dir = to_u8(vault / L"_conflicts" / sysdir.filename() / game.filename() / L"battery");
    s.manifest_path = hubemu::emu_manifest_path(battery, suffix.c_str());
    return true;
}

std::mutex g_recover_mu;

} // namespace

RecoverReport recover(const std::string& dir, const Config& cfg, const std::function<void(const Entry&)>& after, const std::atomic<bool>* stop) {
    std::lock_guard<std::mutex> rlk(g_recover_mu);
    RecoverReport rep;
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(u8(dir), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == L".session") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    struct CancelScope { CancelScope(const std::atomic<bool>* f) { hub::set_sync_cancel(f); } ~CancelScope() { hub::set_sync_cancel(nullptr); } } cancel_scope(stop);
    auto stopped = [&] { return stop && stop->load(); };
    for (const fs::path& p : files) {
        if (stopped()) break;
        std::unique_lock<std::mutex> lk(session_lock(), std::defer_lock);   // a launch in progress owns the emulator's files: wait for it
        while (!lk.try_lock()) { if (stopped()) return rep; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        try {
        std::string text;
        bool read_ok = false;
        {
            std::ifstream f(p, std::ios::binary);
            if (!f) { std::error_code e2; if (!fs::exists(p, e2)) continue; }   // replayed or removed meanwhile
            else { text.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); read_ok = !f.bad(); }
        }
        Entry e;
        if (!read_ok || !parse(text, &e) || to_u8(p.stem()) != e.id) {
            set_aside(p, L".corrupt");
            ++rep.corrupt;
            rep.warnings.push_back("pending session file " + to_u8(p.filename()) + " could not be read: set aside");
            continue;
        }
        /* everything below is checked INSIDE the lock, after any launch finished: the emulator may have started since the last look */
        if (process_running(e.pid, e.created, e.emu_exe, (long long)e.sess.started_unix)) { ++rep.still_running; rep.notes.push_back("pending session " + e.id + ": emulator still running"); continue; }
        if (live_kind(e.emu_kind) || image_running(e.emu_exe, e.inst_managed ? e.inst_dir : std::string())) {
            ++rep.deferred;
            rep.notes.push_back("pending session " + e.id + ": the emulator is running again, waiting");
            continue;
        }
        Entry d;
        std::string why;
        if (!derive(e, cfg, &d, &why)) {
            ++rep.failed;
            ++e.attempts;
            rep.warnings.push_back("pending session " + e.id + " kept (" + e.game_key + "): " + why + " (attempt " + std::to_string(e.attempts) + ")");
            if (e.attempts >= 3) { set_aside(p, L".stale"); rep.warnings.push_back("pending session " + e.id + " set aside as " + to_u8(p.filename()) + ".stale: it never matched this setup"); }
            else write_entry(dir, e);
            continue;
        }
        if (g_replay_hook) g_replay_hook();
        run_exit(d.sess, d);
        if (d.sess.error.empty()) {
            if (after) after(d);
            remove_entry(dir, e.id);
            ++rep.replayed;
            rep.notes.push_back("pending session " + e.id + " replayed: " + d.sess.summary);
        } else if (stopped()) {
            break;   // cut short by shutdown: the journal stays, no attempt is counted
        } else {
            ++rep.failed;
            ++e.attempts;
            rep.warnings.push_back("pending session " + e.id + " failed (attempt " + std::to_string(e.attempts) + "): " + d.sess.error);
            if (e.attempts >= 3) set_aside(p, L".stale");   // gives up; the emulator's own files and the vault are untouched
            else write_entry(dir, e);
        }
        } catch (...) {   // an odd journal (filesystem error, bad_alloc, invalid UTF-8) must never take the hub down: set it aside, never delete
            try { set_aside(p, L".corrupt"); } catch (...) {}
            ++rep.corrupt;
            try { rep.warnings.push_back("a pending session file raised an error and was set aside"); } catch (...) {}
        }
    }
    return rep;
}

namespace {
struct Recovery {
    std::thread th;
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    ~Recovery() { stop_recovery(); }
};
Recovery& rec() { static Recovery r; return r; }
} // namespace

void start_recovery(const std::string& dir, Config cfg, std::function<void(const Entry&)> after, std::function<void(bool, const std::string&)> log) {
    Recovery& r = rec();
    std::lock_guard<std::mutex> lk(r.mu);
    if (r.th.joinable()) return;
    r.stop = false;
    set_log(log);
    r.th = std::thread([dir, cfg, after, log]() {
        try {
        Recovery& r = rec();
        for (;;) {
            RecoverReport rep = recover(dir, cfg, after, &r.stop);
            if (log) { for (const auto& n : rep.notes) log(false, n); for (const auto& w : rep.warnings) log(true, w); }
            if (r.stop || rep.still_running + rep.deferred == 0) return;   // nothing waits for an emulator any more
            std::unique_lock<std::mutex> lk(r.mu);
            if (r.cv.wait_for(lk, std::chrono::seconds(15), [&] { return r.stop.load(); })) return;
        }
        } catch (...) {
            try { if (log) log(true, "pending session recovery stopped after an unexpected error"); } catch (...) {}
        }
    });
}

void stop_recovery() {
    Recovery& r = rec();
    r.stop = true;
    {
        std::lock_guard<std::mutex> lk(r.mu);   // pairs with the wait predicate: no lost wake-up
    }
    r.cv.notify_all();
    if (r.th.joinable() && r.th.get_id() != std::this_thread::get_id()) r.th.join();
}

} // namespace hubjournal
