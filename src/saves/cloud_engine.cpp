/* Cloud saves v2 client engine (docs/CLOUD_SAVES_V2.md sections 4, 5, 7). Public API: cloud_engine.h / cloud_internal.h.
 *
 * Threads: any thread may call the blocking API (job thread by convention). One worker thread drains the persistent queue
 * (pushes after exit, retries, background pulls); one heartbeat thread keeps the lease of the running game. All shared state
 * is behind E.mu, which is never held across file or network I/O. Per game, operations are serialised by a timed mutex so a
 * pre-launch never waits more than a few seconds behind a push of the same game.
 * Local play never depends on any server answer. The account secret is never logged or put in an event. */
#include "cloud_engine.h"
#include "cloud_internal.h"
#include "cloud_http.h"
#include "savestore.h"
#include "games.h"
#include "friends.h"
#include "identity_link.h"
#include "json.h"
#include "winutil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using str = std::string;

namespace hub::saves {

static std::atomic<bool> g_backup_shots{false};
void set_backup_screenshots(bool on) { g_backup_shots = on; }
namespace {

const int64_t kMaxBlob = 16ll * 1024 * 1024;
const int64_t kMaxPlain = 64ll * 1024 * 1024;
const int kPlanMax = 200;

int64_t now_s() { return (int64_t)time(nullptr); }
int64_t mono_ms() { return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

str url_enc(const str& s) {
    str o; char b[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

/* same rule as the vault's folder names (savestore.cpp sanitize_component) */
str sanitize(const str& in) {
    str s = in;
    for (auto& c : s) if ((unsigned char)c < 32 || strchr("<>:\"/\\|?*", c)) c = '_';
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    if (s.empty() || s == "." || s == "..") s = "_";
    return s;
}
str wire_key(const GameId& g) { return sanitize(g.system) + "/" + sanitize(g.key); }
GameId id_of_wire(const str& gkey) {
    size_t p = gkey.find('/');
    return p == str::npos ? GameId{gkey, ""} : GameId{gkey.substr(0, p), gkey.substr(p + 1)};
}
fs::path game_root(const GameId& g) {   // does not create anything
    str v = vault_dir();
    return fs::u8path(v) / fs::u8path(sanitize(g.system)) / fs::u8path(sanitize(g.key));
}
str norm_rel(str p) { for (auto& c : p) if (c == '\\') c = '/'; return p; }
fs::path abs_of(const GameId& g, const str& rel) { return game_root(g) / fs::u8path(rel); }

bool read_all(const fs::path& p, str* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream s;
    s << f.rdbuf();
    *out = s.str();
    return true;
}
bool write_atomic(const fs::path& dst, const str& data) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::path tmp = dst;
    tmp += ".hubtmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) return false;
    }
    fs::rename(tmp, dst, ec);
    if (ec) { fs::remove(dst, ec); fs::rename(tmp, dst, ec); }
    return !ec;
}
str stamp() {
    SYSTEMTIME t; GetLocalTime(&t);
    char b[32]; snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
str computer_name() {
    wchar_t w[MAX_COMPUTERNAME_LENGTH + 2] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(w, &n)) return "This PC";
    return winutil::to_utf8(std::wstring(w, n));
}
str random_hex(int n) {
    static const char* h = "0123456789abcdef";
    std::vector<unsigned char> b((size_t)n);
    if (BCryptGenRandom(nullptr, b.data(), (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        for (int i = 0; i < n; i++) b[(size_t)i] = (unsigned char)(GetTickCount64() >> (i % 8));
    str o;
    for (int i = 0; i < n; i++) o += h[b[(size_t)i] & 15];
    return o;
}

/* ------------------------------------------------------------------ network outcome */
struct Outcome {
    enum K { Ok, Offline, Throttled, Full, Auth, Other } k = Ok;
    int status = 0;
    str code;
    int64_t retry_after = 0;
    bool ok() const { return k == Ok; }
};
Outcome oc_of(const HttpReply& r) {
    Outcome o;
    o.status = r.status;
    ServerError se = parse_error(r.body);
    o.code = se.code;
    o.retry_after = se.retry_after;
    if (o.retry_after == 0) {
        auto it = r.headers.find("retry-after");
        if (it != r.headers.end()) o.retry_after = atoll(it->second.c_str());
    }
    if (r.status >= 200 && r.status < 300) o.k = Outcome::Ok;
    else if (r.status == 0) o.k = Outcome::Offline;
    else if (r.status == 401) o.k = Outcome::Auth;
    else if (r.status == 507) o.k = Outcome::Full;
    else if (r.status == 429 || r.status >= 500) o.k = Outcome::Throttled;
    else o.k = Outcome::Other;
    return o;
}

/* ------------------------------------------------------------------ engine state */
struct LFile { str abs; int64_t size = 0, mtime = 0; str sha; };
struct CacheEnt { int64_t size = 0, mtime = 0; str sha; };

struct Engine {
    std::mutex mu;
    std::condition_variable cv;
    EngineConfig ecfg;
    str base_url, data_dir, uid, secret, device_id, device_name;
    bool id_override = false;
    int64_t id_stamp = 0;
    bool sync_states = true;   // "Back up save states to the cloud": false = state/shot files neither push nor pull
    bool started = false, stop = false, paused = false, startup_done = false, startup_wanted = true;
    bool online_known = false, was_offline = false;
    CloudQueue queue;
    BaseStore base;
    str base_path;
    bool base_loaded = false;
    std::map<str, GameCloudInfo> info;
    CloudAccountInfo acct;
    std::deque<CloudEvent> events;
    std::map<str, SyncMode> running;           // wire key -> mode of the running session
    std::set<str> holds_lease;
    std::map<str, std::shared_ptr<std::timed_mutex>> gmu;
    std::map<str, CacheEnt> hash_cache;
    std::mutex cache_mu;
    std::thread worker, beat;
    std::atomic<bool> beat_stop{false};
    std::mutex beat_mu;
    std::condition_variable beat_cv;
    std::set<str> beat_games;
    int init_total = 0, init_done = 0;
    bool init_large_queued = false;
    int64_t pull_cool_until = 0;
    bool reminder_pending = false, ack = false, reminded = false;
    std::set<str> registered;                  // uid8 values this device registered for
    bool busy = false;                         // worker is running a job
    int64_t cap_warn_day = 0;
    str migrate_mirror;                        // old v1 mirror folder to delete after the first full pass
} E;

str uid8() { return E.uid.substr(0, 8); }

std::shared_ptr<std::timed_mutex> game_lock(const str& gkey) {
    std::lock_guard<std::mutex> lk(E.mu);
    auto& p = E.gmu[gkey];
    if (!p) p = std::make_shared<std::timed_mutex>();
    return p;
}

str data_dir_s() {
    str d = E.data_dir.empty() ? hub::data_dir() : E.data_dir;
    if (!d.empty() && d.back() != '\\' && d.back() != '/') d += '\\';
    return d;
}
str state_path() { return data_dir_s() + "cloud-state.json"; }
str queue_path() { return data_dir_s() + "cloud-queue.json"; }

void push_event_locked(CloudEvent::Kind k, const GameId& g, const str& text, bool good) {
    CloudEvent e;
    e.kind = k; e.game = g; e.text = text; e.good = good;
    E.events.push_back(e);
    while (E.events.size() > 64) E.events.pop_front();
}
void push_event(CloudEvent::Kind k, const GameId& g, const str& text, bool good = true) {
    std::lock_guard<std::mutex> lk(E.mu);
    push_event_locked(k, g, text, good);
}

void log_line(const str& s) {   // cloud_saves.log in the data dir; never contains secrets
    std::ofstream f(data_dir_s() + "cloud_saves.log", std::ios::app);
    if (!f) return;
    std::time_t t = std::time(nullptr);
    char ts[32] = {};
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    f << ts << "  v2: " << s << "\n";
}

/* ---- device state file ---- */
void save_state_file() {
    str reg, o;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        for (auto& r : E.registered) reg += (reg.empty() ? "" : ",") + json::quote(r);
        o = "{\"device_id\":" + json::quote(E.device_id) + ",\"device_name\":" + json::quote(E.device_name) + ",\"registered\":[" + reg +
            "],\"ack\":" + (E.ack ? "true" : "false") + ",\"reminded\":" + (E.reminded ? "true" : "false") + "}";
    }
    write_atomic(fs::u8path(state_path()), o);
}
void load_state_file() {
    str t;
    json::Value j;
    bool have = read_all(fs::u8path(state_path()), &t) && json::parse(t, &j) && j.type == json::Value::Object;
    std::lock_guard<std::mutex> lk(E.mu);
    if (have) {
        E.device_id = j.str("device_id");
        E.device_name = j.str("device_name");
        E.ack = j.boolean("ack", false);
        E.reminded = j.boolean("reminded", false);
        if (const json::Value* r = j.get("registered")) if (r->type == json::Value::Array) for (auto& v : r->a) E.registered.insert(v.s);
    }
    bool fresh = E.device_id.size() < 8;
    if (fresh) E.device_id = "d-" + random_hex(10);
    if (!E.ecfg.device_name.empty()) E.device_name = E.ecfg.device_name;
    if (E.device_name.empty()) E.device_name = computer_name();
    (void)fresh;
}

/* ---- base store ---- */
void base_save_locked() { if (!E.base_path.empty()) E.base.save(E.base_path); }
void base_save() { std::lock_guard<std::mutex> lk(E.mu); base_save_locked(); }
void queue_save_locked() { E.queue.save(); }

BGame& bgame(const str& gkey) { return E.base.games[gkey]; }   // caller holds E.mu

/* ---- identity ---- */
void switch_account_locked(const str& uid, const str& secret) {
    E.uid = uid;
    E.secret = secret;
    str v = vault_dir();
    E.base_path = v + ".sync\\cloud-" + uid.substr(0, 8) + ".json";
    bool had = E.base.load(E.base_path);
    E.base_loaded = true;
    if (!had) {   // new account on this PC: restore mode, cloud first
        E.base = BaseStore();
        E.base.restore = true;
        E.base.device = E.device_id;
        E.startup_done = false;
        E.startup_wanted = true;
        E.init_large_queued = false;
        E.init_total = E.init_done = 0;
    }
    fs::path vp = fs::u8path(v);
    while (!vp.empty() && (vp.filename().empty() || vp.filename() == ".")) vp = vp.parent_path();
    str old = (vp.parent_path() / "cloud_mirror" / uid.substr(0, 8)).u8string();
    std::error_code ec;
    E.migrate_mirror = fs::exists(fs::u8path(old), ec) ? old : str();
    E.info.clear();
    E.acct = CloudAccountInfo();
    E.acct.device_name = E.device_name;
    E.acct.needs_attention = false;
    if (!had) E.base.save(E.base_path);
}

bool refresh_identity() {
    str uid, secret;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        if (E.id_override) return !E.uid.empty();
    }
    str ip = data_dir_s() + "identity.json";
    int64_t st = file_mtime_unix(ip);
    bool need;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        need = E.uid.empty() || st != E.id_stamp;
    }
    if (!need) return true;
    hubnet::Identity id = hubnet::identity_load_or_create(data_dir_s(), vault_dir());
    if (!id.valid()) {
        std::lock_guard<std::mutex> lk(E.mu);
        E.acct.needs_attention = true;
        E.acct.attention = "identity-missing";
        return false;
    }
    std::lock_guard<std::mutex> lk(E.mu);
    E.id_stamp = file_mtime_unix(ip);
    if (id.user_id != E.uid) switch_account_locked(id.user_id, id.secret);
    else E.secret = id.secret;
    return true;
}

CloudConfig cloud_cfg() {
    CloudConfig c;
    std::lock_guard<std::mutex> lk(E.mu);
    c.base_url = E.base_url;
    c.user_id = E.uid;
    c.secret = E.secret;
    return c;
}

struct Api {
    std::unique_ptr<CloudHttp> http;
    str dev;
    Api(int connect_ms, int io_ms) {
        CloudConfig c = cloud_cfg();
        http.reset(new CloudHttp(c, connect_ms, io_ms));
        std::lock_guard<std::mutex> lk(E.mu);
        dev = E.device_id;
    }
    HttpReply req(const str& method, const str& pq, const str& body = str(), std::vector<str> hdr = {},
                  const std::function<bool(const char*, size_t)>& sink = nullptr) {
        if (!http->ok()) { HttpReply r; r.error = "bad server url"; return r; }
        hdr.push_back("X-RH-Device: " + dev);
        return http->call(method, pq, hdr, body, sink);
    }
};

/* ---- info helpers ---- */
GameCloudInfo& info_of_locked(const GameId& g) {
    auto it = E.info.find(wire_key(g));
    if (it == E.info.end()) {
        GameCloudInfo gi;
        gi.id = GameId{sanitize(g.system), sanitize(g.key)};
        it = E.info.emplace(wire_key(g), gi).first;
    }
    return it->second;
}
void set_state(const GameId& g, CloudState s, LocalOnlyReason lo = LocalOnlyReason::None, const str& msg = str()) {
    std::lock_guard<std::mutex> lk(E.mu);
    GameCloudInfo& gi = info_of_locked(g);
    gi.state = s;
    gi.local_only = lo;
    gi.message = msg;
    if (s != CloudState::Conflict) gi.conflicts.clear();
    if (s != CloudState::PlayingElsewhere) gi.playing_on.clear();
}
int queued_count_locked() {   // games waiting, not counting internal bookkeeping items
    std::set<str> g;
    for (auto& q : E.queue.items()) if (q.gkey != "*") g.insert(q.gkey);
    return (int)g.size();
}
void note_online(bool online) {
    std::lock_guard<std::mutex> lk(E.mu);
    bool was = E.acct.online;
    E.acct.online = online;
    E.acct.known = E.acct.known || online;
    if (online) {
        E.online_known = true;
        if (E.was_offline) { E.was_offline = false; push_event_locked(CloudEvent::BackOnline, GameId(), "Back online: your saves are syncing again.", true); }
    } else if (!E.was_offline && (was || E.online_known)) {
        E.was_offline = true;
        push_event_locked(CloudEvent::OfflineStarted, GameId(), "Offline: your saves will sync when you are back.", false);
    } else if (!E.online_known) {
        E.was_offline = true;
        E.acct.known = true;    // started offline: the UI says "Offline" instead of "Checking your cloud saves..." forever
    }
}
void apply_account(const WAccount& a) {
    std::lock_guard<std::mutex> lk(E.mu);
    E.acct.known = true;
    E.acct.online = true;
    E.acct.used = a.used; E.acct.cap = a.cap; E.acct.prot_used = a.prot_used; E.acct.prot_cap = a.prot_cap;
    E.acct.pool = a.pool; E.acct.devices = a.devices;
    E.acct.paused_by_server = a.paused;
    E.acct.needs_attention = false;
    E.acct.attention.clear();
    E.acct.device_name = E.device_name;
    E.acct.queued_games = queued_count_locked();
}
void apply_outcome_account(const Outcome& o) {
    if (o.k == Outcome::Offline) note_online(false);
    else if (o.k == Outcome::Auth) {
        std::lock_guard<std::mutex> lk(E.mu);
        E.acct.needs_attention = true;
        E.acct.attention = "account";
        E.acct.online = true;
    } else if (o.k == Outcome::Throttled || o.k == Outcome::Full) {
        std::lock_guard<std::mutex> lk(E.mu);
        E.acct.online = true;
        E.acct.paused_by_server = o.code.find("cloud-saves-") == 0 || o.code == "account-daily-limit" || o.code == "rate-limited";
    } else if (o.k == Outcome::Ok) note_online(true);
}

/* ------------------------------------------------------------------ local scan */
std::map<str, LFile> scan_game(const GameId& g) {
    std::map<str, LFile> out;
    fs::path root = game_root(g);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        str name = it->path().filename().u8string();
        if (name.size() > 7 && name.compare(name.size() - 7, 7, ".hubtmp") == 0) continue;
        str rel = norm_rel(fs::relative(it->path(), root, ec).u8string());
        if (rel.empty() || rel[0] == '.' || rel.rfind("_conflicts/", 0) == 0 || rel.rfind("_identity", 0) == 0) continue;
        LFile lf;
        lf.abs = it->path().u8string();
        lf.size = (int64_t)it->file_size(ec);
        lf.mtime = file_mtime_unix(lf.abs);
        if (lf.size > kMaxPlain) { out[rel] = lf; continue; }   // sha left empty: too large, never hashed
        {
            std::lock_guard<std::mutex> lk(E.cache_mu);
            auto c = E.hash_cache.find(lf.abs);
            if (c != E.hash_cache.end() && c->second.size == lf.size && c->second.mtime == lf.mtime) lf.sha = c->second.sha;
        }
        if (lf.sha.empty()) {
            lf.sha = sha256_hex_file(lf.abs);
            if (!lf.sha.empty()) {
                std::lock_guard<std::mutex> lk(E.cache_mu);
                E.hash_cache[lf.abs] = CacheEnt{lf.size, lf.mtime, lf.sha};
            }
        }
        out[rel] = lf;
    }
    return out;
}

/* ------------------------------------------------------------------ evaluation */
struct Item {
    str path;
    FileClass cls = FileClass::Battery;
    Act act = Act::Nothing;
    Side local, base, cloud;
    WVer cv;
    bool has_cv = false, evicted = false, lo_marked = false;
    str cloud_dev_name;
    int base_ver = 0;
};

bool sync_states_on() { std::lock_guard<std::mutex> lk(E.mu); return E.sync_states; }

std::vector<Item> evaluate(const GameId& g, const std::map<str, LFile>& locals, const WGame* cg) {
    std::vector<Item> items;
    std::set<str> paths;
    for (auto& kv : locals) paths.insert(kv.first);
    if (cg) for (auto& f : cg->files) paths.insert(f.path);
    BGame bg;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        auto it = E.base.games.find(wire_key(g));
        if (it != E.base.games.end()) bg = it->second;
    }
    for (const str& p : paths) {
        const bool is_shot = p.compare(0, 6, "shots/") == 0;   // screenshots (runner_ctl.h): large-pool "shot" class, only when "Back up screenshots" is on
        if (is_shot && !g_backup_shots.load()) continue;
        Item it;
        it.path = p;
        auto lf = locals.find(p);
        if (lf != locals.end()) {
            it.local.present = !lf->second.sha.empty() || lf->second.size > kMaxPlain;
            it.local.sha = lf->second.sha; it.local.size = lf->second.size; it.local.mtime = lf->second.mtime;
        }
        auto bf = bg.files.find(p);
        if (bf != bg.files.end()) {
            it.base.present = bf->second.ver > 0 || !bf->second.sha.empty();
            it.base.sha = bf->second.sha; it.base.size = bf->second.size; it.base.mtime = bf->second.mtime;
            it.base_ver = bf->second.ver;
            it.lo_marked = !bf->second.lo.empty();
        }
        const WFile* wf = cg ? cg->find(p) : nullptr;
        if (wf) {
            const WVer* cur = wf->current();
            it.evicted = wf->evicted();
            if (cur && !it.evicted) {
                it.cv = *cur; it.has_cv = true;
                it.cloud.present = true; it.cloud.sha = cur->sha; it.cloud.size = cur->size; it.cloud.mtime = cur->mtime;
                it.cloud_dev_name = cur->dev_name;
            }
        }
        int64_t sz = it.local.present ? it.local.size : (it.has_cv ? it.cv.size : 0);
        if (is_shot) it.cls = FileClass::Shot; else
        it.cls = it.has_cv && !it.cv.cls.empty() ? class_from_name(it.cv.cls, classify_path(p, sz)) : classify_path(p, sz);
        if (it.local.present && it.local.size > kMaxPlain) { it.act = Act::Nothing; items.push_back(it); continue; }
        if ((it.cls == FileClass::State || it.cls == FileClass::Shot) && !sync_states_on()) { it.act = Act::Nothing; items.push_back(it); continue; }
        it.act = three_way(it.local, it.base, it.cloud, it.evicted, it.cls);
        /* a file the server refused earlier (cap / too large) stays local only while unchanged */
        if (it.act == Act::Push && it.lo_marked && it.base.present && it.local.sha == it.base.sha) it.act = Act::Nothing;
        items.push_back(it);
    }
    return items;
}

/* ------------------------------------------------------------------ file transfer */
struct PullResult { bool ok = false; Outcome oc; str err; };

PullResult pull_file(Api& api, const GameId& g, const str& gkey, const str& path, const WVer& v, int64_t deadline_ms,
                     const fs::path* dest_override = nullptr) {
    PullResult pr;
    fs::path dst = dest_override ? *dest_override : abs_of(g, path);
    fs::path dl = dst; dl += ".dl.hubtmp";
    fs::path tmp = dst; tmp += ".hubtmp";
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    for (int attempt = 0; attempt < 2; attempt++) {
        str partial;
        if (fs::exists(dl, ec)) {
            read_all(dl, &partial);
            if ((int64_t)partial.size() > v.stored) partial.clear();
        }
        if ((int64_t)partial.size() < v.stored) {
            bool ranged = !partial.empty();
            str chunk;
            str pq = "/saves/v2/blob?game=" + url_enc(gkey) + "&path=" + url_enc(path) + "&ver=" + std::to_string(v.ver);
            std::vector<str> hdr;
            if (ranged) hdr.push_back("Range: bytes=" + std::to_string(partial.size()) + "-");
            HttpReply r = api.req("GET", pq, str(), hdr, [&](const char* d, size_t n) {
                if (deadline_ms && mono_ms() > deadline_ms) return false;
                chunk.append(d, n);
                return true;
            });
            if (r.status == 200 || r.status == 206) {
                if (r.status == 206 && ranged) partial += chunk; else partial = chunk;
            } else if (r.status == 0 && !chunk.empty()) {
                auto ps = r.headers.find("x-partial-status");
                if (!ranged) partial = chunk;                               // from byte 0
                else if (ps != r.headers.end() && ps->second == "206") partial += chunk;
                else if (ps == r.headers.end()) partial += chunk;           // unknown: sha check below catches a wrong guess
                write_atomic(dl, partial);
            }
            if (r.status != 200 && r.status != 206) {
                pr.oc = oc_of(r);
                pr.err = r.status == 0 ? (r.error.empty() ? "network" : r.error) : "HTTP " + std::to_string(r.status);
                if (r.status == 416) { fs::remove(dl, ec); continue; }
                return pr;
            }
            auto hs = r.headers.find("x-rh-sha");
            if (hs != r.headers.end() && !hs->second.empty() && (int64_t)partial.size() == v.stored && sha256_hex_of(partial) != hs->second) {
                fs::remove(dl, ec); pr.err = "download corrupt"; continue;
            }
        }
        if ((int64_t)partial.size() != v.stored) { write_atomic(dl, partial); pr.err = "incomplete"; pr.oc.k = Outcome::Offline; return pr; }
        str plain;
        if (v.enc == "deflate") {
            if (!inflate_raw(partial, &plain, (size_t)kMaxPlain)) { fs::remove(dl, ec); pr.err = "bad compressed data"; continue; }
        } else plain = partial;
        if ((int64_t)plain.size() != v.size || sha256_hex_of(plain) != v.sha) { fs::remove(dl, ec); pr.err = "checksum mismatch"; continue; }
        if (!write_atomic(dst, plain)) { pr.err = "cannot write " + path; pr.oc.k = Outcome::Other; return pr; }
        fs::remove(dl, ec);
        fs::remove(tmp, ec);
        set_file_mtime_unix(dst.u8string(), v.mtime);
        {
            std::lock_guard<std::mutex> lk(E.cache_mu);
            E.hash_cache[dst.u8string()] = CacheEnt{v.size, file_mtime_unix(dst.u8string()), v.sha};
        }
        pr.ok = true;
        pr.oc = Outcome();
        return pr;
    }
    pr.oc.k = Outcome::Other;
    return pr;
}

void backup_local(const GameId& g, const str& path, const str& tag) {
    fs::path src = abs_of(g, path);
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) return;
    fs::path dst = fs::u8path(vault_dir()) / "_conflicts" / fs::u8path(sanitize(g.system)) / fs::u8path(sanitize(g.key)) /
                   fs::u8path(path + "." + tag + "-" + stamp());
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
}

