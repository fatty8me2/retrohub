#include "kanto.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "json.h"
#include "miniz.h"
#include "winutil.h"

#ifdef _WIN32
#include <bcrypt.h>
#endif

namespace fs = std::filesystem;

namespace kanto {

static fs::path P(const std::string& u8) { return fs::path(winutil::to_wide(u8)); }
static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

/* ------------------------------ hashing ------------------------------ */
#ifdef _WIN32
struct Sha1Ctx {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::vector<UCHAR> obj;
    bool ok = false;
    Sha1Ctx() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return;
        DWORD n = 0, got = 0;
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&n, sizeof n, &got, 0);
        obj.resize(n);
        ok = BCryptCreateHash(alg, &h, obj.data(), n, nullptr, 0, 0) == 0;
    }
    void add(const void* d, size_t len) { if (ok) BCryptHashData(h, (PUCHAR)d, (ULONG)len, 0); }
    std::string finish() {
        if (!ok) return "";
        UCHAR out[20];
        BCryptFinishHash(h, out, 20, 0);
        static const char* hx = "0123456789abcdef";
        std::string s;
        for (UCHAR c : out) { s += hx[c >> 4]; s += hx[c & 15]; }
        return s;
    }
    ~Sha1Ctx() { if (h) BCryptDestroyHash(h); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
};
std::string sha1_hex(const void* data, size_t len) { Sha1Ctx c; c.add(data, len); return c.finish(); }
std::string sha1_file(const std::string& path) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return "";
    Sha1Ctx c;
    std::vector<char> buf(1 << 20);
    while (f) { f.read(buf.data(), (std::streamsize)buf.size()); if (f.gcount() > 0) c.add(buf.data(), (size_t)f.gcount()); }
    return c.finish();
}
#endif

/* ------------------------------ package detection ------------------------------ */
const std::vector<std::string>& required_files() {
    static const std::vector<std::string> v = {"app/coop-launch.mjs", "app/coop-bridge.mjs", "app/coop.lua", "app/apply-patch.mjs", "patch/patch.json",
                                               "patch/KantoTogether.bps", "runtime/node/node.exe", "runtime/mgba/mGBA.exe", "node_modules/ws/index.js"};
    return v;
}

bool is_package_dir(const std::string& dir) {
    std::error_code ec;
    for (auto& r : required_files()) if (!fs::is_regular_file(P(dir) / fs::path(r), ec)) return false;
    return true;
}

static std::string norm_entry(std::string n) { for (auto& c : n) if (c == '\\') c = '/'; return n; }

bool inspect_zip(const std::string& zip_path, Package* out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, winutil::to_wide(zip_path).c_str(), L"rb") != 0 || !f) return false;
    mz_zip_archive za;
    memset(&za, 0, sizeof za);
    if (!mz_zip_reader_init_cfile(&za, f, 0, 0)) { fclose(f); return false; }
    std::vector<std::string> names;
    mz_uint n = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory) continue;
        names.push_back(norm_entry(st.m_filename));
    }
    mz_zip_reader_end(&za);
    fclose(f);
    const std::string marker = "app/coop-launch.mjs";
    std::string prefix;
    bool found = false;
    for (auto& nm : names) {
        if (nm.size() >= marker.size() && nm.compare(nm.size() - marker.size(), marker.size(), marker) == 0) {
            std::string pre = nm.substr(0, nm.size() - marker.size());
            if (pre.find("..") != std::string::npos) continue;
            prefix = pre; found = true; break;
        }
    }
    if (!found) return false;
    for (auto& r : required_files())
        if (std::find(names.begin(), names.end(), prefix + r) == names.end()) return false;
    std::error_code ec;
    Package p;
    p.path = zip_path;
    p.is_zip = true;
    p.size = (uint64_t)fs::file_size(P(zip_path), ec);
    auto t = fs::last_write_time(P(zip_path), ec);
    p.stamp = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    p.prefix = prefix;
    *out = p;
    return true;
}

std::vector<Package> find_packages(const std::vector<std::string>& folders, int max_depth) {
    std::vector<Package> res;
    std::vector<std::string> seen;
    std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int depth) {
        std::error_code ec;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            const fs::path& p = it->path();
            std::string name = lower(winutil::to_utf8(p.filename().wstring()));
            bool kanto = name.find("kanto") != std::string::npos;
            if (it->is_directory(e2)) {
                if (kanto && is_package_dir(winutil::to_utf8(p.wstring()))) {
                    Package pk;
                    pk.path = winutil::to_utf8(p.wstring());
                    res.push_back(pk);
                } else if (depth < max_depth) walk(p, depth + 1);
            } else if (kanto && name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0) {
                Package pk;
                if (inspect_zip(winutil::to_utf8(p.wstring()), &pk)) res.push_back(pk);
            }
        }
    };
    for (auto& f : folders) {
        std::string key = lower(f);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        std::error_code ec;
        if (fs::is_directory(P(f), ec)) walk(P(f), 0);
    }
    return res;
}

