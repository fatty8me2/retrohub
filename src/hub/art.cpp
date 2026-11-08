#include "winutil.h"
#include "art.h"
#include "art_match.h"
#include "art_lru.h"
#include "gametdb.h"
#include "netgate.h"
#include "gameinfo.h"
#include "nx.h"
#include "wiiu_meta.h"
#include "theme.h"

#include "library.h"
#include "games.h"
#include "hack_logic.h"

#include <algorithm>
#include <cstring>
#include <SDL.h>

#include <windows.h>
#include <winhttp.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG   // GameTDB DS / 3DS / Wii U covers and Switch package icons are JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
namespace hm = hubart_match;

namespace hubart {
namespace {

const int kMaxTexHeight = 512;
const int kFullMaxW = 2048, kFullMaxH = 1536;   // FullCover scans keep more detail (the whole sheet is drawn across a case)
const long long kMaxPixels = 16LL * 1000 * 1000;   // refuse decompression bombs before allocating (a 16 Mpx RGBA buffer is 64 MB)
const int kMaxSide = 8192, kMaxBoxW = 4096;
const int kUploadsPerFrame = 8;
const long long kMissRetrySeconds = 7LL * 24 * 3600;
const long long kDatRefreshSeconds = 60LL * 24 * 3600;
const long long kDatRetrySeconds = 120;                   // a DAT download that failed is tried again after this
const long long kArtRetryMinSeconds = 30, kArtRetryMaxSeconds = 600;   // transient cover failures: 30 s, 60 s, ... up to 10 min

struct Pixels {
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
};

enum class State { Pending, Ready, Failed };

struct Slot {
    State st = State::Pending;
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    uint32_t avg = 0;   // average saturated colour (theme "Accent from cover"), 0 = none
    bool has_blur = false;
    uint32_t blur[64] = {};   // 8x8 box-averaged copy of the art (packed like IM_COL32; Cases view: the soft background under a square icon)
    long long retry_at = 0;   // Failed by a transient error (offline, HTTP 5xx): ask again from this unix second (0 = never; 404 / bad image stay final)
    int fails = 0;
    std::string name;         // the art name that loaded ("Super Mario 64 (USA)", "gametdb:ALZE01:US", "nupicon:<id>"); audit only
    std::string err;          // why it failed (final or transient); audit only
};

struct Job {
    std::string id;          // texture-cache id
    bool by_name = false;
    hub::GameEntry game;     // when !by_name
    std::string system;      // by_name
    std::string name;        // by_name
    Kind kind = Kind::Boxart;
};

struct Done {
    std::string id;
    bool ok = false;
    bool transient = false;   // failed because of the network (not a 404): worth another try later
    std::string name, err;
    Pixels px;
};

SDL_Renderer* g_renderer = nullptr;
std::string g_cache;                 // with trailing separator
std::atomic<bool> g_enabled{true};
bool g_inited = false;

std::mutex g_mu;                     // guards everything below
std::condition_variable g_cv;
std::deque<Job> g_jobs;
std::deque<Done> g_done;
std::unordered_map<std::string, Slot> g_slots;   // UI thread only (plus shutdown)
Lru g_lru;                                       // UI thread only: Ready textures by last-drawn frame
std::map<std::string, std::string> g_names;      // game key -> No-Intro name
std::map<std::string, std::string> g_misses;     // id -> unix time
bool g_names_dirty = false, g_misses_dirty = false;
std::thread g_worker;
bool g_quit = false;

std::mutex g_flush_mu;               // serialises names/misses writers (never taken under g_mu)
std::mutex g_dat_mu;
std::map<std::string, std::vector<hm::DatRec>> g_dats;
std::map<std::string, long long> g_dat_failed;    // slot -> unix second before which it is not tried again (library-6: an offline start must not stick)

/* ---------- small helpers ---------- */

fs::path P(const std::string& u8) { return fs::u8path(u8); }

bool read_file(const fs::path& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}
bool write_file_atomic(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(p, ec); ec.clear(); fs::rename(tmp, p, ec); }
    return !ec;
}

long long now_s() { return (long long)std::time(nullptr); }

std::wstring widen(const std::string& s) { return winutil::to_wide(s); }

const char* kind_name(Kind k) { return k == Kind::Boxart ? "boxart" : k == Kind::Title ? "title" : k == Kind::FullCover ? "coverfull" : "snap"; }

std::string safe_file_name(const std::string& n) {
    std::string r = hm::thumb_name(n);
    for (auto& c : r) if (c == '"') c = '_';
    return r;
}

/* ---------- persisted maps ---------- */

/* Miss record versions: every time the matching improves the prefixes below are bumped, so a game recorded as a "known miss" by an older
 * version is looked up once more. Current: images "art3:", owned names "name3:", ghost names "name4:". Older records are dropped at load. */
const char* const kMissArt = "art3:";
const char* const kMissName = "name3:";
const char* const kMissGhost = "name4:";
bool stale_miss_key(const std::string& k) {
    return k.compare(0, 5, "art2:") == 0 || k.compare(0, 4, "art:") == 0 || k.compare(0, 5, "name:") == 0 || k.compare(0, 6, "name2:") == 0;
}

void load_maps() {
    std::string t;
    std::map<std::string, std::string> m;
    if (read_file(P(g_cache + "names.json"), &t) && hm::map_from_json(t, &m)) g_names = m;
    if (read_file(P(g_cache + "misses.json"), &t) && hm::map_from_json(t, &m)) {
        g_misses = m;
        for (auto it = g_misses.begin(); it != g_misses.end();) {
            if (stale_miss_key(it->first)) { it = g_misses.erase(it); g_misses_dirty = true; } else ++it;
        }
    }
}
/* Snapshot under g_mu, write with no lock held (the UI thread takes g_mu every frame).
   Callers must NOT hold g_mu. g_flush_mu serialises writers. */
void flush_maps() {
    std::lock_guard<std::mutex> fl(g_flush_mu);
    std::string names, misses;
    std::map<std::string, std::string> nm, ms;
    bool wn = false, wm = false;
    {
        std::lock_guard<std::mutex> l(g_mu);   // copy only; serialise after releasing
        if (g_names_dirty) { nm = g_names; g_names_dirty = false; wn = true; }
        if (g_misses_dirty) { ms = g_misses; g_misses_dirty = false; wm = true; }
    }
    if (wn) names = hm::map_to_json(nm);
    if (wm) misses = hm::map_to_json(ms);
    bool okn = !wn || write_file_atomic(P(g_cache + "names.json"), names);
    bool okm = !wm || write_file_atomic(P(g_cache + "misses.json"), misses);
    if (!okn || !okm) {
        std::lock_guard<std::mutex> l(g_mu);
        if (!okn) g_names_dirty = true;
        if (!okm) g_misses_dirty = true;
    }
}
/* names.json / misses.json are rewritten whole: a big library resolving hundreds of games must not rewrite them per game. A change is
 * written at once if the last write was more than 2 s ago, else left dirty for flush_if_due() (the worker's idle tick), shutdown or detail::flush. */
std::mutex g_deb_mu;
std::chrono::steady_clock::time_point g_last_flush;
bool g_flush_pending = false;

void request_flush() {
    {
        std::lock_guard<std::mutex> l(g_deb_mu);
        auto now = std::chrono::steady_clock::now();
        if (now - g_last_flush < std::chrono::seconds(2)) { g_flush_pending = true; return; }
        g_last_flush = now;
        g_flush_pending = false;
    }
    flush_maps();
}
void flush_if_due() {
    {
        std::lock_guard<std::mutex> l(g_deb_mu);
        if (!g_flush_pending || std::chrono::steady_clock::now() - g_last_flush < std::chrono::seconds(2)) return;
        g_last_flush = std::chrono::steady_clock::now();
        g_flush_pending = false;
    }
    flush_maps();
}

bool is_missed(const std::string& id) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_misses.find(id);
    if (it == g_misses.end()) return false;
    long long t = std::atoll(it->second.c_str());
    if (now_s() - t > kMissRetrySeconds) {
        g_misses.erase(it);
        g_misses_dirty = true;
        return false;
    }
    return true;
}
void record_miss(const std::string& id) {
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_misses[id] = std::to_string(now_s());
        g_misses_dirty = true;
    }
    request_flush();
}

