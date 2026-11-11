#include "session_journal.h"

#include "durable_write.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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

void set_aside(const fs::path& p) {
    std::error_code ec;
    fs::path to = p;
    to += L".corrupt";
    for (int n = 1; fs::exists(to, ec) && n < 1000; ++n) { to = p; to += L".corrupt" + std::to_wstring(n); }
    fs::rename(p, to, ec);
    if (ec) fs::remove(p, ec) ;   // cannot even rename: it must not be trusted (or re-parsed) next time; a journal is never a save
}

std::string new_id() {
    static std::atomic<unsigned> counter{0};
    char b[64];
    std::snprintf(b, sizeof b, "%llx-%lx-%x", (unsigned long long)GetTickCount64() ^ ((unsigned long long)std::time(nullptr) << 20), (unsigned long)GetCurrentProcessId(), counter.fetch_add(1));
    return b;
}

std::mutex g_recover_mu;

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
    kv("system", e.system_short);
    kv("game", e.game_key);
    kv("title", e.title);
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
        else if (k == "system") e.system_short = v;
        else if (k == "game") e.game_key = v;
        else if (k == "title") e.title = v;
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
        if (nm.error.empty() && nm.user_root == s.emu_root) merge_nx_map(s, nm);
    }
    hubemu::end_session(s);
}

void finish(hubemu::SaveSession& s, const Entry& meta, const std::string& dir) {
    run_exit(s, meta);
    live_drop(meta.id);
    if (s.error.empty() && !meta.id.empty()) remove_entry(dir, meta.id);   // an error keeps it: the next start retries (the copy is idempotent)
}

void abort_unspawned(const Entry& meta, const std::string& dir, bool revert) {
    if (revert && !meta.cemu_user.empty()) {
        if (meta.revert_profile) hubemu::revert_cemu_game_profile(meta.cemu_user, meta.cemu_tid);
        if (meta.revert_own_settings) hubemu::revert_cemu_own_settings(meta.cemu_user);
    }
    live_drop(meta.id);
    if (!meta.id.empty()) remove_entry(dir, meta.id);
}

void forget_live() {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live.clear();
}

bool process_running(unsigned long pid, unsigned long long created) {
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;   // there but cannot be inspected: assume it is the emulator
    FILETIME c, x, k, u;
    bool running;
    if (!GetProcessTimes(h, &c, &x, &k, &u)) running = true;
    else {
        const unsigned long long now_created = ((unsigned long long)c.dwHighDateTime << 32) | c.dwLowDateTime;
        if (created && now_created != created) running = false;   // the pid was recycled by another program
        else running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    }
    CloseHandle(h);
    return running;
}

RecoverReport recover(const std::string& dir, const std::function<void(const Entry&)>& after) {
    std::lock_guard<std::mutex> lk(g_recover_mu);
    RecoverReport rep;
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(u8(dir), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == L".session") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const fs::path& p : files) {
        std::string text;
        bool read_ok = false;
        {
            std::ifstream f(p, std::ios::binary);
            if (f) { text.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); read_ok = !f.bad(); }
        }
        Entry e;
        if (!read_ok || !parse(text, &e) || to_u8(p.stem()) != e.id) {
            set_aside(p);
            ++rep.corrupt;
            rep.notes.push_back("pending session file " + to_u8(p.filename()) + " could not be read: set aside");
            continue;
        }
        if (process_running(e.pid, e.created)) { ++rep.still_running; rep.notes.push_back("pending session " + e.id + ": emulator still running"); continue; }
        if (live_kind(e.emu_kind) || (!e.emu_exe.empty() && hubemu::is_running(e.emu_exe))) {   // another session of this emulator runs: its files are not ours to touch
            ++rep.deferred;
            rep.notes.push_back("pending session " + e.id + ": the emulator is running again, waiting");
            continue;
        }
        run_exit(e.sess, e);
        if (e.sess.error.empty()) {
            if (after) after(e);
            remove_entry(dir, e.id);
            ++rep.replayed;
            rep.notes.push_back("pending session " + e.id + " replayed: " + e.sess.summary);
        } else {
            ++rep.failed;
            ++e.attempts;
            rep.notes.push_back("pending session " + e.id + " failed (attempt " + std::to_string(e.attempts) + "): " + e.sess.error);
            if (e.attempts >= 3) set_aside(p);   // gives up; the emulator's own files and the vault are untouched
            else { e.sess.active = true; e.sess.error.clear(); write_entry(dir, e); }
        }
    }
    return rep;
}

namespace {
struct Recovery {
    std::thread th;
    std::mutex mu;
    std::condition_variable cv;
    bool stop = false;
    ~Recovery() { stop_recovery(); }
};
Recovery& rec() { static Recovery r; return r; }
} // namespace

void start_recovery(const std::string& dir, std::function<void(const Entry&)> after, std::function<void(const std::string&)> log) {
    Recovery& r = rec();
    std::lock_guard<std::mutex> lk(r.mu);
    if (r.th.joinable()) return;
    r.stop = false;
    r.th = std::thread([dir, after, log]() {
        Recovery& r = rec();
        for (;;) {
            RecoverReport rep = recover(dir, after);
            if (log) for (const auto& n : rep.notes) log(n);
            if (rep.still_running + rep.deferred == 0) return;   // nothing waits for an emulator any more
            std::unique_lock<std::mutex> lk(r.mu);
            if (r.cv.wait_for(lk, std::chrono::seconds(15), [&] { return r.stop; })) return;
        }
    });
}

void stop_recovery() {
    Recovery& r = rec();
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.stop = true;
    }
    r.cv.notify_all();
    if (r.th.joinable() && r.th.get_id() != std::this_thread::get_id()) r.th.join();
}

} // namespace hubjournal