std::string install_rel_path(const std::string& entry_in, const std::string& prefix) {
    std::string e = norm_entry(entry_in);
    if (e.compare(0, prefix.size(), prefix) != 0) return "";
    e = e.substr(prefix.size());
    if (e.empty() || e.back() == '/' || e[0] == '/' || e.find(':') != std::string::npos) return "";
    for (unsigned char c : e) if (c < 32) return "";
    std::stringstream ss(e);
    std::string seg, first;
    int idx = 0;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty() || seg == "." || seg == "..") return "";
        if (idx++ == 0) first = lower(seg);
    }
    if (first == "rom" || first == "saves") return "";
    return e;
}

std::string install_dir(const std::string& data) {
    std::string d = data;
    if (!d.empty() && d.back() != '\\' && d.back() != '/') d += '\\';
    return d + "apps\\KantoTogether\\";
}
static std::string slash(std::string d) { if (!d.empty() && d.back() != '\\' && d.back() != '/') d += '\\'; return d; }
bool is_installed(const std::string& dir) { return is_package_dir(dir); }
static std::string marker_text(const Package& p) { return std::to_string(p.size) + ":" + std::to_string(p.stamp) + ":" + (p.is_zip ? "z" : "d"); }
bool install_current(const std::string& dir, const Package& p) {
    if (!is_installed(dir)) return false;
    std::string m;
    if (!winutil::read_file(slash(dir) + ".retrohub-installed", &m)) return false;
    return trim(m) == marker_text(p);
}

static bool write_stream(const fs::path& to, const std::function<bool(FILE*)>& fn) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    FILE* o = nullptr;
    if (_wfopen_s(&o, to.wstring().c_str(), L"wb") != 0 || !o) return false;
    bool ok = fn(o);
    fclose(o);
    return ok;
}

bool install(const Package& p, const std::string& dir_in, const std::function<bool(uint64_t, uint64_t)>& progress, std::string* err) {
    std::string dir = slash(dir_in);
    std::error_code ec;
    fs::create_directories(P(dir), ec);
    uint64_t total = 0, done = 0;
    if (p.is_zip) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, winutil::to_wide(p.path).c_str(), L"rb") != 0 || !f) { *err = "Could not open the Kanto Together zip."; return false; }
        mz_zip_archive za;
        memset(&za, 0, sizeof za);
        if (!mz_zip_reader_init_cfile(&za, f, 0, 0)) { fclose(f); *err = "The Kanto Together zip is damaged."; return false; }
        mz_uint n = mz_zip_reader_get_num_files(&za);
        for (mz_uint i = 0; i < n; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory) continue;
            if (!install_rel_path(st.m_filename, p.prefix).empty()) total += st.m_uncomp_size;
        }
        bool ok = true;
        for (mz_uint i = 0; i < n && ok; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory) continue;
            std::string rel = install_rel_path(st.m_filename, p.prefix);
            if (rel.empty()) continue;
            if (!write_stream(P(dir) / P(rel), [&](FILE* o) { return mz_zip_reader_extract_to_cfile(&za, i, o, 0) != 0; })) { *err = "Could not write " + rel + " (disk full or file in use?)."; ok = false; break; }
            done += st.m_uncomp_size;
            if (progress && !progress(done, total)) { *err = "Cancelled."; ok = false; }
        }
        mz_zip_reader_end(&za);
        fclose(f);
        if (!ok) return false;
    } else {
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it(P(p.path), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            std::string rel = winutil::to_utf8(fs::relative(it->path(), P(p.path), e2).wstring());
            rel = install_rel_path(rel, "");
            if (rel.empty()) continue;
            files.push_back(it->path());
            total += (uint64_t)it->file_size(e2);
        }
        for (auto& src : files) {
            std::error_code e2;
            fs::path rel = fs::relative(src, P(p.path), e2);
            fs::create_directories((P(dir) / rel).parent_path(), e2);
            fs::copy_file(src, P(dir) / rel, fs::copy_options::overwrite_existing, e2);
            if (e2) { *err = "Could not copy " + winutil::to_utf8(rel.wstring()); return false; }
            done += (uint64_t)fs::file_size(src, e2);
            if (progress && !progress(done, total)) { *err = "Cancelled."; return false; }
        }
    }
    if (!is_installed(dir)) { *err = "The package is incomplete (files are missing)."; return false; }
    winutil::write_file(dir + ".retrohub-installed", marker_text(p));
    return true;
}

/* ------------------------------ ROM ------------------------------ */
bool read_manifest(const std::string& path, PatchManifest* out) {
    std::string t;
    json::Value v;
    if (!winutil::read_file(path, &t) || !json::parse(t, &v) || !v.is_object()) return false;
    out->base_sha1 = lower(v.str("baseSha1"));
    out->target_sha1 = lower(v.str("targetSha1"));
    out->base_size = (uint64_t)v.num("baseSize");
    return !out->base_sha1.empty() && !out->target_sha1.empty();
}