void base_set(const str& gkey, const str& path, int ver, const str& sha, int64_t size, int64_t mtime, const str& lo = str()) {
    std::lock_guard<std::mutex> lk(E.mu);
    BFile& f = bgame(gkey).files[path];
    f.ver = ver; f.sha = sha; f.size = size; f.mtime = mtime; f.lo = lo;
}

ConflictItem conflict_item(const Item& it) {
    ConflictItem c;
    c.path = it.path;
    c.cls = it.cls;
    c.local.present = it.local.present; c.local.sha = it.local.sha; c.local.size = it.local.size; c.local.mtime = it.local.mtime;
    c.cloud.present = it.cloud.present; c.cloud.sha = it.cloud.sha; c.cloud.size = it.cloud.size; c.cloud.mtime = it.cloud.mtime;
    c.cloud.device_name = it.cloud_dev_name;
    return c;
}

/* ------------------------------------------------------------------ sync of one game */
struct SyncOpts {
    bool pull = true, push = true;
    bool prot_only = false, large_only = false;
    bool pull_absent_only_large = false;    // background pulls after launch never overwrite an existing local file
    bool auto_newest = false;               // conflicts resolve to the newest silently (read-only sessions)
    bool queue_large_pulls = false;         // pre-launch: large pulls are left for a background job
    int64_t deadline_ms = 0;
};
struct SyncOut {
    Outcome oc;
    int pulled = 0, pushed = 0;
    std::vector<ConflictItem> conflicts;
    int64_t pending_bytes = 0;
    bool large_pending = false;
    bool local_only_added = false;
};