/* ---------- WinHTTP ---------- */

/* Politeness (per-site spacing, 429 / Retry-After blocks) lives in common/netgate.*, shared with the game-info client. */

/* status: HTTP status, or 0 for a transport failure. */
bool http_get(const std::string& url, std::string* body, int* status, size_t max_bytes = 64u << 20) {
    *status = 0;
    body->clear();
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;  uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;   uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
    const std::string host_u8(winutil::to_utf8(std::wstring(host, uc.dwHostNameLength)));
    if (!netgate::acquire(host_u8)) return false;   // spaced per site, and blocked while a Retry-After runs

    /* One shared session for the process (WinHTTP sessions are thread-safe). */
    static std::mutex ses_mu;
    static HINTERNET ses_h = nullptr;
    HINTERNET ses;
    {
        std::lock_guard<std::mutex> l(ses_mu);
        if (!ses_h) {   // retried on the next request if opening failed
            ses_h = WinHttpOpen(L"RetroHub", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
            if (ses_h) WinHttpSetTimeouts(ses_h, 5000, 5000, 10000, 20000);
        }
        ses = ses_h;
    }
    if (!ses) return false;
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &sz, WINHTTP_NO_HEADER_INDEX);
        *status = (int)code;
        if (code == 429 || code == 503) {
            wchar_t ra[32] = {};
            DWORD rl = sizeof ra;
            int secs = 120;
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"Retry-After", ra, &rl, WINHTTP_NO_HEADER_INDEX)) secs = _wtoi(ra) > 0 ? _wtoi(ra) : 120;
            netgate::block(host_u8, secs);
        }
        ok = true;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
            if (avail == 0) break;
            size_t old = body->size();
            if (old + avail > max_bytes) { ok = false; break; }
            body->resize(old + avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, &(*body)[old], avail, &got)) { ok = false; break; }
            body->resize(old + got);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (!ok) *status = 0;
    return ok;
}

/* ---------- DAT cache ---------- */

/* tree = the libretro-thumbnails repo's real file names (GitHub tree JSON) instead of the No-Intro / Redump DAT. */
const std::vector<hm::DatRec>* get_dat(const std::string& system, bool tree = false) {
    if (system == "wiiu") tree = false;   // its DAT already is the tree
    const std::string slot = tree ? system + "#tree" : system;
    std::lock_guard<std::mutex> l(g_dat_mu);
    auto it = g_dats.find(slot);
    if (it != g_dats.end()) return &it->second;
    { auto f = g_dat_failed.find(slot); if (f != g_dat_failed.end() && now_s() < f->second) return nullptr; }
    std::string url = tree ? hm::tree_url(system) : hm::dat_url(system);
    if (url.empty()) return nullptr;
    fs::path p = P(g_cache + "dat/" + system + (tree ? ".tree" : ".dat"));
    std::string text;
    bool have = read_file(p, &text);
    bool stale = false;
    if (have) {
        std::error_code ec;
        auto ft = fs::last_write_time(p, ec);
        if (!ec) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now() - ft).count();
            stale = age > kDatRefreshSeconds;
        }
    }
    if (!have || stale) {
        std::string body;
        int st = 0;
        if (http_get(url, &body, &st, 32u << 20) && st == 200 && body.size() > 1000) {
            write_file_atomic(p, body);
            text = body;
            have = true;
        } else if (!have) {
            g_dat_failed[slot] = now_s() + kDatRetrySeconds;
            return nullptr;
        }
    }
    auto recs = (tree || system == "wiiu") ? hm::parse_wiiu_tree(text) : hm::parse_dat(text);
    if (recs.empty()) { g_dat_failed[slot] = now_s() + kDatRetrySeconds; return nullptr; }
    g_dat_failed.erase(slot);
    return &(g_dats[slot] = std::move(recs));
}

/* ---------- name resolution ---------- */

std::string tail_after_last(const std::string& s, char c) {
    size_t p = s.rfind(c);
    return p == std::string::npos ? "" : s.substr(p + 1);
}

/* A friend / pack ghost has path "ghost" or "ghost|<pack entry name>"; the entry name (No-Intro / Redump style, region tag kept) is a lookup hint. */
bool is_ghost_path(const std::string& p) { return p.compare(0, 5, "ghost") == 0 && (p.size() == 5 || p[5] == '|'); }
std::string ghost_hint(const std::string& p) { return is_ghost_path(p) && p.size() > 6 ? p.substr(6) : ""; }