RomPick pick_rom(const std::vector<RomCandidate>& cands, const std::string& base_sha1, const std::function<std::string(const std::string&)>& hash) {
    RomPick r;
    for (auto& c : cands) {
        std::string ser = c.serial;
        for (auto& ch : ser) ch = (char)std::toupper((unsigned char)ch);
        const bool firered = ser == "BPRE" || c.catalog_id == "pokefirered";   // FireRed game code / catalog id
        if (!firered) {
            if (!ser.empty() || (c.size != 0 && c.size != 16777216)) continue;   // another game, or too small/large to be it
        }
        std::string h = lower(hash(c.path));
        if (h.empty()) continue;
        if (h == lower(base_sha1)) { r.state = RomState::Ok; r.path = c.path; r.seen_sha1 = h; return r; }
        if (!firered) continue;   // an unidentified 16 MiB file that is not FireRed 1.0: ignore
        r.state = RomState::WrongRevision;
        if (r.seen_sha1.empty()) r.seen_sha1 = h;
    }
    return r;
}

std::string rom_message(const RomPick& p) {
    if (p.state == RomState::Ok) return "";
    if (p.state == RomState::WrongRevision)
        return "Kanto Together needs the original Pokemon FireRed (USA) version 1.0 (SHA-1 " + std::string(kBaseSha1) +
               "). The FireRed in your library is a different revision" + (p.seen_sha1.empty() ? "" : " (" + p.seen_sha1 + ")") + ".";
    return "Kanto Together needs your own Pokemon FireRed (USA) 1.0 ROM. Add it to a RetroHub ROM folder (Settings > ROM folders).";
}

std::string reason_for(RomState s) { return s == RomState::WrongRevision ? "wrong_rom" : "no_rom"; }
bool valid_fail_reason(const std::string& r) {
    for (const char* x : {"no_rom", "wrong_rom", "install", "patch", "cancelled", "package", "start"}) if (r == x) return true;
    return false;
}
std::string fail_sentence(const std::string& r) {
    if (r == "no_rom") return "they do not have Pokemon FireRed (USA) 1.0 in their library";
    if (r == "wrong_rom") return "their FireRed is not the original USA 1.0 version";
    if (r == "install") return "the Kanto Together package could not be installed on their PC";
    if (r == "package") return "they do not have the Kanto Together package";
    if (r == "patch") return "the game could not be built from their FireRed ROM";
    if (r == "cancelled") return "they cancelled the setup";
    if (r == "start") return "the game could not be started on their PC";
    return "something went wrong on their PC";
}