bool class_ok(const SyncOpts& o, FileClass c, int64_t size) {
    bool prot = is_protected(c, size);
    if (o.prot_only && !prot) return false;
    if (o.large_only && prot) return false;
    return true;
}

struct Prep { str path; FileClass cls; LFile lf; str stored; str enc = "none"; str stored_sha; int base = 0; };

/* plan -> blobs -> commit. Returns false when the sequence stopped on a network/server problem (out.oc set). */
void do_push(Api& api, const GameId& g, const str& gkey, std::vector<Item> todo, const SyncOpts& opts, SyncOut& out, bool* new_cap_loss) {
    std::stable_sort(todo.begin(), todo.end(), [](const Item& a, const Item& b) {
        bool pa = is_protected(a.cls, a.local.size), pb = is_protected(b.cls, b.local.size);
        if (pa != pb) return pa;
        return a.local.mtime > b.local.mtime;
    });
    int64_t played = now_s();
    for (int round = 0; round < 3 && !todo.empty(); round++) {
        std::vector<Item> next;
        for (size_t off = 0; off < todo.size(); off += kPlanMax) {
            std::vector<Prep> preps;
            std::vector<PlanFile> pf;
            for (size_t i = off; i < todo.size() && i < off + kPlanMax; i++) {
                const Item& it = todo[i];
                Prep p;
                p.path = it.path; p.cls = it.cls; p.base = it.base_ver;
                LFile cur;
                cur.abs = abs_of(g, it.path).u8string();
                if (it.local.size > kMaxBlob * 4) {
                    std::lock_guard<std::mutex> lk(E.mu);
                    BFile& bf = bgame(gkey).files[it.path];
                    bf.sha = it.local.sha; bf.size = it.local.size; bf.mtime = it.local.mtime; bf.lo = "too-large";
                    out.local_only_added = true;
                    continue;
                }
                str plain;
                if (!read_all(fs::u8path(cur.abs), &plain)) continue;
                cur.size = (int64_t)plain.size();
                cur.sha = sha256_hex_of(plain);
                cur.mtime = file_mtime_unix(cur.abs);
                p.lf = cur;
                p.stored = plain;
                if (plain.size() > 4096) {
                    str z;
                    if (deflate_raw(plain, &z) && (double)z.size() <= (double)plain.size() * 0.8) { p.stored = std::move(z); p.enc = "deflate"; }
                }
                if ((int64_t)p.stored.size() > kMaxBlob) {
                    std::lock_guard<std::mutex> lk(E.mu);
                    BFile& bf = bgame(gkey).files[it.path];
                    bf.sha = cur.sha; bf.size = cur.size; bf.mtime = cur.mtime; bf.lo = "too-large";
                    out.local_only_added = true;
                    continue;
                }
                p.stored_sha = p.enc == "none" ? cur.sha : sha256_hex_of(p.stored);
                PlanFile f;
                f.path = p.path; f.sha = cur.sha; f.size = cur.size; f.stored = (int64_t)p.stored.size(); f.enc = p.enc;
                f.mtime = cur.mtime; f.cls = class_name(p.cls); f.base = p.base;
                pf.push_back(f);
                preps.push_back(std::move(p));
            }
            if (preps.empty()) continue;
            {
                std::lock_guard<std::mutex> lk(E.mu);
                auto bi = E.base.games.find(gkey);
                if (bi != E.base.games.end() && bi->second.played) played = bi->second.played;
            }
            HttpReply pr = api.req("POST", "/saves/v2/plan", build_plan_body(gkey, played, "rw", pf));
            Outcome po = oc_of(pr);
            if (!po.ok()) { out.oc = po; return; }
            PlanReply plan;
            str perr;
            if (!parse_plan(pr.body, &plan, &perr)) { out.oc.k = Outcome::Other; out.oc.code = "bad-plan"; return; }
            std::map<str, PlanAnswer> ans;
            for (auto& a : plan.files) ans[a.path] = a;
            int to_commit = 0;
            std::set<str> committing;
            for (auto& p : preps) {
                auto ai = ans.find(p.path);
                if (ai == ans.end()) continue;
                const PlanAnswer& a = ai->second;
                if (a.action == "skip") {
                    str lo = a.reason == "too-large" ? "too-large" : "cap";
                    std::lock_guard<std::mutex> lk(E.mu);
                    BFile& bf = bgame(gkey).files[p.path];
                    bf.sha = p.lf.sha; bf.size = p.lf.size; bf.mtime = p.lf.mtime; bf.lo = lo;
                    out.local_only_added = true;
                    if (new_cap_loss) *new_cap_loss = true;
                } else if (a.action == "same") {
                    base_set(gkey, p.path, a.has_current ? a.current.ver : p.base, p.lf.sha, p.lf.size, p.lf.mtime);
                } else if (a.action == "conflict") {
                    Item it;
                    for (auto& t : todo) if (t.path == p.path) it = t;
                    if (a.has_current) {
                        it.cv = a.current; it.has_cv = true;
                        it.cloud.present = true; it.cloud.sha = a.current.sha; it.cloud.size = a.current.size; it.cloud.mtime = a.current.mtime;
                        it.cloud_dev_name = a.current.dev_name;
                        it.base_ver = a.current.ver;
                    }
                    it.local.present = true; it.local.sha = p.lf.sha; it.local.size = p.lf.size; it.local.mtime = p.lf.mtime;
                    if (it.cls == FileClass::Battery || it.cls == FileClass::Card || !a.has_current) out.conflicts.push_back(conflict_item(it));
                    else if (a.current.mtime > p.lf.mtime) {    // newest wins: cloud
                        PullResult r = pull_file(api, g, gkey, p.path, a.current, opts.deadline_ms);
                        if (r.ok) { backup_local(g, p.path, "pc"); base_set(gkey, p.path, a.current.ver, a.current.sha, a.current.size, a.current.mtime); out.pulled++; }
                        else if (!r.oc.ok()) { out.oc = r.oc; return; }
                    } else {                                     // newest wins: this PC, retry against the new base
                        base_set(gkey, p.path, a.current.ver, a.current.sha, a.current.size, a.current.mtime);
                        Item re = it; re.base_ver = a.current.ver; re.act = Act::Push; next.push_back(re);
                    }
                } else if (a.action == "upload" || a.action == "have") {
                    to_commit++;
                    committing.insert(p.path);
                    if (a.action == "upload") {
                        str pq = "/saves/v2/blob?sha=" + p.lf.sha + "&ticket=" + url_enc(plan.ticket);
                        std::vector<str> hdr = {"X-RH-Sha: " + p.stored_sha, "X-RH-Plain-Sha: " + p.lf.sha, "X-RH-Mtime: " + std::to_string(p.lf.mtime)};
                        HttpReply br = api.req("PUT", pq, p.stored, hdr);
                        Outcome bo = oc_of(br);
                        if (!bo.ok()) { out.oc = bo; return; }
                    }
                }
            }
            if (to_commit == 0) continue;
            HttpReply cr = api.req("POST", "/saves/v2/commit", "{\"ticket\":" + json::quote(plan.ticket) + "}");
            Outcome co = oc_of(cr);
            if (co.code == "ticket-expired" || cr.status == 409) { out.oc = co; out.oc.k = Outcome::Throttled; return; }   // plan again on the retry
            if (!co.ok()) { out.oc = co; return; }
            CommitReply com;
            str cerr;
            if (!parse_commit(cr.body, &com, &cerr)) { out.oc.k = Outcome::Other; out.oc.code = "bad-commit"; return; }
            for (auto& r : com.results) {
                const Prep* pp = nullptr;
                for (auto& p : preps) if (p.path == r.path) pp = &p;
                if (!pp) continue;
                if (r.conflict) {
                    Item it;
                    for (auto& t : todo) if (t.path == r.path) it = t;
                    if (r.has_current) {
                        it.cv = r.current; it.has_cv = true;
                        it.cloud.present = true; it.cloud.sha = r.current.sha; it.cloud.size = r.current.size; it.cloud.mtime = r.current.mtime;
                        it.cloud_dev_name = r.current.dev_name;
                    }
                    it.local.present = true; it.local.sha = pp->lf.sha; it.local.size = pp->lf.size; it.local.mtime = pp->lf.mtime;
                    if ((it.cls == FileClass::Battery || it.cls == FileClass::Card) || !r.has_current) out.conflicts.push_back(conflict_item(it));
                    else if (r.current.mtime > pp->lf.mtime) {
                        PullResult pr2 = pull_file(api, g, gkey, r.path, r.current, opts.deadline_ms);
                        if (pr2.ok) { backup_local(g, r.path, "pc"); base_set(gkey, r.path, r.current.ver, r.current.sha, r.current.size, r.current.mtime); out.pulled++; }
                    } else {
                        base_set(gkey, r.path, r.current.ver, r.current.sha, r.current.size, r.current.mtime);
                        Item re = it; re.base_ver = r.current.ver; re.act = Act::Push; next.push_back(re);
                    }
                    continue;
                }
                if (r.cloud == "evicted") {
                    std::lock_guard<std::mutex> lk(E.mu);
                    BFile& bf = bgame(gkey).files[r.path];
                    bf.sha = pp->lf.sha; bf.size = pp->lf.size; bf.mtime = pp->lf.mtime; bf.lo = "cap";
                    out.local_only_added = true;
                    if (new_cap_loss) *new_cap_loss = true;
                    continue;
                }
                base_set(gkey, r.path, r.ver, pp->lf.sha, pp->lf.size, pp->lf.mtime);
                out.pushed++;
            }
            if (!com.evicted.empty()) {
                std::vector<str> names;
                {
                    std::lock_guard<std::mutex> lk(E.mu);
                    for (auto& ev : com.evicted) {
                        auto bi = E.base.games.find(ev.first);
                        if (bi == E.base.games.end()) continue;
                        BFile& bf = bi->second.files[ev.second];
                        bf.lo = "cap";
                        if (std::find(names.begin(), names.end(), ev.first) == names.end()) names.push_back(ev.first);
                    }
                    for (auto& n : names) {
                        auto ii = E.info.find(n);
                        if (ii != E.info.end()) ii->second.files_local_only++;
                    }
                }
                if (!names.empty() && new_cap_loss) *new_cap_loss = true;
                if (!names.empty()) {
                    int64_t day = now_s() / 86400;
                    bool say;
                    {
                        std::lock_guard<std::mutex> lk(E.mu);
                        say = E.base.cap_toast_day != day;
                        if (say) E.base.cap_toast_day = day;
                    }
                    if (say) {
                        str list;
                        for (size_t i = 0; i < names.size() && i < 3; i++) {
                            str n = names[i];
                            size_t sl = n.find('/');
                            list += (i ? ", " : "") + (sl == str::npos ? n : n.substr(sl + 1));
                        }
                        if (names.size() > 3) list += " and " + std::to_string(names.size() - 3) + " more";
                        push_event(CloudEvent::WentLocalOnly, g, "Cloud space is full: older save states of " + list + " stay on this PC only.", false);
                    }
                }
            }
        }
        todo = next;
        if (!out.oc.ok()) return;
    }
}