/* Fan hacks without a retail release of their own borrow the base game's box: catalog id -> (system, No-Intro name). */
const char* hack_base_name(const std::string& catalog_id) {
    if (catalog_id == "remix") return "Super Smash Bros. (USA)";
    return nullptr;
}

/* Catalog entry for a ghost: same console, same title once articles / accents / punctuation are folded. */
const hub::GameInfo* catalog_by_title(const std::string& sys, const std::string& title) {
    std::string k = hm::title_key(title);
    if (k.size() < 3) return nullptr;
    if (sys == "n64" && k.compare(0, 10, "smashremix") == 0) return hub::find_game("remix");   // any Smash Remix version
    for (const hub::GameInfo& gi : hub::catalog())
        if (gi.system == sys && hm::title_key(gi.title) == k) return &gi;
    return nullptr;
}

/* ---------- GameTDB (cover scans + id lookup), Switch package icons ---------- */

/* Only the art worker thread (and the headless tool) may download a GameTDB database, and only when a Wii U / Switch title really has to be mapped
 * to a product code. Every other caller (the UI thread through detail::resolve_name, the shortcut maker) reads what is already on disk. */
thread_local bool t_may_download = false;
struct DownloadScope {
    bool old;
    DownloadScope() : old(t_may_download) { t_may_download = true; }
    ~DownloadScope() { t_may_download = old; }
};

const gametdb::Index* tdb_index(const std::string& sys) {
    if (!t_may_download) return gametdb::cached_index(sys);
    return gametdb::index_for(sys, [](const std::string& u, std::string* b, int* st) { return http_get(u, b, st, 64u << 20); });
}

std::string folder_of_region(const std::string& region) {
    return region.find("NTSC-U") != std::string::npos ? "US" : region.find("PAL") != std::string::npos ? "EN" : region.find("NTSC-J") != std::string::npos ? "JA" : "";
}