/* ------------------------------ saves ------------------------------ */
int sync_saves(const std::string& src, const std::string& dst) {
    std::error_code ec;
    if (!fs::is_directory(P(src), ec)) return 0;
    fs::create_directories(P(dst), ec);
    int n = 0;
    for (fs::directory_iterator it(P(src), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        if (lower(winutil::to_utf8(it->path().extension().wstring())) != ".sav") continue;
        fs::path to = P(dst) / it->path().filename();
        if (fs::exists(to, e2)) {
            if (fs::file_size(to, e2) == fs::file_size(it->path(), e2)) {
                std::ifstream a(it->path(), std::ios::binary), b(to, std::ios::binary);
                std::string x((std::istreambuf_iterator<char>(a)), {}), y((std::istreambuf_iterator<char>(b)), {});
                if (x == y) continue;
            }
            if (fs::last_write_time(to, e2) >= fs::last_write_time(it->path(), e2)) continue;   // destination is as new or newer
            fs::path bk = P(dst) / ".kt-backup";
            fs::create_directories(bk, e2);
            for (int k = 1; k < 1000; ++k) {
                fs::path cand = bk / (it->path().filename().wstring() + L"." + std::to_wstring(k));
                if (!fs::exists(cand, e2)) { fs::copy_file(to, cand, e2); break; }
            }
        }
        fs::copy_file(it->path(), to, fs::copy_options::overwrite_existing, e2);
        if (!e2) ++n;
    }
    return n;
}

/* ------------------------------ command lines ------------------------------ */
std::string normalize_code(const std::string& s) {
    std::string o;
    for (char c : s) if (std::isxdigit((unsigned char)c)) o += (char)std::toupper((unsigned char)c);
    return o;
}
bool valid_code(const std::string& c) {
    if (c.size() != 10) return false;
    for (char ch : c) if (!std::isxdigit((unsigned char)ch) || std::islower((unsigned char)ch)) return false;
    return true;
}
std::string node_exe(const std::string& d) { return slash(d) + "runtime\\node\\node.exe"; }
std::string mgba_exe(const std::string& d) { return slash(d) + "runtime\\mgba\\mGBA.exe"; }
std::string rom_path(const std::string& d) { return slash(d) + "rom\\KantoTogether.gba"; }

std::string launcher_cmdline(const LaunchOpts& o) {
    std::string c = winutil::quote_arg(node_exe(o.dir)) + " " + winutil::quote_arg("app\\coop-launch.mjs") + (o.host ? " host" : " join " + o.code);
    c += " --rom " + winutil::quote_arg(o.rom) + " --saves " + winutil::quote_arg(o.saves) + " --mgba " + winutil::quote_arg(mgba_exe(o.dir));
    if (o.sound) c += " --unmute";
    if (o.lua_port > 0) c += " --lua-port " + std::to_string(o.lua_port);
    return c;
}
std::string patch_cmdline(const std::string& dir, const std::string& base) {
    return winutil::quote_arg(node_exe(dir)) + " " + winutil::quote_arg("app\\apply-patch.mjs") + " --manifest " + winutil::quote_arg("patch\\patch.json") +
           " --out " + winutil::quote_arg("rom\\KantoTogether.gba") + " --rom " + winutil::quote_arg(base);
}
std::string cmd_trade(int s) { return s >= 1 && s <= 6 ? "trade " + std::to_string(s) + "\n" : ""; }
std::string cmd_confirm() { return "confirm\n"; }
std::string cmd_cancel() { return "cancel\n"; }
std::string cmd_code(const std::string& c) { std::string n = normalize_code(c); return valid_code(n) ? "code " + n + "\n" : ""; }
std::string cmd_quit() { return "quit\n"; }

/* ------------------------------ output -> state ------------------------------ */
const char* phase_name(Phase p) {
    switch (p) {
        case Phase::Idle: return "idle"; case Phase::Starting: return "starting"; case Phase::WaitingForPartner: return "waiting";
        case Phase::Connected: return "connected"; case Phase::Reconnecting: return "reconnecting"; case Phase::PartnerAway: return "partner-away";
        case Phase::Failed: return "failed"; case Phase::Ended: return "ended";
    }
    return "?";
}
std::string phase_text(Phase p, bool host) {
    switch (p) {
        case Phase::Idle: return "Not running";
        case Phase::Starting: return host ? "Starting the game and connecting..." : "Connecting to your partner...";
        case Phase::WaitingForPartner: return host ? "Waiting for your partner to join..." : "Joining your partner...";
        case Phase::Connected: return "Your partner is connected.";
        case Phase::Reconnecting: return "Connection lost - reconnecting by itself...";
        case Phase::PartnerAway: return "Your partner is not connected right now. They can come back by themselves.";
        case Phase::Failed: return "Could not connect.";
        case Phase::Ended: return "Session ended.";
    }
    return "";
}

static std::string strip_prefix(std::string l) {
    l = trim(l);
    const std::string b = "[bridge] ";
    if (l.compare(0, b.size(), b) == 0) l = l.substr(b.size());
    if (l.compare(0, 5, "lua: ") == 0) l = l.substr(5);
    return l;
}

bool feed_line(State& s, const std::string& raw) {
    std::string l = strip_prefix(raw);
    if (l.empty()) return false;
    std::smatch m;
    static const std::regex rc(R"(^(NEW )?ROOM CODE[^:]*:\s*([0-9A-Fa-f]{10})\b)");
    if (std::regex_search(l, m, rc)) {
        std::string code = normalize_code(m[2].str());
        if (code != s.code) { s.code = code; ++s.code_gen; }
        if (s.phase != Phase::Connected) s.phase = Phase::WaitingForPartner;
        s.detail = phase_text(s.phase, s.host);
        return true;
    }
    if (l.find(">>> Your partner is connected. <<<") != std::string::npos || l == "peer connected") {
        s.phase = Phase::Connected; s.detail = phase_text(s.phase, s.host); s.last_error.clear(); return true;
    }
    if (l.compare(0, 31, "Your partner is not connected r") == 0 || l == "peer disconnected") {
        s.phase = Phase::PartnerAway;
        s.detail = phase_text(s.phase, s.host); return true;
    }
    if (l.find("reconnecting in") != std::string::npos) {
        s.phase = Phase::Reconnecting; s.detail = phase_text(s.phase, s.host); return true;
    }
    if (l == "Connection is back." || l == "reconnected") {
        if (s.phase == Phase::Reconnecting) s.phase = Phase::WaitingForPartner;
        s.detail = "Connection is back."; return true;
    }
    if (l.compare(0, 15, "Could not join:") == 0) { s.last_error = trim(l.substr(15)); s.phase = Phase::Failed; s.detail = "Could not join: " + s.last_error; return true; }
    if (l.compare(0, 8, "[launch]") == 0) {
        if (l.find("mGBA exited") != std::string::npos) { s.mgba_exited = true; s.phase = Phase::Ended; s.detail = phase_text(s.phase, s.host); return true; }
        if (l.find("Starting the game window") != std::string::npos) { if (s.phase == Phase::Idle) s.phase = Phase::Starting; return false; }
        s.last_error = trim(l.substr(8)); s.phase = Phase::Failed; s.detail = s.last_error; return true;
    }
    if (l.compare(0, 12, "relay error:") == 0) { s.last_error = trim(l.substr(12)); return true; }
    /* trade (coop.lua LOG lines; species names already substituted by the bridge) */
    static const std::regex ro(R"(^you offer slot (\d+): (.+)$)");
    if (std::regex_search(l, m, ro)) {
        const std::string slot_s = m[1].str();   // untrusted text: no exceptions, 1..6 only
        if (slot_s.size() != 1 || slot_s[0] < '1' || slot_s[0] > '6') return false;
        s.trade.phase = TradePhase::Offered; s.trade.my_slot = slot_s[0] - '0'; s.trade.mine = m[2].str();
        s.trade.i_confirmed = false; s.trade.peer_confirmed = false;
        s.trade.note = "You offer " + s.trade.mine + (s.trade.theirs.empty() ? ". Waiting for your partner's offer." : ". Both offers are in: confirm to swap.");
        return true;
    }
    static const std::regex rp(R"(^peer offers: (.+?)\s+->)");
    if (std::regex_search(l, m, rp)) {
        s.trade.phase = TradePhase::Offered; s.trade.theirs = m[1].str(); s.trade.peer_confirmed = false;
        s.trade.note = "Your partner offers " + s.trade.theirs + (s.trade.mine.empty() ? ". Pick one of your team to offer." : ". Both offers are in: confirm to swap.");
        return true;
    }
    if (l == "confirmed; waiting for the peer") { s.trade.i_confirmed = true; s.trade.note = "You confirmed. Waiting for your partner."; return true; }
    if (l == "peer confirmed the trade") { s.trade.peer_confirmed = true; s.trade.note = "Your partner confirmed."; return true; }
    if (l.compare(0, 11, "TRADE DONE:") == 0) {
        s.trade = Trade(); s.trade.phase = TradePhase::Done; ++s.trades_done; s.trade.note = "Trade complete! " + trim(l.substr(11)); return true;
    }
    if (l.compare(0, 16, "trade cancelled:") == 0) {
        std::string why = trim(l.substr(16)); s.trade = Trade(); s.trade.phase = TradePhase::Cancelled; s.trade.note = "Trade cancelled (" + why + ")."; return true;
    }
    if (l.compare(0, 18, "TRADE INCONSISTENT") == 0) { s.trade = Trade(); s.trade.phase = TradePhase::Cancelled; s.trade.note = "The trade could not finish: " + l; return true; }
    if (l.compare(0, 14, "trade prepared") == 0) { s.trade.note = "Trade is being finished by the host..."; return true; }
    if (l.compare(0, 6, "usage:") == 0 || l == "no peer connected" || l == "ROM not detected yet" || l.compare(0, 4, "that") == 0 ||
        l == "both players must offer first" || l.compare(0, 12, "an earlier t") == 0 || l.compare(0, 8, "too late")  == 0) {
        s.trade.note = l; return true;
    }
    return false;
}

/* ------------------------------ texts ------------------------------ */
std::string first_session_text(bool host) {
    return std::string("First time? Both of you choose NEW GAME and type your own names. ") +
           (host ? "You (host) pick your starter first; Oak then asks you to pick your partner's starter too (talk it over)."
                 : "Use a brand-new game: the first time you reach the game world you are synced to the host's story (you keep your own name, team and bag).") +
           " Save in the game soon after (Start menu > SAVE).";
}
const char* credit_text() {
    return "Kanto Together is a fan project made by a friend; RetroHub only helps you run your own copy. It uses mGBA (MPL 2.0) and Node.js (MIT), and is derived from the "
           "pret/pokefirered decompilation. Nothing from the package is shipped with RetroHub, and it contains no Nintendo ROM. Licences: LICENSES.txt in the installed package.";
}
std::string howto_text() {
    return
        "# What it is\n"
        "A two-player co-op version of Pokemon FireRed. You each play on your own PC; the games are linked, so you see each other in the game and walk the story together.\n"
        "# Before you start\n"
        "Both of you need the Kanto Together zip in a RetroHub ROM folder and your own Pokemon FireRed (USA) 1.0 ROM in your library. RetroHub installs and builds the game for you the first time.\n"
        "# Playing together\n"
        "1. One of you hosts a room, the other joins with the code (or from a friend invite).\n"
        "2. The host picks Kanto Together, the guest presses Ready, the host presses Launch.\n"
        "3. The game window opens on both PCs. RetroHub shows the connection status and the Trade panel in the room.\n"
        "# First session\n"
        "Both choose NEW GAME and type your own names. The host picks their starter first, then the guest's. Save in the game soon after (Start > SAVE), especially the guest.\n"
        "# What is shared\n"
        "Shared: the story (badges, beaten trainers, items on the ground, doors), the Pokedex and the map you are on. TMs, HMs, key items and gift Pokemon are copied to both. "
        "Not shared: your team, bag, money, name. Trainer battles are double battles with your partner's Pokemon.\n"
        "# Trading\n"
        "Stand still in the game. In the Trade panel pick a team slot (1 = first Pokemon). You both see what the other offers; then both press Confirm. Cancel backs out.\n"
        "# Controls (mGBA)\n"
        "Arrow keys = D-pad, X = A, Z = B, A = L, S = R, Enter = Start, Backspace = Select. Click the game window first. Xbox-type pads work through mGBA's own pad support.\n"
        "# Saves\n"
        "Each player keeps their own save. RetroHub stores it in your save vault, so it follows you across your PCs.\n"
        "# If the connection drops\n"
        "It reconnects by itself. If the host restarts the game, the new room code is sent to the guest automatically.\n";
}

#ifdef _WIN32
/* ------------------------------ Session ------------------------------ */
static std::wstring env_block(const std::vector<std::pair<std::string, std::string>>& add) {
    std::wstring blk;
    LPWCH env = GetEnvironmentStringsW();
    for (LPWCH p = env; *p;) {
        std::wstring kv(p);
        p += kv.size() + 1;
        bool skip = false;
        for (auto& a : add) {
            std::wstring k = winutil::to_wide(a.first) + L"=";
            if (_wcsnicmp(kv.c_str(), k.c_str(), k.size()) == 0) skip = true;
        }
        if (!skip) { blk += kv; blk.push_back(L'\0'); }
    }
    FreeEnvironmentStringsW(env);
    for (auto& a : add) { blk += winutil::to_wide(a.first + "=" + a.second); blk.push_back(L'\0'); }
    blk.push_back(L'\0');
    return blk;
}

struct Spawned { HANDLE proc = nullptr, in_w = nullptr, out_r = nullptr; DWORD pid = 0; };
static bool spawn_piped(const std::string& cmdline, const std::string& cwd, const std::string& desktop,
                        const std::vector<std::pair<std::string, std::string>>& env, Spawned* out, HANDLE job) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) return false;
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r; si.StartupInfo.hStdOutput = out_w; si.StartupInfo.hStdError = out_w;
    /* Inherit exactly the two pipe ends meant for the child, nothing else the hub holds. */
    HANDLE inherit[2] = {in_r, out_w};
    SIZE_T attr_sz = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_sz);
    std::vector<char> attr_buf(attr_sz);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attr_buf.data();
    bool attr_ok = attr_sz && InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_sz) &&
                   UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);
    if (!attr_ok) { CloseHandle(in_r); CloseHandle(in_w); CloseHandle(out_r); CloseHandle(out_w); return false; }
    std::wstring desk = winutil::to_wide(desktop);
    if (!desk.empty()) {
        CreateDesktopW(desk.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        si.StartupInfo.lpDesktop = &desk[0];
    }
    std::wstring cmd = winutil::to_wide(cmdline);
    std::wstring blk = env_block(env);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, (LPVOID)blk.data(),
                             cwd.empty() ? nullptr : winutil::to_wide(cwd).c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(in_r); CloseHandle(out_w);
    if (!ok) { CloseHandle(in_w); CloseHandle(out_r); return false; }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {   // never let node/mGBA run outside the kill-on-close job
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(in_w); CloseHandle(out_r);
        return false;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    out->proc = pi.hProcess; out->in_w = in_w; out->out_r = out_r; out->pid = pi.dwProcessId;
    return true;
}

int free_loopback_port() {
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    int port = 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s != INVALID_SOCKET) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(s, (sockaddr*)&a, sizeof a) == 0) { int n = sizeof a; if (getsockname(s, (sockaddr*)&a, &n) == 0) port = ntohs(a.sin_port); }
        closesocket(s);
    }
    return port;
}