/* Core: bring one game in line. `cg` = manifest entry (null when the cloud has nothing). */
SyncOut sync_game_with(Api& api, const GameId& g, const WGame* cg, const SyncOpts& opts) {
    SyncOut out;
    str gkey = wire_key(g);
    std::map<str, LFile> locals = scan_game(g);
    std::vector<Item> items = evaluate(g, locals, cg);
    bool restore;
    bool dl;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        restore = E.base.restore;
        dl = E.base.games.count(gkey) && E.base.games[gkey].decide_later;
    }
    std::vector<Item*> pulls;
    for (auto& it : items) {
        if (it.act == Act::Pull && opts.pull && class_ok(opts, it.cls, it.cloud.size)) {
            bool prot = is_protected(it.cls, it.cloud.size);
            if (!prot && opts.queue_large_pulls) { if (!it.local.present) out.large_pending = true; out.pending_bytes += it.cloud.size; continue; }
            if (!prot && opts.pull_absent_only_large && it.local.present) continue;
            pulls.push_back(&it);
        } else if (it.act == Act::Pull) {
            out.pending_bytes += it.cloud.size;
        }
    }
    std::stable_sort(pulls.begin(), pulls.end(), [](const Item* a, const Item* b) {
        bool pa = is_protected(a->cls, a->cloud.size), pb = is_protected(b->cls, b->cloud.size);
        if (pa != pb) return pa;
        return a->cloud.mtime > b->cloud.mtime;
    });
    for (Item* it : pulls) {
        if (opts.deadline_ms && mono_ms() > opts.deadline_ms) { out.pending_bytes += it->cloud.size; out.oc.k = Outcome::Offline; out.oc.code = "deadline"; continue; }
        if (it->local.present && it->local.sha != it->base.sha) backup_local(g, it->path, "pc");   // newest-wins replaced a changed local copy
        PullResult r = pull_file(api, g, gkey, it->path, it->cv, opts.deadline_ms);
        if (r.ok) {
            base_set(gkey, it->path, it->cv.ver, it->cv.sha, it->cv.size, it->cv.mtime);
            out.pulled++;
        } else {
            out.pending_bytes += it->cloud.size;
            if (!r.oc.ok()) { out.oc = r.oc; if (r.oc.k != Outcome::Other) break; }
            log_line("pull failed " + gkey + "/" + it->path + ": " + r.err);
        }
    }
    std::vector<Item> push_items;
    for (auto& it : items) {
        if (!class_ok(opts, it.cls, std::max(it.local.size, it.cloud.size))) continue;
        switch (it.act) {
            case Act::Adopt:
                base_set(gkey, it.path, it.cv.ver, it.cloud.sha, it.cloud.size, it.local.mtime);
                break;
            case Act::LocalOnlyCap: {
                std::lock_guard<std::mutex> lk(E.mu);
                BFile& bf = bgame(gkey).files[it.path];
                bf.sha = it.local.sha; bf.size = it.local.size; bf.mtime = it.local.mtime; bf.lo = "cap";
                out.local_only_added = true;
                break;
            }
            case Act::Conflict: {
                if (opts.auto_newest) {   // read-only session: newest wins, the loser is kept
                    if (it.cloud.mtime > it.local.mtime) {
                        backup_local(g, it.path, "pc");
                        PullResult r = pull_file(api, g, gkey, it.path, it.cv, opts.deadline_ms);
                        if (r.ok) { base_set(gkey, it.path, it.cv.ver, it.cv.sha, it.cv.size, it.cv.mtime); out.pulled++; }
                    }
                } else out.conflicts.push_back(conflict_item(it));
                break;
            }
            case Act::Push: push_items.push_back(it); break;
            default: break;
        }
    }
    for (auto& it : push_items) out.pending_bytes += it.local.size;
    if (opts.push && !restore && !dl && out.oc.ok() && !push_items.empty()) {
        bool cap_loss = false;
        do_push(api, g, gkey, push_items, opts, out, &cap_loss);
        if (out.oc.ok()) { out.pending_bytes = 0; }
    }
    {
        std::lock_guard<std::mutex> lk(E.mu);
        if (out.oc.ok() && out.conflicts.empty()) bgame(gkey).last_synced = now_s();
        base_save_locked();
    }
    return out;
}

void update_info(const GameId& g, const WGame* cg, const SyncOut& out, bool pending_queue) {
    str gkey = wire_key(g);
    std::lock_guard<std::mutex> lk(E.mu);
    GameCloudInfo& gi = info_of_locked(g);
    BGame& bg = bgame(gkey);
    int synced = 0, lo = 0;
    str lo_kind;
    for (auto& f : bg.files) {
        if (!f.second.lo.empty()) { lo++; lo_kind = f.second.lo; }
        else if (f.second.ver > 0) synced++;
    }
    gi.files_synced = synced;
    gi.files_local_only = lo;
    gi.last_synced = bg.last_synced;
    gi.pinned = bg.pinned || (cg && cg->pin);
    if (cg) {
        int64_t cb = 0;
        for (auto& f : cg->files) if (!f.evicted()) if (const WVer* c = f.current()) cb += c->stored;
        gi.cloud_bytes = cb;
    }
    gi.pending_bytes = out.pending_bytes;
    gi.conflicts = out.conflicts;
    gi.message.clear();
    gi.local_only = LocalOnlyReason::None;
    if (bg.lo_user) { gi.state = CloudState::LocalOnly; gi.local_only = LocalOnlyReason::User; gi.message = "Not uploaded (you turned cloud saves off for this game)"; return; }
    if (!out.conflicts.empty() || bg.decide_later) {
        gi.state = CloudState::Conflict;
        if (bg.decide_later) { gi.local_only = LocalOnlyReason::DecideLater; gi.message = "Saves differ between this PC and the cloud. Not uploading until you decide."; }
        else gi.message = "Saves differ between this PC and the cloud.";
        return;
    }
    if (!out.oc.ok()) {
        switch (out.oc.k) {
            case Outcome::Auth: gi.state = CloudState::NeedsAttention; gi.message = "Cloud saves need your account to be checked."; break;
            case Outcome::Full: gi.state = CloudState::LocalOnly; gi.local_only = LocalOnlyReason::OverCap; gi.message = "Cloud is full: this game stays on this PC."; break;
            case Outcome::Throttled:
                gi.state = out.oc.code.find("paused") != str::npos || out.oc.code.find("busy") != str::npos || out.oc.code.find("unavailable") != str::npos ? CloudState::Paused : CloudState::Waiting;
                gi.message = gi.state == CloudState::Paused ? "Cloud saves are paused for now." : "Will try again soon.";
                break;
            default: gi.state = CloudState::Waiting; gi.message = "Offline: will sync when you are back."; break;
        }
        return;
    }
    if (pending_queue || out.large_pending) { gi.state = CloudState::Waiting; gi.message = out.large_pending ? "Save states are still downloading." : "Waiting to sync."; return; }
    if (lo > 0 && synced == 0) {
        gi.state = CloudState::LocalOnly;
        gi.local_only = lo_kind == "too-large" ? LocalOnlyReason::TooLarge : LocalOnlyReason::OverCap;
        gi.message = lo_kind == "too-large" ? "Too large for the cloud: stays on this PC." : "Cloud space is full: stays on this PC.";
        return;
    }
    gi.state = CloudState::Synced;
    if (lo > 0) {
        gi.local_only = lo_kind == "too-large" ? LocalOnlyReason::TooLarge : LocalOnlyReason::OverCap;
        gi.message = std::to_string(lo) + " file(s) stay on this PC only (cloud full).";
    }
}