std::string lower_ascii(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool ends_with_ci(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    return s.size() >= n && lower_ascii(s.substr(s.size() - n)) == suf;
}

/* "gametdb:<ID>:<folder>" for a game, "" when GameTDB cannot help (not one of GameCube / Wii U / DS / 3DS / Switch, or no id).
 * libretro_name (the No-Intro / Redump name already resolved, may be "") only supplies the region tag and a second title to try. */
std::string tdb_name_for(const hub::GameEntry& g, const std::string& libretro_name) {
    const std::string sys = hub::system_short(g.system);
    if (!gametdb::has_cover(sys) && sys != "switch") return "";
    std::string id = sys == "switch" ? "" : gametdb::id_from_serial(sys, g.serial);
    std::string hint = gametdb::region_hint_from_name(libretro_name.empty() ? g.path : libretro_name);
    std::string folder = "*";
    if (id.empty() && (sys == "wiiu" || sys == "switch") && !(sys == "switch" && g.nx2)) {   // no product code in the files we can read: the title picks the GameTDB entry
        if (const gametdb::Index* idx = tdb_index(sys)) {
            std::optional<gametdb::Rec> r = idx->by_title(g.title);
            if (!r && !libretro_name.empty()) r = idx->by_title(libretro_name);
            if (r) { id = r->id; folder = folder_of_region(r->region); if (folder.empty()) folder = "*"; }
        }
    }
    if (id.empty()) return "";
    if (folder == "*" && !hint.empty()) { auto f = gametdb::region_folders(id, hint); if (!f.empty()) folder = f.front(); }
    return hm::gametdb_name_for(id, folder);
}

/* A Switch .nup holds the game's own icon in its package meta: cached under the pseudo name "nupicon:<title id>" (JPEG / PNG bytes). */
std::string nup_icon_name(const hub::GameEntry& g) {
    if (g.system != hub::System::Switch || !ends_with_ci(g.path, ".nup")) return "";
    std::string tid = g.key.compare(0, 7, "switch:") == 0 ? g.key.substr(7) : g.key;
    return tid.empty() ? "" : "nupicon:" + tid;
}

bool ensure_nup_icon(const hub::GameEntry& g, const std::string& name) {
    fs::path file = P(g_cache + "switch/boxart/" + safe_file_name(name) + ".png");
    std::error_code ec;
    if (fs::exists(file, ec)) return true;
    std::vector<uint8_t> icon;
    if (!hub::nx::nup_icon_file(g.path, &icon) || icon.empty()) return false;
    return write_file_atomic(file, std::string((const char*)icon.data(), icon.size()));
}

bool resolve_name_impl(const hub::GameEntry& g, std::string* name, std::string* how) {
    {   /* a ROM hack / translation borrows its base game's box: look the cover up under the base key (N64 md5, GBA / NES / SNES crc come from it) */
        std::string base_key;
        if (!g.hack_patch.empty() && hubui::hacks::parse_key(g.key, &base_key, nullptr)) {
            hub::GameEntry b = g;
            b.key = base_key;
            b.hack_patch.clear();
            b.hack_base.clear();
            return resolve_name_impl(b, name, how);
        }
    }
    std::string sys = hub::system_short(g.system);
    const bool ghost = is_ghost_path(g.path);
    if (g.system == hub::System::Switch) {   // no libretro set: a real box cover from GameTDB when its title matches, else the package's own icon
        {
            std::lock_guard<std::mutex> l(g_mu);
            auto it = g_names.find(g.key);
            if (it != g_names.end() && !it->second.empty()) { *name = it->second; if (how) *how = "cache"; return true; }
        }
        std::string n = tdb_name_for(g, ""), h2 = "gametdb";
        bool persist = true;
        if (n.empty()) {
            n = nup_icon_name(g);
            h2 = "nupicon";
            persist = g.nx2 || gametdb::cached_index("switch");   // GameTDB's index not there yet: the icon serves now, the box is looked for again next time
        }
        if (n.empty()) return false;
        if (persist) {
            {
                std::lock_guard<std::mutex> l(g_mu);
                g_names[g.key] = n;
                g_names_dirty = true;
            }
            request_flush();
        }
        *name = n;
        if (how) *how = h2;
        return true;
    }
    {
        /* A catalog name is authoritative: a cached plain name that differs from it (the catalog entry was corrected since, e.g. Master Quest used
         * to borrow Ocarina of Time's box) is replaced. Pseudo names (GameTDB / package icon fallbacks) stay: they record a fallback that worked. */
        const hub::GameInfo* cgi = g.catalog_id.empty() ? nullptr : hub::find_game(g.catalog_id);
        const std::string cat_name = cgi ? cgi->nointro : std::string();
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_names.find(g.key);
        if (it != g_names.end() && !it->second.empty()) {
            const bool pseudo = it->second.compare(0, 8, "gametdb:") == 0 || it->second.compare(0, 8, "nupicon:") == 0;
            if (pseudo || cat_name.empty() || it->second == cat_name) {
                *name = it->second;
                if (how) *how = "cache";
                return true;
            }
        }
    }
    const std::string miss_id = (ghost ? kMissGhost : kMissName) + g.key;   // versioned: misses recorded by older matching do not hide the improved lookup

    std::string found, h;
    bool skip_dat = false;
    std::string cat_id = g.catalog_id;
    if (cat_id.empty() && ghost) {   // a pack / friend ghost has no scan: match the catalog by loose title
        if (const hub::GameInfo* gi = catalog_by_title(sys, g.title)) cat_id = gi->id;
    }
    if (!cat_id.empty()) {
        if (const hub::GameInfo* gi = hub::find_game(cat_id)) {
            if (!gi->nointro.empty()) { found = gi->nointro; h = "catalog"; }
            else if (const char* base = hack_base_name(cat_id)) { found = base; h = "base"; }
            else skip_dat = true;   // curated entry with no retail release (ROM hack, native port): no art
        }
    }
    /* A recorded name miss only gates the DAT search: a catalog name is authoritative, so a miss left over
     * from before the entry matched the catalog must not hide its art. */
    if (found.empty() && is_missed(miss_id)) return false;
    if (skip_dat) {
        record_miss(miss_id);
        return false;
    }
    if (found.empty()) {
        if (const std::vector<hm::DatRec>* dat = get_dat(sys)) {
            hm::Query q;
            q.system = sys;
            q.serial = g.serial;
            q.title = g.title;
            q.fuzzy = ghost;   // friend / pack ghost entry (see get_ghost)
            q.hint = ghost_hint(g.path);
            if (g.system == hub::System::WiiU) {   // region from the file / archive name; the USA release is the fallback
                std::string pl = g.path;
                for (auto& c : pl) c = (char)std::tolower((unsigned char)c);
                q.region = pl.find("(europe") != std::string::npos ? "Europe" : pl.find("(japan") != std::string::npos ? "Japan" : "USA";
            }
            if (g.system == hub::System::N64) {
                size_t c = g.key.find(':');
                q.md5 = c == std::string::npos ? "" : g.key.substr(c + 1);
            } else if (g.system == hub::System::GBA) {
                q.crc = tail_after_last(g.key, '-');
            } else if (g.system == hub::System::NES || g.system == hub::System::SNES) {
                q.crc = tail_after_last(g.key, ':');   // headerless CRC32 = the No-Intro DAT's rom crc
            }
            found = hm::match_name(*dat, q, &h);
            if (found.empty() && sys == "gbc") {   // dual-mode carts (Pokemon Red / Yellow ...) are in the Game Boy DAT
                if (const std::vector<hm::DatRec>* gb = get_dat("gb")) { q.system = "gb"; found = hm::match_name(*gb, q, &h); }
            }
            if (found.empty() && ghost) {   // known title differences ("007 - GoldenEye")
                std::string al = hm::title_alias(sys, g.title);
                if (!al.empty()) { q.title = al; q.hint = al; found = hm::match_name(*dat, q, &h); }
            }
        } else {
            /* DAT unavailable (offline): don't remember a miss, try again next session */
            return false;
        }
    }
    if (found.empty() && g.system == hub::System::WiiU) {   // libretro-thumbnails does not have it: GameTDB by the product code in meta.xml
        std::string up = g.path;
        if (up.size() > 4 && up.compare(up.size() - 4, 4, ".wua") == 0) {
            std::ifstream f(P(up), std::ios::binary);
            std::error_code ec;
            uint64_t sz = fs::file_size(P(up), ec);
            hub::WiiuMeta wm;
            if (f && !ec) {
                hub::ReadAtFn rd = [&](uint64_t off, void* buf, size_t n) { f.clear(); f.seekg((std::streamoff)off); f.read((char*)buf, (std::streamsize)n); return f.gcount() == (std::streamsize)n; };
                if (hub::wua_read_meta(rd, sz, &wm)) { found = hm::gametdb_name(wm.game_id4, wm.region); h = "gametdb"; }
            }
        }
    }
    if (found.empty() && gametdb::has_cover(sys)) { found = tdb_name_for(g, ""); h = "gametdb"; }   // libretro-thumbnails does not know it: GameTDB by product code / title
    if (found.empty()) {
        record_miss(miss_id);
        return false;
    }
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_names[g.key] = found;
        g_names_dirty = true;
    }
    request_flush();
    *name = found;
    if (how) *how = h;
    return true;
}

/* ---------- image load / downscale ---------- */

/* Header-only check, before anything is allocated: a known format, each side <= 8192 and at most 16 Mpx. */
bool header_ok(const std::string& bytes, int* w, int* h) {
    int n = 0;
    if (!stbi_info_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), w, h, &n)) return false;
    return *w > 0 && *h > 0 && *w <= kMaxSide && *h <= kMaxSide && (long long)*w * *h <= kMaxPixels;
}