bool Session::start(const LaunchOpts& o_in, std::string* err) {
    LaunchOpts o = o_in;
    if (o.lua_port <= 0) o.lua_port = free_loopback_port();   // the package's default 8788 would clash with a second instance
    if (started_) { *err = "already running"; return false; }
    if (!is_installed(o.dir)) { *err = "Kanto Together is not installed."; return false; }
    if (!o.host && !valid_code(o.code)) { *err = "That does not look like a room code."; return false; }
    std::error_code ec;
    fs::create_directories(P(o.saves), ec);
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof li)) {
        if (job_) { CloseHandle(job_); job_ = nullptr; }
        *err = "Could not set up the safety job for the game (Windows refused). Kanto Together was not started.";
        return false;
    }
    std::vector<std::pair<std::string, std::string>> env = {{"NODE_NO_WARNINGS", "1"}};
    if (!o.relay.empty()) env.push_back({"COOP_RELAY", o.relay});
    Spawned sp;
    if (!spawn_piped(launcher_cmdline(o), o.dir, o.desktop, env, &sp, job_)) {
        CloseHandle(job_); job_ = nullptr;
        *err = "Could not start the Kanto Together launcher.";
        return false;
    }
    desktop_ = o.desktop;
    proc_ = sp.proc; in_w_ = sp.in_w; out_r_ = sp.out_r; pid_ = sp.pid;
    started_ = true;
    st_ = State();
    st_.host = o.host;
    st_.phase = Phase::Starting;
    st_.detail = phase_text(Phase::Starting, o.host);
    buf_.clear();
    log_.clear();
    return true;
}

