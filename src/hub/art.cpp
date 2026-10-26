#include "winutil.h"
#include "art.h"
#include "art_match.h"
#include "wiiu_meta.h"

#include "library.h"
#include "games.h"

#include <SDL.h>

#include <windows.h>
#include <winhttp.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"

#include <atomic>
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
const int kUploadsPerFrame = 8;
const long long kMissRetrySeconds = 7LL * 24 * 3600;
const long long kDatRefreshSeconds = 60LL * 24 * 3600;

struct Pixels {
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
};

enum class State { Pending, Ready, Failed };

struct Slot {
    State st = State::Pending;
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
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
std::map<std::string, std::string> g_names;      // game key -> No-Intro name
std::map<std::string, std::string> g_misses;     // id -> unix time
bool g_names_dirty = false, g_misses_dirty = false;
std::thread g_worker;
bool g_quit = false;

std::mutex g_dat_mu;
std::map<std::string, std::vector<hm::DatRec>> g_dats;
std::map<std::string, bool> g_dat_failed;        // session-level

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

const char* kind_name(Kind k) { return k == Kind::Boxart ? "boxart" : k == Kind::Title ? "title" : "snap"; }

std::string safe_file_name(const std::string& n) {
    std::string r = hm::thumb_name(n);
    for (auto& c : r) if (c == '"') c = '_';
    return r;
}

/* ---------- persisted maps ---------- */

void load_maps() {
    std::string t;
    std::map<std::string, std::string> m;
    if (read_file(P(g_cache + "names.json"), &t) && hm::map_from_json(t, &m)) g_names = m;
    if (read_file(P(g_cache + "misses.json"), &t) && hm::map_from_json(t, &m)) g_misses = m;
}
void flush_maps_locked() {
    if (g_names_dirty && write_file_atomic(P(g_cache + "names.json"), hm::map_to_json(g_names))) g_names_dirty = false;
    if (g_misses_dirty && write_file_atomic(P(g_cache + "misses.json"), hm::map_to_json(g_misses))) g_misses_dirty = false;
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
    std::lock_guard<std::mutex> l(g_mu);
    g_misses[id] = std::to_string(now_s());
    g_misses_dirty = true;
    flush_maps_locked();
}

/* ---------- WinHTTP ---------- */

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

    HINTERNET ses = WinHttpOpen(L"RetroHub", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 5000, 5000, 10000, 20000);
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
    WinHttpCloseHandle(ses);
    if (!ok) *status = 0;
    return ok;
}

/* ---------- DAT cache ---------- */

const std::vector<hm::DatRec>* get_dat(const std::string& system) {
    std::lock_guard<std::mutex> l(g_dat_mu);
    auto it = g_dats.find(system);
    if (it != g_dats.end()) return &it->second;
    if (g_dat_failed[system]) return nullptr;
    std::string url = hm::dat_url(system);
    if (url.empty()) return nullptr;
    fs::path p = P(g_cache + "dat/" + system + ".dat");
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
            g_dat_failed[system] = true;
            return nullptr;
        }
    }
    auto recs = system == "wiiu" ? hm::parse_wiiu_tree(text) : hm::parse_dat(text);
    if (recs.empty()) { g_dat_failed[system] = true; return nullptr; }
    return &(g_dats[system] = std::move(recs));
}

/* ---------- name resolution ---------- */

std::string tail_after_last(const std::string& s, char c) {
    size_t p = s.rfind(c);
    return p == std::string::npos ? "" : s.substr(p + 1);
}

bool resolve_name_impl(const hub::GameEntry& g, std::string* name, std::string* how) {
    std::string sys = hub::system_short(g.system);
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_names.find(g.key);
        if (it != g_names.end() && !it->second.empty()) {
            *name = it->second;
            if (how) *how = "cache";
            return true;
        }
    }
    const std::string miss_id = "name:" + g.key;

    std::string found, h;
    bool skip_dat = false;
    if (!g.catalog_id.empty()) {
        if (const hub::GameInfo* gi = hub::find_game(g.catalog_id)) {
            if (!gi->nointro.empty()) { found = gi->nointro; h = "catalog"; }
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
            q.fuzzy = g.path == "ghost";   // friend-game ghost entry (see get_ghost)
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
    if (found.empty()) {
        record_miss(miss_id);
        return false;
    }
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_names[g.key] = found;
        g_names_dirty = true;
        flush_maps_locked();
    }
    *name = found;
    if (how) *how = h;
    return true;
}

/* ---------- image load / downscale ---------- */