bool decode_png(const std::string& bytes, Pixels* out, int max_w = kMaxBoxW, int max_h = kMaxTexHeight) {
    int w = 0, h = 0, n = 0;
    if (!header_ok(bytes, &w, &h)) return false;
    unsigned char* d = stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!d) return false;
    if (w <= 0 || h <= 0) { stbi_image_free(d); return false; }
    if (h <= max_h && w <= max_w) {
        out->rgba.assign(d, d + (size_t)w * h * 4);
        out->w = w; out->h = h;
    } else {
        double sc = std::min((double)max_h / h, (double)max_w / w);
        int nh = std::max(1, (int)(h * sc + 0.5));
        int nw = std::max(1, (int)(w * sc + 0.5));
        out->rgba.resize((size_t)nw * nh * 4);
        out->w = nw; out->h = nh;
        for (int y = 0; y < nh; ++y) {
            int y0 = (int)((long long)y * h / nh), y1 = (int)((long long)(y + 1) * h / nh);
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < nw; ++x) {
                int x0 = (int)((long long)x * w / nw), x1 = (int)((long long)(x + 1) * w / nw);
                if (x1 <= x0) x1 = x0 + 1;
                double r = 0, gg = 0, b = 0, a = 0;
                int cnt = 0;
                for (int yy = y0; yy < y1; ++yy)
                    for (int xx = x0; xx < x1; ++xx) {
                        const unsigned char* s = d + ((size_t)yy * w + xx) * 4;
                        r += s[0] * (double)s[3]; gg += s[1] * (double)s[3]; b += s[2] * (double)s[3]; a += s[3];
                        ++cnt;
                    }
                unsigned char* o = &out->rgba[((size_t)y * nw + x) * 4];
                if (a > 0) {
                    o[0] = (unsigned char)(r / a + 0.5); o[1] = (unsigned char)(gg / a + 0.5);
                    o[2] = (unsigned char)(b / a + 0.5);
                } else {
                    o[0] = o[1] = o[2] = 0;
                }
                o[3] = (unsigned char)(a / cnt + 0.5);
            }
        }
    }
    stbi_image_free(d);
    return true;
}

/* Cached file or download. false: *err says why; *missed true when a 404 was recorded. */
bool load_image(const std::string& system, const std::string& name, Kind kind, Pixels* px, std::string* err,
                std::string* png_path) {
    if (name.empty()) { *err = "no name"; return false; }
    std::string miss_id = kMissArt + system + ":" + kind_name(kind) + ":" + name;
    fs::path file = P(g_cache + system + "/" + kind_name(kind) + "/" + safe_file_name(name) + ".png");
    if (png_path) *png_path = file.u8string();
    std::string bytes;
    const int cap_w = kind == Kind::FullCover ? kFullMaxW : kMaxBoxW, cap_h = kind == Kind::FullCover ? kFullMaxH : kMaxTexHeight;
    if (read_file(file, &bytes) && decode_png(bytes, px, cap_w, cap_h)) return true;
    if (name.compare(0, 8, "nupicon:") == 0) { *err = "no icon"; return false; }   // extracted from the package by the caller, never downloaded
    if (is_missed(miss_id)) { *err = "known miss"; return false; }
    std::vector<std::string> urls = hm::thumb_urls(system, (int)kind, name);
    if (urls.empty()) { *err = "unsupported system"; return false; }
    int st = 0;
    for (const std::string& url : urls) {   /* 404 on one repo: try the next (gbc -> gb) */
        if (!http_get(url, &bytes, &st)) { *err = "network error"; return false; }
        if (st != 404) break;
    }
    if (st == 404 && name.compare(0, 8, "gametdb:") != 0) {   /* the DAT name has no thumbnail: look for the same game in the repo's real file list */
        std::vector<std::string> trees = {system};
        if (system == "gbc") trees.push_back("gb");
        for (const std::string& ts : trees) {
            const std::vector<hm::DatRec>* tree = get_dat(ts, true);
            if (!tree) continue;
            hm::Query q;
            q.system = ts;
            q.title = name;
            q.region = hm::region_of(name);
            q.fuzzy = false;
            std::string alt = hm::match_name(*tree, q);
            if (alt.empty() || alt == name) continue;
            std::string u = hm::thumb_url(ts, (int)kind, alt);
            if (u.empty()) continue;
            if (!http_get(u, &bytes, &st)) { *err = "network error"; return false; }
            if (st != 404) { urls.assign(1, u); break; }
        }
    }
    if (st == 404) { record_miss(miss_id); *err = "404 (no art for this name)"; return false; }
    /* libretro-thumbnails stores some entries (e.g. the "(Rev 2)" 3DS boxarts) as git symlinks: raw.githubusercontent
     * serves the link text (a few dozen bytes, "Other Name.png") instead of an image. Follow it up to twice. */
    for (int hop = 0; hop < 2 && st == 200 && !urls.empty() && bytes.size() < 400 && !(bytes.size() > 4 && bytes.compare(1, 3, "PNG") == 0); ++hop) {
        std::string target = bytes;
        while (!target.empty() && (target.back() == '\n' || target.back() == '\r' || target.back() == ' ')) target.pop_back();
        if (target.size() < 5 || target.compare(target.size() - 4, 4, ".png") != 0) break;
        std::string base = urls.back();
        base.resize(base.rfind('/'));   // directory of the link
        while (target.compare(0, 3, "../") == 0) { target.erase(0, 3); base.resize(base.rfind('/')); }
        std::string enc;
        for (size_t a = 0; a <= target.size();) {   // encode per path segment, keep the '/' separators
            size_t b = target.find('/', a);
            if (b == std::string::npos) b = target.size();
            enc += "/" + hm::url_encode(target.substr(a, b - a));
            a = b + 1;
        }
        urls.back() = base + enc;
        if (!http_get(urls.back(), &bytes, &st)) { *err = "network error"; return false; }
    }
    if (st == 404) { record_miss(miss_id); *err = "404 (no art for this name)"; return false; }
    if (st != 200) { *err = "HTTP " + std::to_string(st); return false; }
    if (!decode_png(bytes, px, cap_w, cap_h)) { *err = "bad image"; return false; }
    write_file_atomic(file, bytes);
    return true;
}

/* ---------- worker ---------- */

/* One job, start to finish: resolve the name, fetch / read the image. Shared by the worker thread and the headless tool.
 * Fallbacks: a Boxart libretro has no thumbnail for tries GameTDB's front cover (GameCube / Wii U / DS / 3DS) and a Switch .nup's icon. */