void Session::update() {
    if (!out_r_) return;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(out_r_, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        char tmp[4096];
        DWORD got = 0;
        if (!ReadFile(out_r_, tmp, (DWORD)std::min<DWORD>(avail, sizeof tmp), &got, nullptr) || !got) break;
        buf_.append(tmp, got);
    }
    size_t nl;
    while ((nl = buf_.find('\n')) != std::string::npos) {
        std::string line = trim(buf_.substr(0, nl));
        buf_.erase(0, nl + 1);
        if (line.empty()) continue;
        log_.push_back(line);
        if (log_.size() > 300) log_.pop_front();
        feed_line(st_, line);
    }
    if (!running() && st_.phase != Phase::Ended && st_.phase != Phase::Failed) { st_.phase = Phase::Ended; st_.detail = phase_text(Phase::Ended, st_.host); }
}

bool Session::running() const {
    return proc_ && WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT;
}
void Session::send(const std::string& cmd) {
    if (cmd.empty() || !in_w_) return;
    DWORD w = 0;
    WriteFile(in_w_, cmd.data(), (DWORD)cmd.size(), &w, nullptr);
}
HANDLE Session::dup_handle() const {
    HANDLE h = nullptr;
    if (proc_) DuplicateHandle(GetCurrentProcess(), proc_, GetCurrentProcess(), &h, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0);
    return h;
}
static std::atomic<int> g_stops{0};
int Session::stopping() { return g_stops.load(); }
void Session::drain_stops(unsigned timeout_ms) {
    ULONGLONG t0 = GetTickCount64();
    while (g_stops.load() > 0 && GetTickCount64() - t0 < timeout_ms) Sleep(20);
}