bool decode_png(const std::string& bytes, Pixels* out) {
    int w = 0, h = 0, n = 0;
    unsigned char* d = stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!d) return false;
    if (w <= 0 || h <= 0) { stbi_image_free(d); return false; }
    if (h <= kMaxTexHeight) {
        out->rgba.assign(d, d + (size_t)w * h * 4);
        out->w = w; out->h = h;
    } else {
        int nh = kMaxTexHeight;
        int nw = (int)((double)w * nh / h + 0.5);
        if (nw < 1) nw = 1;
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
    std::string miss_id = "art:" + system + ":" + kind_name(kind) + ":" + name;
    fs::path file = P(g_cache + system + "/" + kind_name(kind) + "/" + safe_file_name(name) + ".png");
    if (png_path) *png_path = file.u8string();
    std::string bytes;
    if (read_file(file, &bytes) && decode_png(bytes, px)) return true;
    if (is_missed(miss_id)) { *err = "known miss"; return false; }
    std::vector<std::string> urls = hm::thumb_urls(system, (int)kind, name);
    if (urls.empty()) { *err = "unsupported system"; return false; }
    int st = 0;
    for (const std::string& url : urls) {   /* 404 on one repo: try the next (gbc -> gb) */
        if (!http_get(url, &bytes, &st)) { *err = "network error"; return false; }
        if (st != 404) break;
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
    if (!decode_png(bytes, px)) { *err = "bad image"; return false; }
    write_file_atomic(file, bytes);
    return true;
}

/* ---------- worker ---------- */

void worker_main() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> l(g_mu);
            g_cv.wait(l, [] { return g_quit || !g_jobs.empty(); });
            if (g_quit) return;
            j = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        Done d;
        d.id = j.id;
        std::string sys = j.by_name ? j.system : hub::system_short(j.game.system);
        std::string name = j.name, err;
        bool ok = true;
        if (!j.by_name) ok = resolve_name_impl(j.game, &name, nullptr);
        if (ok) ok = load_image(sys, name, j.kind, &d.px, &err, nullptr);
        d.ok = ok;
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
            return it->second.tex;
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
        flush_maps_locked();
    }
    g_renderer = nullptr;
}

void pump() {
    if (!g_renderer) return;
    for (int n = 0; n < kUploadsPerFrame; ++n) {
        Done d;
        {
            std::lock_guard<std::mutex> l(g_mu);
            if (g_done.empty()) return;
            d = std::move(g_done.front());
            g_done.pop_front();
        }
        Slot& s = g_slots[d.id];
        if (!d.ok) { s.st = State::Failed; continue; }
        SDL_Texture* t = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, d.px.w, d.px.h);
        if (!t) { s.st = State::Failed; continue; }
        SDL_UpdateTexture(t, nullptr, d.px.rgba.data(), d.px.w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
        s.tex = t; s.w = d.px.w; s.h = d.px.h; s.st = State::Ready;
    }
}

SDL_Texture* get(const hub::GameEntry& game, Kind kind, int* w, int* h) {
    if (!g_enabled.load()) return nullptr;
    Job j;
    j.id = slot_id(game.key, kind);
    j.kind = kind;
    /* cheap path first: avoid copying the entry once the slot exists */
    auto it = g_slots.find(j.id);
    if (it != g_slots.end()) {
        if (it->second.st == State::Ready) {
            if (w) *w = it->second.w;
            if (h) *h = it->second.h;
            return it->second.tex;
        }
        return nullptr;
    }
    j.game = game;
    return lookup_or_queue(std::move(j), w, h);
}

SDL_Texture* get_by_name(const std::string& system_short, const std::string& nointro_name, Kind kind, int* w, int* h) {
    if (!g_enabled.load() || nointro_name.empty()) return nullptr;
    Job j;
    j.id = slot_id("name:" + system_short + ":" + nointro_name, kind);
    j.by_name = true;
    j.system = system_short;
    j.name = nointro_name;
    j.kind = kind;
    return lookup_or_queue(std::move(j), w, h);
}

SDL_Texture* get_ghost(const std::string& system_short, const std::string& key, const std::string& catalog_id,
                       const std::string& title, Kind kind, int* w, int* h) {
    if (!g_enabled.load()) return nullptr;
    Job j;
    j.id = slot_id("ghost:" + system_short + ":" + key + ":" + title, kind);
    j.kind = kind;
    auto it = g_slots.find(j.id);
    if (it == g_slots.end()) {
        /* Resolve exactly like an owned copy: catalog name, serial/crc from the identity key, DAT title, then fuzzy. */
        j.game.system = hub::system_from_short(system_short);
        j.game.key = key.empty() ? "ghost:" + system_short + ":" + title : key;
        j.game.title = title;
        j.game.catalog_id = catalog_id;
        j.game.serial = hm::serial_from_key(system_short, key);
        j.game.path = "ghost";
    }
    return lookup_or_queue(std::move(j), w, h);
}

bool enabled() { return g_enabled.load(); }
void set_enabled(bool on) { g_enabled.store(on); }

/* ---------- headless / test entry points ---------- */

namespace detail {

void init_headless(const std::string& cache_dir) { setup_cache(cache_dir); }

bool resolve_name(const hub::GameEntry& game, std::string* name, std::string* how) {
    return resolve_name_impl(game, name, how);
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