bool run_job(const Job& j, Pixels* px, std::string* err, bool* transient, std::string* how, std::string* png_path, std::string* used = nullptr) {
    std::string sys = j.by_name ? j.system : hub::system_short(j.game.system);
    std::string name = j.name;
    bool ok = true, unresolved = false;
    *transient = false;
    if (j.kind == Kind::FullCover) {   // GameTDB only, by product code (Wii U: by title through the GameTDB index)
        if (j.by_name || !gametdb::has_fullcover(sys)) { *err = "no full cover for this system"; return false; }
        std::string libretro;
        { std::lock_guard<std::mutex> l(g_mu); auto it = g_names.find(j.game.key); if (it != g_names.end()) libretro = it->second; }
        name = tdb_name_for(j.game, libretro);
        if (name.empty()) {
            *err = "no GameTDB id";
            *transient = sys == "wiiu" && !gametdb::cached_index("wiiu");   // the Wii U index (title -> product code) could not be loaded: offline, ask again later
            return false;
        }
        if (how) *how = "gametdb";
        ok = load_image(sys, name, j.kind, px, err, png_path);
        if (ok && used) *used = name;
        *transient = !ok && (*err == "network error" || err->compare(0, 5, "HTTP ") == 0);
        if (!ok && !*transient && (*err == "known miss" || err->compare(0, 3, "404") == 0)) {   // no scan for this region: the same game's other regions (Phantom Hourglass US -> PAL, 3DS "EKJA" -> EKJE)
            std::string id, folder;
            if (hm::parse_gametdb_name(name, &id, &folder)) {
                const std::string hint = gametdb::region_hint_from_name(libretro.empty() ? j.game.path : libretro);
                int tried = 0;
                for (const auto& sib : gametdb::regional_siblings(id, hint)) {
                    if (tried >= (gametdb::cached_index(sys) ? 3 : 1)) break;   // without the index only the best guess is tried
                    if (const gametdb::Index* idx = gametdb::cached_index(sys)) { if (!idx->by_id(sib.first)) continue; }   // only ids GameTDB lists
                    ++tried;
                    const std::string sn = hm::gametdb_name_for(sib.first, sib.second + "!");
                    std::string e2;
                    if (load_image(sys, sn, j.kind, px, &e2, png_path)) {
                        ok = true;
                        if (used) *used = sn;
                        if (how) *how = "gametdb-sibling";
                        err->clear();
                        *transient = false;
                        return true;
                    }
                    if (e2 == "network error" || e2.compare(0, 5, "HTTP ") == 0) { *transient = true; *err = e2; break; }
                }
            }
        }
        return ok;
    }
    if (!j.by_name) { ok = resolve_name_impl(j.game, &name, how); unresolved = !ok; }
    if (ok) {
        if (name.compare(0, 8, "nupicon:") == 0 && !j.by_name) ensure_nup_icon(j.game, name);
        ok = load_image(sys, name, j.kind, px, err, png_path);
        if (ok && used) *used = name;
    }
    if (!ok && j.kind == Kind::Boxart && !j.by_name) {
        const bool final_fail = unresolved || err->compare(0, 3, "404") == 0 || *err == "known miss" || *err == "no icon" || *err == "bad image";
        if (final_fail) {
            std::vector<std::string> alts;
            std::string t = tdb_name_for(j.game, unresolved ? "" : name);
            if (!t.empty() && t != name) alts.push_back(t);
            std::string nup = nup_icon_name(j.game);
            if (!nup.empty() && nup != name) alts.push_back(nup);
            for (const std::string& alt : alts) {
                if (alt.compare(0, 8, "nupicon:") == 0) ensure_nup_icon(j.game, alt);
                std::string e2;
                if (load_image(sys, alt, j.kind, px, &e2, png_path)) {
                    ok = true;
                    if (used) *used = alt;
                    if (how) *how = alt.compare(0, 8, "nupicon:") == 0 ? "nupicon" : "gametdb";
                    {   // remember which name has art, so the next start goes straight there
                        std::lock_guard<std::mutex> l(g_mu);
                        g_names[j.game.key] = alt;
                        g_names_dirty = true;
                    }
                    request_flush();
                    break;
                }
                if (e2 == "network error" || e2.compare(0, 5, "HTTP ") == 0) *err = e2;
            }
        }
    }
    if (!ok) {   // library-6: the network (or a DAT that could not be fetched) is not a verdict on the cover; 404s are recorded as misses already
        bool net = *err == "network error" || err->compare(0, 5, "HTTP ") == 0;
        if (unresolved) { std::lock_guard<std::mutex> dl(g_dat_mu); for (auto& kv : g_dat_failed) if (now_s() < kv.second) net = true; }   // a DAT is down right now
        if (unresolved && !j.game.nx2 && (sys == "wiiu" || sys == "switch") && !gametdb::cached_index(sys)) net = true;   // the GameTDB index is not there yet
        *transient = net;
    }
    return ok;
}

void worker_main() {
    t_may_download = true;   // this thread may fetch a GameTDB database when a Wii U / Switch title has to be mapped (once; the UI thread never does)
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> l(g_mu);
            while (!g_quit && g_jobs.empty()) {
                if (g_cv.wait_for(l, std::chrono::seconds(2)) == std::cv_status::timeout) {
                    l.unlock();
                    flush_if_due();   // idle: write what the 2 s debounce held back
                    l.lock();
                }
            }
            if (g_quit) return;
            j = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        Done d;
        d.id = j.id;
        std::string err, how;
        bool transient = false;
        d.ok = run_job(j, &d.px, &err, &transient, &how, nullptr, &d.name);
        d.transient = !d.ok && transient;
        d.err = err;
        std::lock_guard<std::mutex> l(g_mu);
        g_done.push_back(std::move(d));
    }
}

void setup_cache(const std::string& cache_dir) {
    g_cache = cache_dir;
    if (!g_cache.empty() && g_cache.back() != '\\' && g_cache.back() != '/') g_cache += '\\';
    std::error_code ec;
    fs::create_directories(P(g_cache), ec);
    load_maps();
    gametdb::set_dir(g_cache + "tdb/");
    g_inited = true;
}

std::string slot_id(const std::string& key, Kind k) { return key + "|" + std::to_string((int)k); }

SDL_Texture* lookup_or_queue(Job&& j, int* w, int* h) {
    if (!g_enabled.load() || !g_renderer || !g_inited) return nullptr;
    auto it = g_slots.find(j.id);
    if (it != g_slots.end()) {
        if (it->second.st == State::Ready) {
            if (w) *w = it->second.w;
            if (h) *h = it->second.h;
            g_lru.touch(it->first);
            return it->second.tex;
        }
        if (it->second.st == State::Failed && it->second.retry_at && now_s() >= it->second.retry_at) {   // library-6: a transient failure is asked again
            it->second.st = State::Pending;
            it->second.retry_at = 0;
            { std::lock_guard<std::mutex> l(g_mu); g_jobs.push_back(std::move(j)); }
            g_cv.notify_one();
        }
        return nullptr;
    }
    g_slots[j.id] = Slot{};
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_jobs.push_back(std::move(j));
    }
    g_cv.notify_one();
    return nullptr;
}

} // namespace