struct StopCtx { HANDLE proc, in_w, out_r, job; std::wstring desktop; };

static BOOL CALLBACK close_enum(HWND h, LPARAM lp) {
    const std::vector<DWORD>* pids = (const std::vector<DWORD>*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (std::find(pids->begin(), pids->end(), pid) != pids->end() && GetWindow(h, GW_OWNER) == nullptr) PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}

static void stop_worker(StopCtx c) {
    auto alive = [&]() { return c.proc && WaitForSingleObject(c.proc, 0) == WAIT_TIMEOUT; };
    if (alive()) {
        /* mGBA (a child of node, inside the job) gets WM_CLOSE like a click on X: it flushes its save before exiting; node exits right after. */
        std::vector<DWORD> pids;
        if (c.job) {
            std::vector<char> buf(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 64 * sizeof(ULONG_PTR));
            auto* l = (JOBOBJECT_BASIC_PROCESS_ID_LIST*)buf.data();
            if (QueryInformationJobObject(c.job, JobObjectBasicProcessIdList, l, (DWORD)buf.size(), nullptr))
                for (DWORD i = 0; i < l->NumberOfAssignedProcesses; ++i) pids.push_back((DWORD)l->ProcessIdList[i]);
        }
        HDESK hd = c.desktop.empty() ? nullptr : OpenDesktopW(c.desktop.c_str(), 0, FALSE, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE);
        if (hd) { EnumDesktopWindows(hd, close_enum, (LPARAM)&pids); CloseDesktop(hd); }
        else EnumWindows(close_enum, (LPARAM)&pids);   // this thread's desktop (the normal one)
        if (WaitForSingleObject(c.proc, 8000) != WAIT_OBJECT_0 && c.in_w) {
            DWORD w = 0;
            std::string q = cmd_quit();
            WriteFile(c.in_w, q.data(), (DWORD)q.size(), &w, nullptr);
            WaitForSingleObject(c.proc, 2000);
        }
    }
    if (c.job) CloseHandle(c.job);   // kills anything still running
    if (c.proc) CloseHandle(c.proc);
    if (c.in_w) CloseHandle(c.in_w);
    if (c.out_r) CloseHandle(c.out_r);
    --g_stops;
}

void Session::stop(bool wait) {
    if (!started_) return;
    update();   // last output lines
    StopCtx c{proc_, in_w_, out_r_, job_, winutil::to_wide(desktop_)};
    proc_ = in_w_ = out_r_ = job_ = nullptr;
    started_ = false;
    if (st_.phase != Phase::Failed) { st_.phase = Phase::Ended; st_.detail = phase_text(Phase::Ended, st_.host); }
    ++g_stops;
    std::thread t(stop_worker, c);
    if (wait) t.join(); else t.detach();
}

bool run_hidden(const std::string& cmdline, const std::string& cwd, const std::string& desktop, DWORD timeout_ms, std::string* output, DWORD* code,
                const std::atomic<bool>* cancel) {
    Spawned sp;
    if (!spawn_piped(cmdline, cwd, desktop, {}, &sp, nullptr)) return false;
    ULONGLONG t0 = GetTickCount64();
    std::string out;
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(sp.out_r, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            char tmp[4096]; DWORD got = 0;
            if (!ReadFile(sp.out_r, tmp, std::min<DWORD>(avail, sizeof tmp), &got, nullptr) || !got) break;
            out.append(tmp, got);
        }
        if (WaitForSingleObject(sp.proc, 50) == WAIT_OBJECT_0) break;
        if (GetTickCount64() - t0 > timeout_ms || (cancel && cancel->load())) { TerminateProcess(sp.proc, 1); WaitForSingleObject(sp.proc, 2000); break; }
    }
    DWORD avail = 0;
    while (PeekNamedPipe(sp.out_r, nullptr, 0, nullptr, &avail, nullptr) && avail) {
        char tmp[4096]; DWORD got = 0;
        if (!ReadFile(sp.out_r, tmp, std::min<DWORD>(avail, sizeof tmp), &got, nullptr) || !got) break;
        out.append(tmp, got);
    }
    DWORD c = 1;
    GetExitCodeProcess(sp.proc, &c);
    CloseHandle(sp.proc); CloseHandle(sp.in_w); CloseHandle(sp.out_r);
    if (output) *output = out;
    if (code) *code = c;
    return true;
}