/* ------------------------------------------------------------------ manifest + device */
struct MFetch { Outcome oc; WManifest m; };
MFetch fetch_manifest(Api& api, const str& gkey, int64_t since = -1) {
    MFetch f;
    str pq = "/saves/v2/manifest";
    char sep = '?';
    if (since >= 0) { pq += sep + str("since=") + std::to_string(since); sep = '&'; }
    if (!gkey.empty()) pq += sep + str("game=") + url_enc(gkey);
    HttpReply r = api.req("GET", pq);
    f.oc = oc_of(r);
    apply_outcome_account(f.oc);
    if (!f.oc.ok()) return f;
    str err;
    if (!parse_manifest(r.body, &f.m, &err)) { f.oc.k = Outcome::Other; f.oc.code = "bad-manifest"; return f; }
    apply_account(f.m.account);
    return f;
}

Outcome ensure_device(Api& api) {
    str u8;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        u8 = E.uid.substr(0, 8);
        if (E.registered.count(u8)) return Outcome();
    }
    str name, dev;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        name = E.device_name; dev = E.device_id;
    }
    HttpReply r = api.req("POST", "/saves/v2/device", "{\"dev\":" + json::quote(dev) + ",\"name\":" + json::quote(name) + "}");
    Outcome o = oc_of(r);
    apply_outcome_account(o);
    if (o.ok()) {
        json::Value j;
        if (json::parse(r.body, &j) && j.type == json::Value::Object) {
            std::lock_guard<std::mutex> lk(E.mu);
            int64_t d = j.i64("devices", 0);
            if (d > 0) E.acct.devices = (int)d;
        }
        { std::lock_guard<std::mutex> lk(E.mu); E.registered.insert(u8); }
        save_state_file();
    } else if (r.status == 403) {
        std::lock_guard<std::mutex> lk(E.mu);
        E.acct.needs_attention = true;
        E.acct.attention = "device-limit";
    }
    return o;
}

bool enabled_now() {
    std::lock_guard<std::mutex> lk(E.mu);
    return E.started && !E.base_url.empty() && !E.paused && !E.stop;
}

void enqueue(const str& gkey, const str& reason, int64_t when = 0, bool reset = true) {
    {
        std::lock_guard<std::mutex> lk(E.mu);
        E.queue.put(gkey, reason, when ? when : now_s(), reset);
        E.queue.save();
        E.acct.queued_games = queued_count_locked();
    }
    E.cv.notify_all();
}

/* ------------------------------------------------------------------ lease */
LeaseReply lease_call(Api& api, const str& gkey, const str& mode, bool take, Outcome* oc_out) {
    LeaseReply lr;
    str body = "{\"game\":" + json::quote(gkey) + ",\"mode\":" + json::quote(mode) + (take ? ",\"take\":true" : "") + "}";
    HttpReply r = api.req("POST", "/saves/v2/lease", body);
    Outcome o = oc_of(r);
    if (oc_out) *oc_out = o;
    if (r.status == 423) { parse_lease(r.body, &lr); lr.held = true; lr.ok = false; return lr; }
    if (o.ok()) { parse_lease(r.body, &lr); if (!lr.held) lr.ok = true; }
    return lr;
}

/* ------------------------------------------------------------------ prelaunch */
enum class LeaseStep { Check, Take, Ignore };

PrelaunchResult prelaunch_core(const GameId& g, SyncMode mode, int timeout_ms, int pull_budget_ms, LeaseStep step) {
    PrelaunchResult res;
    if (!enabled_now() || !refresh_identity()) return res;
    str gkey = wire_key(g);
    bool user_local_only;
    { std::lock_guard<std::mutex> lk(E.mu); user_local_only = E.base.games.count(gkey) && E.base.games[gkey].lo_user; }
    if (user_local_only) { set_state(g, CloudState::LocalOnly, LocalOnlyReason::User, "Not uploaded (you turned cloud saves off for this game)"); return res; }
    auto gl = game_lock(gkey);
    std::unique_lock<std::timed_mutex> lk(*gl, std::defer_lock);
    if (!lk.try_lock_for(std::chrono::milliseconds(3000))) { res.offline = false; return res; }   // a push of this game is still running: play now
    int cms = std::max(500, std::min(timeout_ms / 2, 2000));
    Api api(cms, std::max(1000, timeout_ms));
    Outcome dev = ensure_device(api);
    if (!dev.ok()) log_line("prelaunch " + gkey + ": device http " + std::to_string(dev.status) + " " + dev.code);
    if (!dev.ok() && dev.k == Outcome::Offline) { res.offline = true; set_state(g, CloudState::Waiting, LocalOnlyReason::None, "Offline: will sync when you are back."); enqueue(gkey, "push"); return res; }
    MFetch mf = fetch_manifest(api, gkey);
    if (!mf.oc.ok()) {
        log_line("prelaunch " + gkey + ": manifest http " + std::to_string(mf.oc.status) + " " + mf.oc.code);
        res.offline = true;
        SyncOut o; o.oc = mf.oc;
        update_info(g, nullptr, o, false);
        if (mf.oc.k == Outcome::Offline || mf.oc.k == Outcome::Throttled) enqueue(gkey, "push");
        return res;
    }
    const WGame* cg = nullptr;
    for (auto& w : mf.m.games) if (w.gkey == gkey) cg = &w;
    /* lease (only accounts with 2+ devices; read-only sessions never take one) */
    if (mode == SyncMode::ReadWrite && mf.m.account.devices >= 2 && step != LeaseStep::Ignore) {
        Outcome lo;
        LeaseReply lr = lease_call(api, gkey, "rw", step == LeaseStep::Take, &lo);
        log_line("prelaunch " + gkey + ": lease http " + std::to_string(lo.status) + " held=" + (lr.held ? lr.name : str("no")));
        if (lr.held && step == LeaseStep::Check) {
            bool mine;
            { std::lock_guard<std::mutex> l2(E.mu); mine = lr.dev == E.device_id; }
            if (!mine) {
                res.action = PrelaunchResult::NeedLeaseDecision;
                res.held_by = lr.name;
                res.held_since = lr.since;
                std::lock_guard<std::mutex> l2(E.mu);
                GameCloudInfo& gi = info_of_locked(g);
                gi.state = CloudState::PlayingElsewhere;
                gi.playing_on = lr.name;
                gi.message = "Open on " + lr.name;
                return res;
            }
        }
        if (lr.ok || step == LeaseStep::Take) { std::lock_guard<std::mutex> l2(E.mu); E.holds_lease.insert(gkey); }
    }
    SyncOpts so;
    so.push = false;
    so.queue_large_pulls = true;
    so.auto_newest = mode == SyncMode::ReadOnly;
    so.deadline_ms = mono_ms() + pull_budget_ms;
    SyncOut out = sync_game_with(api, g, cg, so);
    res.files_pulled = out.pulled;
    res.conflicts = out.conflicts;
    if (!out.oc.ok() && out.oc.k == Outcome::Offline) res.offline = true;
    update_info(g, cg, out, false);
    if (out.large_pending && mode == SyncMode::ReadWrite) enqueue(gkey, "pull-large");
    if (!out.conflicts.empty()) {
        res.action = PrelaunchResult::NeedConflictDecision;
        push_event(CloudEvent::ConflictFound, g, "Saves for " + g.key + " differ between this PC and the cloud.", false);
    }
    if (out.pulled > 0) push_event(CloudEvent::Pulled, g, "Cloud saves for " + g.key + " were downloaded.", true);
    if (res.offline) enqueue(gkey, "push");
    return res;
}

/* ------------------------------------------------------------------ queue worker */
void note_pushed(const GameId& g, int n) {
    bool remind = false;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        if (!E.ack && !E.reminded && n > 0) { E.reminded = true; E.reminder_pending = true; remind = true; }
    }
    if (remind) save_state_file();
    if (n > 0) push_event(CloudEvent::Pushed, g, "Saves backed up to the cloud.", true);
}

/* One queue job. Returns true when the item is finished (remove), false to retry. */
bool run_job(const QItem& q, int64_t* retry_after, bool* park, bool* defer) {
    *retry_after = 0; *park = false; *defer = false;
    GameId g = id_of_wire(q.gkey);
    if (!refresh_identity()) { *park = true; return false; }
    {
        std::lock_guard<std::mutex> lk(E.mu);
        if (E.running.count(q.gkey)) { *retry_after = 15; *defer = true; return false; }
        if (E.base.restore && (q.reason == "push" || q.reason == "sync-all" || q.reason == "init-prot" || q.reason == "init-large")) { *retry_after = 20; *defer = true; return false; }
        if (E.base.games.count(q.gkey) && E.base.games[q.gkey].lo_user) return true;
    }
    auto gl = game_lock(q.gkey);
    std::unique_lock<std::timed_mutex> lk(*gl, std::defer_lock);
    if (!lk.try_lock_for(std::chrono::milliseconds(2000))) { *retry_after = 5; *defer = true; return false; }
    Api api(4000, 20000);
    Outcome dev = ensure_device(api);
    if (!dev.ok()) {
        log_line("job " + q.reason + " " + q.gkey + ": device http " + std::to_string(dev.status) + " " + dev.code);
        SyncOut o; o.oc = dev; update_info(g, nullptr, o, true);
        *retry_after = dev.retry_after;
        *park = dev.k == Outcome::Auth;
        return false;
    }
    MFetch mf = fetch_manifest(api, q.gkey);
    if (!mf.oc.ok()) {
        SyncOut o; o.oc = mf.oc; update_info(g, nullptr, o, true);
        *retry_after = mf.oc.retry_after;
        *park = mf.oc.k == Outcome::Auth;
        return mf.oc.k == Outcome::Full;
    }
    const WGame* cg = nullptr;
    for (auto& w : mf.m.games) if (w.gkey == q.gkey) cg = &w;
    SyncOpts so;
    const str& r = q.reason;
    if (r == "pull" || r == "pull-large") {
        so.push = false;
        so.prot_only = r == "pull";
        so.large_only = r == "pull-large";
        so.pull_absent_only_large = r == "pull-large";
    } else if (r == "init-prot") so.prot_only = true;
    else if (r == "init-large") so.large_only = true;
    SyncOut out = sync_game_with(api, g, cg, so);
    bool done = out.oc.ok();
    bool full = out.oc.k == Outcome::Full;
    if (!out.oc.ok()) log_line("job " + r + " " + q.gkey + ": http " + std::to_string(out.oc.status) + " " + out.oc.code);
    update_info(g, cg, out, !done && !full);
    if (!out.conflicts.empty()) push_event(CloudEvent::ConflictFound, g, "Saves for " + g.key + " differ between this PC and the cloud.", false);
    if (out.pushed > 0) note_pushed(g, out.pushed);
    if (out.pulled > 0) {
        push_event(CloudEvent::Pulled, g, "Cloud saves for " + g.key + " were downloaded.", true);
        if (r == "pull") {
            std::lock_guard<std::mutex> l2(E.mu);
            E.pull_cool_until = now_s() + std::max<int64_t>(1, (out.pulled * 64 * 1024) / (4 * 1024 * 1024 / 60));
        }
    }
    if (out.oc.k == Outcome::Auth) { *park = true; push_event(CloudEvent::NeedsAttention, g, "Cloud saves need attention: your account could not be verified.", false); }
    if (out.oc.k == Outcome::Throttled) *retry_after = out.oc.retry_after;
    if (out.oc.k == Outcome::Offline && out.oc.code == "deadline") { *retry_after = 1; }
    if (full) done = true;                                 // not retried: LocalOnly
    if (!out.conflicts.empty() && done) done = true;       // waits for the player's decision
    if (r == "pull-large" && out.large_pending) done = false;
    return done;
}