/* ---------- public API ---------- */

void init(SDL_Renderer* renderer, const std::string& cache_dir) {
    if (g_worker.joinable()) return;
    g_renderer = renderer;
    setup_cache(cache_dir);
    g_quit = false;
    g_worker = std::thread(worker_main);
}

void shutdown() {
    if (g_worker.joinable()) {
        {
            std::lock_guard<std::mutex> l(g_mu);
            g_quit = true;
        }
        g_cv.notify_all();
        g_worker.join();
    }
    for (auto& kv : g_slots)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    g_slots.clear();
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_jobs.clear();
        g_done.clear();
    }
    flush_maps();
    g_renderer = nullptr;
}

void pump() {
    if (!g_renderer) return;
    g_lru.tick();
    for (auto& id : g_lru.victims(kMaxCoverTextures)) {   // before any draw this frame: safe to destroy; re-fetched from the disk cache if needed again
        auto it = g_slots.find(id);
        if (it != g_slots.end()) { if (it->second.tex) SDL_DestroyTexture(it->second.tex); g_slots.erase(it); }
        g_lru.remove(id);
    }
    for (int n = 0; n < kUploadsPerFrame; ++n) {
        Done d;
        {
            std::lock_guard<std::mutex> l(g_mu);
            if (g_done.empty()) return;
            d = std::move(g_done.front());
            g_done.pop_front();
        }
        Slot& s = g_slots[d.id];
        s.err = d.err;
        if (!d.ok) {
            s.st = State::Failed;
            if (d.transient) {   // retried later with a growing pause, instead of staying blank for the whole session
                long long wait = std::min(kArtRetryMaxSeconds, kArtRetryMinSeconds << std::min(s.fails, 5));
                ++s.fails;
                s.retry_at = now_s() + wait;
            }
            continue;
        }
        SDL_Texture* t = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, d.px.w, d.px.h);
        if (!t) { s.st = State::Failed; continue; }
        SDL_UpdateTexture(t, nullptr, d.px.rgba.data(), d.px.w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
        s.tex = t; s.w = d.px.w; s.h = d.px.h; s.st = State::Ready; s.name = d.name; g_lru.touch(d.id);
        s.avg = hubtheme::average_saturated(d.px.rgba.data(), d.px.w, d.px.h, 4);
        s.has_blur = blur8(d.px.rgba.data(), d.px.w, d.px.h, (unsigned char*)s.blur);   // false = no visible pixel: no blur
    }
}

bool blur8(const unsigned char* rgba, int w, int h, unsigned char out[8 * 8 * 4]) {
    double tot[3] = {0, 0, 0}, totw = 0;
    for (int cy = 0; cy < 8; ++cy)
        for (int cx = 0; cx < 8; ++cx) {
            const int x0 = cx * w / 8, x1 = std::max(x0 + 1, (cx + 1) * w / 8), y0 = cy * h / 8, y1 = std::max(y0 + 1, (cy + 1) * h / 8);
            const int sx = std::max(1, (x1 - x0) / 16), sy = std::max(1, (y1 - y0) / 16);
            double r = 0, g = 0, b = 0, wt = 0;
            for (int y = y0; y < y1 && y < h; y += sy)
                for (int x = x0; x < x1 && x < w; x += sx) {
                    const unsigned char* p = rgba + ((size_t)y * w + x) * 4;
                    const double a = p[3] / 255.0;
                    r += p[0] * a; g += p[1] * a; b += p[2] * a; wt += a;
                }
            unsigned char* o = out + (cy * 8 + cx) * 4;
            if (wt > 0.0) { o[0] = (unsigned char)(r / wt + 0.5); o[1] = (unsigned char)(g / wt + 0.5); o[2] = (unsigned char)(b / wt + 0.5); tot[0] += r; tot[1] += g; tot[2] += b; totw += wt; }
            else { o[0] = o[1] = o[2] = 0; }
            o[3] = wt > 0.0 ? 255 : 0;
        }
    if (totw <= 0.0) return false;
    for (int i = 0; i < 64; ++i) {   // transparent cells take the picture's mean colour, so no hole shows
        unsigned char* o = out + i * 4;
        if (o[3] == 0) { if (totw > 0.0) { o[0] = (unsigned char)(tot[0] / totw + 0.5); o[1] = (unsigned char)(tot[1] / totw + 0.5); o[2] = (unsigned char)(tot[2] / totw + 0.5); } o[3] = 255; }
    }
    return true;
}

const uint32_t* blur_colors(SDL_Texture* tex) {
    if (!tex) return nullptr;
    for (auto& kv : g_slots) if (kv.second.tex == tex) return kv.second.has_blur ? kv.second.blur : nullptr;
    return nullptr;
}

uint32_t average_color(SDL_Texture* tex) {
    if (!tex) return 0;
    for (auto& kv : g_slots) if (kv.second.tex == tex) return kv.second.avg;
    return 0;
}

void fullcover_split(int w, int h, float front_aspect, int* back_x1, int* front_x0) {
    if (w <= 0 || h <= 0) { if (back_x1) *back_x1 = 0; if (front_x0) *front_x0 = 0; return; }
    float face = (float)h * (front_aspect > 0.f ? front_aspect : 0.71f);
    float spine = (float)w - 2.f * face;
    if (!(spine >= 0.02f * (float)w && spine <= 0.14f * (float)w)) { spine = 0.05f * (float)w; face = ((float)w - spine) * 0.5f; }
    const int b = (int)(face + 0.5f), f = (int)((float)w - face + 0.5f);
    if (back_x1) *back_x1 = b;
    if (front_x0) *front_x0 = std::max(b, f);
}

SDL_Texture* get(const hub::GameEntry& game, Kind kind, int* w, int* h) {
    if (!g_enabled.load()) return nullptr;
    if (kind == Kind::FullCover && !gametdb::has_fullcover(hub::system_short(game.system))) return nullptr;   // only GameCube / Wii U / DS / 3DS have a scan
    Job j;
    j.id = slot_id(game.key, kind);
    j.kind = kind;
    /* cheap path first: avoid copying the entry once the slot exists */
    auto it = g_slots.find(j.id);
    if (it != g_slots.end()) {
        if (it->second.st == State::Ready) {
            if (w) *w = it->second.w;
            if (h) *h = it->second.h;
            g_lru.touch(it->first);
            return it->second.tex;
        }
        if (!(it->second.st == State::Failed && it->second.retry_at && now_s() >= it->second.retry_at)) return nullptr;
    }
    j.game = game;
    return lookup_or_queue(std::move(j), w, h);
}