/* ------------------------------ Prep ------------------------------ */
float Prep::progress() const { uint64_t t = total_.load(); return t ? (float)((double)got_.load() / (double)t) : 0.f; }
std::string Prep::message() const { std::lock_guard<std::mutex> l(mu_); return msg_; }
std::string Prep::reason() const { std::lock_guard<std::mutex> l(mu_); return reason_; }

void Prep::start(const Package& pkg, const std::string& data_dir, const std::vector<RomCandidate>& cands, const std::string& desktop) {
    if (busy()) return;
    join();
    cancel_ = false; got_ = 0; total_ = 0;
    rom_state_ = RomState::Ok;
    { std::lock_guard<std::mutex> l(mu_); msg_ = "Preparing Kanto Together..."; reason_.clear(); }
    dir_ = install_dir(data_dir);
    stage_ = Stage::Installing;
    th_ = std::thread([this, pkg, data_dir, cands, desktop]() { run(pkg, data_dir, cands, desktop); });
}

void Prep::run(Package pkg, std::string data_dir, std::vector<RomCandidate> cands, std::string desktop) {
    auto fail = [&](const std::string& m, const std::string& why) { { std::lock_guard<std::mutex> l(mu_); msg_ = m; reason_ = why; } stage_ = Stage::Failed; };
    std::string dir = install_dir(data_dir);
    if (!install_current(dir, pkg)) {
        { std::lock_guard<std::mutex> l(mu_); msg_ = "Installing Kanto Together..."; }
        std::string err;
        bool ok = install(pkg, dir, [this](uint64_t d, uint64_t t) { got_ = d; total_ = t; return !cancel_.load(); }, &err);
        if (!ok) return fail("Install failed: " + err, cancel_.load() ? "cancelled" : "install");
    }
    stage_ = Stage::FindingRom;
    PatchManifest pm;
    if (!read_manifest(dir + "patch\\patch.json", &pm)) return fail("The package's patch description is unreadable.", "package");
    if (pm.base_sha1 != lower(kBaseSha1)) return fail("This Kanto Together package expects a different base ROM than FireRed (USA) 1.0, which RetroHub does not support.", "package");
    std::string out = rom_path(dir);
    std::error_code ec;
    if (fs::exists(P(out), ec) && sha1_file(out) == pm.target_sha1) { stage_ = Stage::Done; { std::lock_guard<std::mutex> l(mu_); msg_ = "Ready."; } return; }
    { std::lock_guard<std::mutex> l(mu_); msg_ = "Checking your FireRed ROM..."; }
    RomPick pick = pick_rom(cands, kBaseSha1);   // the constant, not only the package's own manifest
    rom_state_ = pick.state;
    if (pick.state != RomState::Ok) return fail(rom_message(pick), reason_for(pick.state));
    if (cancel_.load()) return fail("Cancelled.", "cancelled");
    stage_ = Stage::Patching;
    { std::lock_guard<std::mutex> l(mu_); msg_ = "Building the game from your FireRed..."; }
    fs::create_directories(P(dir) / "rom", ec);
    std::string output;
    DWORD code = 1;
    bool started = run_hidden(patch_cmdline(dir, pick.path), dir, desktop, 120000, &output, &code, &cancel_);
    if (cancel_.load()) return fail("Cancelled.", "cancelled");
    if (!started || code != 0 || sha1_file(out) != pm.target_sha1)
        return fail("Could not build the game: " + trim(output.substr(output.size() > 300 ? output.size() - 300 : 0)), "patch");
    stage_ = Stage::Done;
    { std::lock_guard<std::mutex> l(mu_); msg_ = "Ready."; }
}
#endif

}  // namespace kanto