void startup_pass();

void queue_initial_backup_locked_free() {
    /* every local game: protected files first */
    std::vector<GameId> games;
    str v = vault_dir();
    std::error_code ec;
    for (auto& sd : fs::directory_iterator(fs::u8path(v), ec)) {
        if (!sd.is_directory(ec)) continue;
        str sn = sd.path().filename().u8string();
        if (sn.empty() || sn[0] == '_' || sn[0] == '.') continue;
        for (auto& gd : fs::directory_iterator(sd.path(), ec)) {
            if (!gd.is_directory(ec)) continue;
            games.push_back(GameId{sn, gd.path().filename().u8string()});
        }
    }
    {
        std::lock_guard<std::mutex> lk(E.mu);
        for (auto& g : games) {
            GameCloudInfo& gi = info_of_locked(g);
            if (gi.state == CloudState::Unknown) gi.state = CloudState::Waiting;
        }
    }
    {
        std::lock_guard<std::mutex> lk(E.mu);
        for (auto& g : games) {
            str k = wire_key(g);
            BGame& bg = E.base.games[k];
            bool any_unsynced = bg.files.empty();
            if (!any_unsynced) continue;
            E.queue.put(k, "init-prot", now_s(), false);
            E.init_total++;
        }
        E.queue.save();
        E.acct.queued_games = queued_count_locked();
    }
}

void queue_large_after_initial() {
    std::vector<std::pair<int64_t, str>> order;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        if (E.init_large_queued) return;
        E.init_large_queued = true;
        for (auto& kv : E.base.games) order.push_back({kv.second.played, kv.first});
    }
    std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::lock_guard<std::mutex> lk(E.mu);
    int64_t t = now_s();
    for (auto& o : order) E.queue.put(o.second, "init-large", t, false);
    E.queue.save();
}

/* first pass after start (and after "Sync now"): manifest delta, restore mode pull, initial backup */
void startup_pass() {
    if (!enabled_now() || !refresh_identity()) return;
    Api api(3000, 15000);
    Outcome dev = ensure_device(api);
    if (!dev.ok()) { return; }
    bool restore;
    int64_t since;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        restore = E.base.restore;
        since = E.base.rev;
    }
    MFetch mf = fetch_manifest(api, str(), restore ? -1 : since);
    if (!mf.oc.ok()) {
        if (mf.oc.k == Outcome::Auth) push_event(CloudEvent::NeedsAttention, GameId(), "Cloud saves need attention: your account could not be verified.", false);
        return;
    }
    {   // cloud game list for the UI
        std::lock_guard<std::mutex> lk(E.mu);
        for (auto& w : mf.m.games) {
            GameCloudInfo& gi = info_of_locked(id_of_wire(w.gkey));
            gi.pinned = w.pin;
            if (w.lease.present && w.lease.dev != E.device_id && w.lease.until > now_s()) { gi.state = CloudState::PlayingElsewhere; gi.playing_on = w.lease.name; }
            int64_t cb = 0;
            for (auto& f : w.files) if (!f.evicted()) if (const WVer* c = f.current()) cb += c->stored;
            gi.cloud_bytes = cb;
            if (gi.state == CloudState::Unknown) gi.state = CloudState::Waiting;
        }
    }
    std::vector<const WGame*> order;
    for (auto& w : mf.m.games) order.push_back(&w);
    std::sort(order.begin(), order.end(), [](const WGame* a, const WGame* b) { return a->played_at > b->played_at; });
    bool all_ok = true;
    if (restore) {
        /* restore mode: pull everything protected first (all games), then large items newest-played first; no push meanwhile */
        for (int phase = 0; phase < 2; phase++) {
            for (const WGame* w : order) {
                { std::lock_guard<std::mutex> lk(E.mu); if (E.stop) return; }
                GameId g = id_of_wire(w->gkey);
                auto gl = game_lock(w->gkey);
                std::unique_lock<std::timed_mutex> lk(*gl, std::defer_lock);
                if (!lk.try_lock_for(std::chrono::milliseconds(1000))) { all_ok = false; continue; }
                SyncOpts so;
                so.push = false;
                so.prot_only = phase == 0;
                so.large_only = phase == 1;
                so.pull_absent_only_large = phase == 1;
                SyncOut out = sync_game_with(api, g, w, so);
                update_info(g, w, out, false);
                if (!out.conflicts.empty()) push_event(CloudEvent::ConflictFound, g, "Saves for " + g.key + " differ between this PC and the cloud.", false);
                if (out.pulled > 0) push_event(CloudEvent::Pulled, g, "Cloud saves for " + g.key + " were restored.", true);
                if (!out.oc.ok()) { all_ok = false; if (out.oc.k != Outcome::Other) break; }
            }
        }
        if (all_ok) {
            std::lock_guard<std::mutex> lk(E.mu);
            E.base.restore = false;
            E.base.rev = mf.m.rev;
            base_save_locked();
            E.queue.wake_parked(now_s());      // pushes that waited for the pull phase may run now
        }
    } else {
        for (const WGame* w : order) {
            GameId g = id_of_wire(w->gkey);
            bool local_exists = fs::exists(game_root(g));
            bool mine;
            { std::lock_guard<std::mutex> lk(E.mu); mine = !w->device.empty() && w->device == E.device_name; }   // this PC pushed it last: nothing to fetch
            (void)mine;   // even a game this PC pushed last is checked once per start so its status becomes Synced after a restart
            std::lock_guard<std::mutex> lk(E.mu);
            if (local_exists) E.queue.put(w->gkey, "pull", now_s() + 1, false);
            else {   // only in the cloud: nothing is waiting, it comes down at the first launch here
                GameCloudInfo& gi = info_of_locked(g);
                if (gi.state == CloudState::Waiting) gi.state = CloudState::Synced;
            }
        }
        std::lock_guard<std::mutex> lk(E.mu);
        E.base.rev = mf.m.rev;
        base_save_locked();
        E.queue.save();
    }
    if (all_ok) {
        queue_initial_backup_locked_free();
        std::lock_guard<std::mutex> lk(E.mu);
        E.startup_done = true;
        if (!E.migrate_mirror.empty()) {   // v1 cloud_mirror is no longer used: delete it after the first successful pass
            std::error_code ec;
            fs::remove_all(fs::u8path(E.migrate_mirror), ec);
            fs::remove(fs::u8path(E.migrate_mirror + ".cloudstate"), ec);
            fs::remove(fs::u8path(E.migrate_mirror).parent_path() / (fs::u8path(E.migrate_mirror).filename().u8string() + ".cloudstate"), ec);
            E.migrate_mirror.clear();
        }
    }
}

/* process at most one due job (or the startup pass). Returns true when something ran. */
bool work_once() {
    bool do_start = false;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        if (E.stop || !E.started || E.paused || E.base_url.empty()) return false;
        do_start = E.startup_wanted;
        E.startup_wanted = false;
    }
    if (do_start) {
        { std::lock_guard<std::mutex> lk(E.mu); E.busy = true; }
        startup_pass();
        {
            std::lock_guard<std::mutex> lk(E.mu);
            E.busy = false;
            if (!E.startup_done) E.queue.put("*", "startup", now_s() + 60, false);
        }
        return true;
    }
    QItem q;
    int64_t now = now_s();
    {
        std::lock_guard<std::mutex> lk(E.mu);
        // background pulls are throttled to roughly 4 MB per minute
        CloudQueue tmp = E.queue;
        QItem cand;
        bool found = false;
        while (tmp.next_due(now, &cand)) {
            if ((cand.reason == "pull" || cand.reason == "pull-large") && E.pull_cool_until > now) { tmp.remove(cand.gkey, cand.reason); continue; }
            q = cand; found = true; break;
        }
        if (!found) return false;
        E.busy = true;
    }
    if (q.gkey == "*") {
        { std::lock_guard<std::mutex> lk(E.mu); E.queue.remove("*", "startup"); E.startup_wanted = true; E.busy = false; }
        return true;
    }
    int64_t retry = 0;
    bool park = false, defer = false;
    bool done = run_job(q, &retry, &park, &defer);
    {
        std::lock_guard<std::mutex> lk(E.mu);
        E.busy = false;
        if (done) {
            E.queue.remove(q.gkey, q.reason);
            if (q.reason == "init-prot") E.init_done++;
        } else if (park) {
            for (auto& it : const_cast<std::vector<QItem>&>(E.queue.items())) if (it.gkey == q.gkey && it.reason == q.reason) it.next_at = -1;
        } else if (defer) {
            E.queue.put(q.gkey, q.reason, now_s() + retry, false);   // waiting for a running game / restore: no failure counted
        } else {
            E.queue.fail(q.gkey, q.reason, now_s(), retry);
        }
        E.queue.save();
        E.acct.queued_games = queued_count_locked();
    }
    bool init_left = false;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        for (auto& it : E.queue.items()) if (it.reason == "init-prot") init_left = true;
    }
    if (!init_left && E.init_total > 0) queue_large_after_initial();
    return true;
}

void worker_main() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(E.mu);
            if (E.stop) return;
        }
        bool ran = work_once();
        std::unique_lock<std::mutex> lk(E.mu);
        if (E.stop) return;
        if (ran) continue;
        int64_t now = now_s();
        int64_t e = E.queue.earliest(now);
        auto until = e > now ? std::chrono::seconds(std::min<int64_t>(e - now, 60)) : std::chrono::seconds(30);
        E.cv.wait_for(lk, until);
    }
}

void beat_main() {
    for (;;) {
        std::set<str> games;
        {
            std::unique_lock<std::mutex> lk(E.beat_mu);
            E.beat_cv.wait_for(lk, std::chrono::milliseconds(std::max(50, E.ecfg.beat_ms)), [] { return E.beat_stop.load(); });
            if (E.beat_stop) return;
            games = E.beat_games;
        }
        for (auto& gkey : games) {
            Api api(3000, 8000);
            api.req("POST", "/saves/v2/lease/beat", "{\"game\":" + json::quote(gkey) + "}");
        }
    }
}