SDL_Texture* get_by_name(const std::string& system_short, const std::string& nointro_name, Kind kind, int* w, int* h) {
    if (!g_enabled.load() || nointro_name.empty() || kind == Kind::FullCover) return nullptr;
    Job j;
    j.id = slot_id("name:" + system_short + ":" + nointro_name, kind);
    j.by_name = true;
    j.system = system_short;
    j.name = nointro_name;
    j.kind = kind;
    return lookup_or_queue(std::move(j), w, h);
}

namespace {
/* The stand-in entry for a ghost: resolved exactly like an owned copy (catalog name, serial / crc from the identity key, DAT title, fuzzy). */
hub::GameEntry ghost_entry(const std::string& system_short, const std::string& key, const std::string& catalog_id,
                           const std::string& title, const std::string& hint) {
    hub::GameEntry g;
    const std::string sys = hm::system_of_key(system_short, key);   // a PS2 disc a friend filed under GameCube is looked up as PS2
    g.system = hub::system_from_short(sys);
    g.key = key.empty() ? "ghost:" + system_short + ":" + title : key;
    g.title = title;
    g.catalog_id = catalog_id;
    g.serial = hm::serial_from_key(sys, key);
    g.path = hint.empty() ? "ghost" : "ghost|" + hint;
    return g;
}
}

std::string game_slot_id(const std::string& key, Kind kind) { return slot_id(key, kind); }
std::string ghost_slot_id(const std::string& system_short, const std::string& key, const std::string& title, Kind kind) {
    return slot_id("ghost:" + system_short + ":" + key + ":" + title, kind);
}

SDL_Texture* get_ghost(const std::string& system_short, const std::string& key, const std::string& catalog_id,
                       const std::string& title, Kind kind, int* w, int* h, const std::string& hint) {
    if (!g_enabled.load()) return nullptr;
    if (kind == Kind::FullCover && !gametdb::has_fullcover(hm::system_of_key(system_short, key))) return nullptr;
    Job j;
    j.id = ghost_slot_id(system_short, key, title, kind);
    j.kind = kind;
    auto it = g_slots.find(j.id);
    if (it == g_slots.end() || it->second.st == State::Failed) j.game = ghost_entry(system_short, key, catalog_id, title, hint);
    return lookup_or_queue(std::move(j), w, h);
}

SlotInfo slot_info(const std::string& id) {
    SlotInfo r;
    auto it = g_slots.find(id);
    if (it == g_slots.end()) return r;
    const Slot& s = it->second;
    r.state = s.st == State::Ready ? SlotState::Ready : s.st == State::Failed ? SlotState::Failed : SlotState::Pending;
    r.w = s.w; r.h = s.h; r.name = s.name; r.err = s.err;
    r.transient = s.st == State::Failed && s.retry_at != 0;
    return r;
}

std::string catalog_id_for(const std::string& system_short, const std::string& title) {
    const hub::GameInfo* gi = catalog_by_title(system_short, title);
    return gi ? gi->id : std::string();
}

std::string cached_name(const std::string& key) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_names.find(key);
    return it == g_names.end() ? std::string() : it->second;
}

bool enabled() { return g_enabled.load(); }
void set_enabled(bool on) { g_enabled.store(on); }

/* ---------- headless / test entry points ---------- */

namespace detail {

void init_headless(const std::string& cache_dir) { setup_cache(cache_dir); }

bool resolve_name(const hub::GameEntry& game, std::string* name, std::string* how) {
    return resolve_name_impl(game, name, how);
}

bool resolve_ghost(const std::string& system_short, const std::string& key, const std::string& catalog_id,
                   const std::string& title, const std::string& hint, std::string* name, std::string* how) {
    return resolve_name_impl(ghost_entry(system_short, key, catalog_id, title, hint), name, how);
}

bool fetch_ghost(const std::string& system_short, const std::string& key, const std::string& catalog_id, const std::string& title,
                 const std::string& hint, Kind kind, int* w, int* h, std::string* err, std::string* name) {
    if (kind == Kind::FullCover && !gametdb::has_fullcover(hm::system_of_key(system_short, key))) { if (err) *err = "no full cover for this system"; return false; }
    DownloadScope allow;
    Job j;
    j.kind = kind;
    j.game = ghost_entry(system_short, key, catalog_id, title, hint);
    Pixels px;
    std::string e, how;
    bool transient = false;
    bool ok = run_job(j, &px, &e, &transient, &how, nullptr, name);
    if (ok) { if (w) *w = px.w; if (h) *h = px.h; }
    else if (err) *err = e + (transient ? " (transient)" : "");
    return ok;
}

void flush() { flush_maps(); }
bool image_header_ok(const std::string& bytes, int* w, int* h) {
    int a = 0, b = 0;
    bool ok = header_ok(bytes, &a, &b);
    if (w) *w = a;
    if (h) *h = b;
    return ok;
}

bool fetch_game(const hub::GameEntry& game, Kind kind, int* w, int* h, std::string* err, std::string* how, std::string* png_path, std::string* name) {
    DownloadScope allow;   // the headless tool runs on one thread and may download the databases it needs
    Job j;
    j.kind = kind;
    j.game = game;
    Pixels px;
    std::string e;
    bool transient = false;
    bool ok = run_job(j, &px, &e, &transient, how, png_path, name);
    if (ok) { if (w) *w = px.w; if (h) *h = px.h; }
    else if (err) *err = e + (transient ? " (transient)" : "");
    return ok;
}

bool fetch_decode(const std::string& system_short, const std::string& nointro_name, Kind kind, int* w, int* h,
                  std::string* err, std::string* png_path) {
    Pixels px;
    std::string e;
    bool ok = load_image(system_short, nointro_name, kind, &px, &e, png_path);
    if (ok) { if (w) *w = px.w; if (h) *h = px.h; }
    else if (err) *err = e;
    return ok;
}

} // namespace detail
} // namespace hubart