void stop_beat_if_idle() {
    bool idle;
    { std::lock_guard<std::mutex> lk(E.beat_mu); idle = E.beat_games.empty(); }
    if (idle && E.beat.joinable()) {
        E.beat_stop = true;
        E.beat_cv.notify_all();
        E.beat.join();
    }
}

} // namespace

/* ================================================================== public API */
void cloud_engine_configure(const EngineConfig& cfg) {
    std::lock_guard<std::mutex> lk(E.mu);
    E.ecfg = cfg;
    E.data_dir = cfg.data_dir;
}

void cloud_set_identity(const std::string& uid, const std::string& secret) {
    bool need_state;
    { std::lock_guard<std::mutex> lk(E.mu); need_state = E.device_id.empty(); }
    if (need_state) load_state_file();
    std::lock_guard<std::mutex> lk(E.mu);
    E.id_override = true;
    switch_account_locked(uid, secret);
}

void cloud_engine_start(const std::string& base_url) {
    {
        std::lock_guard<std::mutex> lk(E.mu);
        if (E.started) return;
    }
    load_state_file();
    {
        std::lock_guard<std::mutex> lk(E.mu);
        E.base_url = base_url;
        while (!E.base_url.empty() && E.base_url.back() == '/') E.base_url.pop_back();
        E.stop = false;
        E.started = true;
        E.startup_wanted = true;
        E.startup_done = false;
        E.queue.set_path(queue_path());
        E.queue.load(queue_path());
        E.queue.wake_parked(now_s(), true);        // hub start is a trigger for retries that gave up
        E.acct.device_name = E.device_name;
        E.acct.queued_games = queued_count_locked();
    }
    { bool inline_id; { std::lock_guard<std::mutex> lk(E.mu); inline_id = E.ecfg.no_threads || E.id_override; } if (inline_id) refresh_identity(); }   // identity.json is read/created on the worker thread otherwise
    {   // local games are listed right away (state Unknown until looked at)
        std::vector<GameId> games;
        std::error_code ec;
        for (auto& sd : fs::directory_iterator(fs::u8path(vault_dir()), ec)) {
            if (!sd.is_directory(ec)) continue;
            str sn = sd.path().filename().u8string();
            if (sn.empty() || sn[0] == '_' || sn[0] == '.') continue;
            for (auto& gd : fs::directory_iterator(sd.path(), ec)) if (gd.is_directory(ec)) games.push_back(GameId{sn, gd.path().filename().u8string()});
        }
        std::lock_guard<std::mutex> lk(E.mu);
        for (auto& g : games) info_of_locked(g);
    }
    bool threads;
    { std::lock_guard<std::mutex> lk(E.mu); threads = !E.ecfg.no_threads; }
    if (threads) E.worker = std::thread(worker_main);
}

void cloud_engine_stop() {
    {
        std::lock_guard<std::mutex> lk(E.mu);
        if (!E.started) return;
        E.stop = true;
    }
    E.cv.notify_all();
    if (E.worker.joinable()) E.worker.join();
    E.beat_stop = true;
    E.beat_cv.notify_all();
    if (E.beat.joinable()) E.beat.join();
    std::lock_guard<std::mutex> lk(E.mu);
    E.queue.save();
    E.started = false;
    E.stop = false;
    E.running.clear();
    E.holds_lease.clear();
    { std::lock_guard<std::mutex> l2(E.beat_mu); E.beat_games.clear(); }
    E.uid.clear(); E.secret.clear(); E.id_override = false; E.id_stamp = 0;
    E.device_id.clear(); E.device_name.clear();
    E.info.clear();
    E.events.clear();
    E.acct = CloudAccountInfo();
    E.base = BaseStore();
    E.base_loaded = false;
    E.registered.clear();
    E.init_total = E.init_done = 0;
    E.init_large_queued = false;
    E.pull_cool_until = 0;
}

void cloud_engine_pump(int max_jobs) {
    for (int i = 0; i < max_jobs; i++) if (!work_once()) break;
}

bool cloud_engine_idle() {
    std::lock_guard<std::mutex> lk(E.mu);
    if (E.busy || E.startup_wanted) return false;
    QItem q;
    return !E.queue.next_due(now_s(), &q);
}

std::string cloud_queue_dump() {
    std::lock_guard<std::mutex> lk(E.mu);
    std::string s;
    for (auto& q : E.queue.items()) s += q.gkey + ":" + q.reason + ":" + std::to_string(q.tries) + ":" + std::to_string(q.next_at) + ";";
    return s;
}

void cloud_account_changed() {
    {
        std::lock_guard<std::mutex> lk(E.mu);
        if (E.id_override) return;     // tests set the identity explicitly
        E.id_stamp = 0;                // refresh_identity() re-reads identity.json on its next run
        E.startup_wanted = true;
        E.startup_done = false;
    }
    E.cv.notify_all();
}

void cloud_wake_queue() {
    {
        std::lock_guard<std::mutex> lk(E.mu);
        E.queue.wake_parked(now_s());
        E.queue.save();
    }
    E.cv.notify_all();
}

void cloud_sync_all_now() {
    {
        std::lock_guard<std::mutex> lk(E.mu);
        if (!E.started) return;
        E.queue.wake_parked(now_s());
        E.startup_wanted = true;
        for (auto& kv : E.info) if (kv.second.state != CloudState::LocalOnly || kv.second.local_only != LocalOnlyReason::User) E.queue.put(kv.first, "sync-all", now_s(), true);
        E.queue.save();
    }
    E.cv.notify_all();
}

void cloud_set_sync_states(bool on) {
    bool changed;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        changed = E.sync_states != on;
        E.sync_states = on;
        if (changed && on) for (auto& kv : E.info) E.queue.put(kv.first, "sync-all", now_s(), true);   // catch up on the states skipped while off
    }
    E.cv.notify_all();
}

void cloud_set_paused(bool paused) {
    {
        std::lock_guard<std::mutex> lk(E.mu);
        E.paused = paused;
        for (auto& kv : E.info) if (paused) { kv.second.state = CloudState::Paused; kv.second.message = "Cloud sync is paused in Advanced settings."; }
        else if (kv.second.state == CloudState::Paused) kv.second.state = CloudState::Unknown;
    }
    E.cv.notify_all();
}

PrelaunchResult cloud_prelaunch_ex(const GameId& g, SyncMode mode, int timeout_ms, int pull_budget_ms) {
    return prelaunch_core(g, mode, timeout_ms, pull_budget_ms, LeaseStep::Check);
}
PrelaunchResult cloud_prelaunch(const GameId& g, SyncMode mode, int timeout_ms) {
    int budget;
    { std::lock_guard<std::mutex> lk(E.mu); budget = E.ecfg.pull_budget_ms > 0 ? E.ecfg.pull_budget_ms : 15000; }
    return prelaunch_core(g, mode, timeout_ms, budget, LeaseStep::Check);
}

PrelaunchResult cloud_resolve_lease(const GameId& g, LeaseChoice c) {
    if (c == LeaseChoice::Cancel) {
        set_state(g, CloudState::PlayingElsewhere, LocalOnlyReason::None, "Not started.");
        return PrelaunchResult();
    }
    int budget;
    { std::lock_guard<std::mutex> lk(E.mu); budget = E.ecfg.pull_budget_ms > 0 ? E.ecfg.pull_budget_ms : 15000; }
    return prelaunch_core(g, SyncMode::ReadWrite, 4000, budget, c == LeaseChoice::TakeOver ? LeaseStep::Take : LeaseStep::Ignore);
}

void cloud_resolve_conflict(const GameId& g, ConflictChoice c) {
    if (!enabled_now() || !refresh_identity()) return;
    str gkey = wire_key(g);
    auto gl = game_lock(gkey);
    std::lock_guard<std::timed_mutex> glk(*gl);
    if (c == ConflictChoice::DecideLater) {
        { std::lock_guard<std::mutex> lk(E.mu); bgame(gkey).decide_later = true; base_save_locked(); }
        std::lock_guard<std::mutex> lk(E.mu);
        GameCloudInfo& gi = info_of_locked(g);
        gi.state = CloudState::Conflict;
        gi.local_only = LocalOnlyReason::DecideLater;
        return;
    }
    Api api(3000, 15000);
    MFetch mf = fetch_manifest(api, gkey);
    if (!mf.oc.ok()) return;
    const WGame* cg = nullptr;
    for (auto& w : mf.m.games) if (w.gkey == gkey) cg = &w;
    std::vector<Item> items = evaluate(g, scan_game(g), cg);
    bool need_push = false;
    int resolved = 0;
    for (auto& it : items) {
        if (it.act != Act::Conflict) continue;
        ConflictChoice use = c;
        if (c == ConflictChoice::UseNewer) use = it.cloud.mtime > it.local.mtime ? ConflictChoice::UseCloud : ConflictChoice::UseLocal;
        if (use == ConflictChoice::UseCloud) {
            backup_local(g, it.path, "pc");
            PullResult r = pull_file(api, g, gkey, it.path, it.cv, mono_ms() + 15000);
            if (r.ok) { base_set(gkey, it.path, it.cv.ver, it.cv.sha, it.cv.size, it.cv.mtime); resolved++; }
        } else {   // keep this PC: the cloud copy stays as a version on the server and is kept locally in _conflicts as well
            fs::path keep = fs::u8path(vault_dir()) / "_conflicts" / fs::u8path(sanitize(g.system)) / fs::u8path(sanitize(g.key)) /
                            fs::u8path(it.path + ".cloud-" + stamp());
            pull_file(api, g, gkey, it.path, it.cv, mono_ms() + 8000, &keep);
            need_push = true;
            base_set(gkey, it.path, it.cv.ver, it.cv.sha, it.cv.size, it.cv.mtime);    // now "this PC changed, cloud same" -> push
            resolved++;
        }
    }
    (void)need_push;
    { std::lock_guard<std::mutex> lk(E.mu); bgame(gkey).decide_later = false; base_save_locked(); }
    push_event(CloudEvent::ConflictResolved, g, "Saves for " + g.key + " are sorted out.", true);
    SyncOut none;
    update_info(g, cg, none, true);
    enqueue(gkey, "push");
}

void cloud_game_started(const GameId& g, SyncMode mode) {
    str gkey = wire_key(g);
    bool beat = false;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        E.running[gkey] = mode;
        beat = mode == SyncMode::ReadWrite && E.holds_lease.count(gkey) && E.started;
    }
    if (!beat) return;
    { std::lock_guard<std::mutex> lk(E.beat_mu); E.beat_games.insert(gkey); }
    if (!E.beat.joinable()) { E.beat_stop = false; E.beat = std::thread(beat_main); }
}

void cloud_game_exited(const GameId& g) {
    str gkey = wire_key(g);
    SyncMode mode = SyncMode::ReadWrite;
    bool had_lease = false, dl = false, lo_user = false;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        auto it = E.running.find(gkey);
        if (it != E.running.end()) { mode = it->second; E.running.erase(it); }
        had_lease = E.holds_lease.erase(gkey) > 0;
        BGame& bg = bgame(gkey);
        if (mode == SyncMode::ReadWrite) bg.played = now_s();
        dl = bg.decide_later;
        lo_user = bg.lo_user;
    }
    { std::lock_guard<std::mutex> lk(E.beat_mu); E.beat_games.erase(gkey); }
    stop_beat_if_idle();
    if (!enabled_now()) return;
    if (had_lease) {
        Api api(2000, 5000);
        api.req("POST", "/saves/v2/lease/release", "{\"game\":" + json::quote(gkey) + "}");
    }
    if (mode == SyncMode::ReadOnly || lo_user) return;
    base_save();
    if (dl) { set_state(g, CloudState::Conflict, LocalOnlyReason::DecideLater, "Not uploading until you decide."); return; }
    set_state(g, CloudState::Waiting, LocalOnlyReason::None, "Waiting to sync.");
    enqueue(gkey, "push");
}

CloudAccountInfo cloud_account() {
    std::lock_guard<std::mutex> lk(E.mu);
    CloudAccountInfo a = E.acct;
    a.queued_games = queued_count_locked();
    if (E.paused) a.paused_by_server = false;
    return a;
}

std::vector<GameCloudInfo> cloud_games() {
    std::lock_guard<std::mutex> lk(E.mu);
    std::vector<GameCloudInfo> v;
    for (auto& kv : E.info) v.push_back(kv.second);
    return v;
}

bool cloud_game(const GameId& g, GameCloudInfo* out) {
    std::lock_guard<std::mutex> lk(E.mu);
    auto it = E.info.find(wire_key(g));
    if (it == E.info.end()) return false;
    if (out) *out = it->second;
    return true;
}

bool cloud_pop_event(CloudEvent* out) {
    std::lock_guard<std::mutex> lk(E.mu);
    if (E.events.empty()) return false;
    if (out) *out = E.events.front();
    E.events.pop_front();
    return true;
}

void cloud_set_pin(const GameId& g, bool pinned) {
    str gkey = wire_key(g);
    { std::lock_guard<std::mutex> lk(E.mu); bgame(gkey).pinned = pinned; info_of_locked(g).pinned = pinned; base_save_locked(); }
    if (!enabled_now()) return;
    Api api(3000, 8000);
    api.req("POST", "/saves/v2/game", "{\"game\":" + json::quote(gkey) + ",\"pin\":" + (pinned ? "true" : "false") + "}");
}

void cloud_set_local_only(const GameId& g, bool local_only) {
    str gkey = wire_key(g);
    {
        std::lock_guard<std::mutex> lk(E.mu);
        bgame(gkey).lo_user = local_only;
        GameCloudInfo& gi = info_of_locked(g);
        if (local_only) { gi.state = CloudState::LocalOnly; gi.local_only = LocalOnlyReason::User; gi.message = "Not uploaded (you turned cloud saves off for this game)"; }
        else { gi.state = CloudState::Waiting; gi.local_only = LocalOnlyReason::None; gi.message = "Waiting to sync."; }
        base_save_locked();
    }
    if (!local_only) enqueue(gkey, "sync-all");
}

void cloud_remove_game(const GameId& g) {
    str gkey = wire_key(g);
    if (enabled_now()) {
        Api api(3000, 10000);
        HttpReply r = api.req("POST", "/saves/v2/game", "{\"game\":" + json::quote(gkey) + ",\"remove\":true}");
        (void)r;
    }
    std::lock_guard<std::mutex> lk(E.mu);
    BGame& bg = bgame(gkey);
    bg.files.clear();
    bg.lo_user = true;                 // stays on this PC only, otherwise the next play would upload it again
    GameCloudInfo& gi = info_of_locked(g);
    gi.state = CloudState::LocalOnly; gi.local_only = LocalOnlyReason::User; gi.cloud_bytes = 0;
    gi.message = "Removed from the cloud; kept on this PC.";
    base_save_locked();
}

std::vector<std::pair<std::string, std::vector<VersionInfo>>> cloud_versions(const GameId& g) {
    std::vector<std::pair<std::string, std::vector<VersionInfo>>> out;
    if (!enabled_now() || !refresh_identity()) return out;
    Api api(3000, 10000);
    MFetch mf = fetch_manifest(api, wire_key(g));
    if (!mf.oc.ok()) return out;
    for (auto& w : mf.m.games) {
        if (w.gkey != wire_key(g)) continue;
        for (auto& f : w.files) {
            std::vector<VersionInfo> vs;
            for (auto& v : f.vers) {
                VersionInfo vi;
                vi.ver = v.ver; vi.size = v.size; vi.mtime = v.mtime; vi.ts = v.ts; vi.device_name = v.dev_name;
                vs.push_back(vi);
            }
            out.push_back({f.path, vs});
        }
    }
    return out;
}

bool cloud_restore_version(const GameId& g, const std::string& path, int ver, std::string* err) {
    auto fail = [&](const str& m) { if (err) *err = m; return false; };
    if (!enabled_now() || !refresh_identity()) return fail("Cloud saves are not available right now.");
    str gkey = wire_key(g);
    auto gl = game_lock(gkey);
    std::lock_guard<std::timed_mutex> glk(*gl);
    Api api(3000, 15000);
    MFetch mf = fetch_manifest(api, gkey);
    if (!mf.oc.ok()) return fail("Could not reach the cloud.");
    const WGame* cg = nullptr;
    for (auto& w : mf.m.games) if (w.gkey == gkey) cg = &w;
    const WFile* wf = cg ? cg->find(path) : nullptr;
    if (!wf) return fail("That file is not in the cloud.");
    HttpReply r = api.req("POST", "/saves/v2/restore",
        "{\"game\":" + json::quote(gkey) + ",\"path\":" + json::quote(path) + ",\"ver\":" + std::to_string(ver) + ",\"base\":" + std::to_string(wf->cur) + "}");
    Outcome o = oc_of(r);
    if (!o.ok()) return fail(r.status == 412 ? "The cloud copy changed meanwhile. Try again." : "The cloud could not restore that version.");
    MFetch m2 = fetch_manifest(api, gkey);
    if (!m2.oc.ok()) return fail("Restored in the cloud, but could not download it yet.");
    const WGame* g2 = nullptr;
    for (auto& w : m2.m.games) if (w.gkey == gkey) g2 = &w;
    const WFile* f2 = g2 ? g2->find(path) : nullptr;
    const WVer* cur = f2 ? f2->current() : nullptr;
    if (!cur) return fail("Restored in the cloud, but could not download it yet.");
    backup_local(g, path, "pc");
    PullResult pr = pull_file(api, g, gkey, path, *cur, mono_ms() + 15000);
    if (!pr.ok) return fail("Restored in the cloud, but the download failed: " + pr.err);
    base_set(gkey, path, cur->ver, cur->sha, cur->size, cur->mtime);
    base_save();
    push_event(CloudEvent::Restored, g, "Restored an earlier version of the save for " + g.key + ".", true);
    return true;
}

/* The account code (XXXX-XXXX-XXXX) is the ONE add / recover mechanism: the Settings > Account screen and this engine share
 * identity_link (server: /link/keep + /link/redeem). The text is kept in account_code.txt next to identity.json. */
std::string cloud_recovery_code() {
    std::ifstream f(data_dir_s() + "account_code.txt");
    str c;
    if (f && std::getline(f, c)) { while (!c.empty() && (c.back() == '\r' || c.back() == ' ')) c.pop_back(); }
    return hubnet::link_normalize_code(c).size() == 12 ? c : str();
}
bool cloud_create_account_code(std::string* code, std::string* err) {
    hubnet::Identity id;
    if (!hubnet::identity_read_file(data_dir_s() + "identity.json", &id)) { if (err) *err = "There is no account on this PC yet."; return false; }
    str base, e2, c;
    { std::lock_guard<std::mutex> lk(E.mu); base = E.base_url; }
    if (!hubnet::link_keep_code(base, id, &c, &e2)) { if (err) *err = e2; return false; }
    std::ofstream f(data_dir_s() + "account_code.txt", std::ios::trunc);
    if (f) f << c << "\n";
    if (code) *code = c;
    return true;
}
/* One "add / recover this account" path: recovery codes and link codes both end here. The identity files are replaced (the
 * old account is backed up by hubnet), the engine switches accounts and enters restore mode: cloud wins for every game this
 * PC has no save for, and a differing local save becomes a conflict at the game's next launch. */
bool cloud_adopt_account(const std::string& uid, const std::string& secret, std::string* err) {
    hubnet::Identity id;
    id.user_id = uid; id.secret = secret; id.created = now_s();
    if (!id.valid()) { if (err) *err = "That account is not valid."; return false; }
    if (!hubnet::identity_adopt(data_dir_s(), vault_dir(), id)) { if (err) *err = "Could not store the account on this PC."; return false; }
    {
        std::lock_guard<std::mutex> lk(E.mu);
        E.id_stamp = 0;
        if (E.started || E.base_loaded) switch_account_locked(uid, secret);
    }
    refresh_identity();
    cloud_wake_queue();
    E.cv.notify_all();
    return true;
}
bool cloud_adopt_recovery_code(const std::string& code, std::string* err) {
    str base;
    { std::lock_guard<std::mutex> lk(E.mu); base = E.base_url; }
    hubnet::Identity id;
    str e2;
    if (!hubnet::link_redeem_code(base, code, &id, &e2)) { if (err) *err = e2; return false; }
    std::remove((data_dir_s() + "account_code.txt").c_str());   // the saved code belonged to the previous account
    return cloud_adopt_account(id.user_id, id.secret, err);
}

void cloud_set_device_name(const std::string& name) {
    if (name.empty()) return;
    {
        std::lock_guard<std::mutex> lk(E.mu);
        E.device_name = name;
        E.acct.device_name = name;
        E.registered.clear();     // re-register (rename) on the next call
    }
    save_state_file();
    cloud_wake_queue();
}
std::string cloud_device_name() { std::lock_guard<std::mutex> lk(E.mu); return E.device_name.empty() ? computer_name() : E.device_name; }
bool cloud_recovery_acknowledged() { std::lock_guard<std::mutex> lk(E.mu); return E.ack; }
void cloud_recovery_set_acknowledged(bool v) { { std::lock_guard<std::mutex> lk(E.mu); E.ack = v; } save_state_file(); }
bool cloud_take_recovery_reminder() {
    std::lock_guard<std::mutex> lk(E.mu);
    bool r = E.reminder_pending && !E.ack;
    E.reminder_pending = false;
    return r;
}
void cloud_backup_progress(int* done, int* total) {
    std::lock_guard<std::mutex> lk(E.mu);
    if (done) *done = E.init_done;
    if (total) *total = E.init_total;
}

} // namespace hub::saves
