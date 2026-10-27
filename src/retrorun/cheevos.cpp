#include "cheevos.h"

#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>

#include "osd.h"
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#include "rc_libretro.h"

namespace cheevos {

unsigned console_for_system(const std::string& s) {
    if (s == "nes") return RC_CONSOLE_NINTENDO;
    if (s == "snes") return RC_CONSOLE_SUPER_NINTENDO;
    if (s == "gb") return RC_CONSOLE_GAMEBOY;
    if (s == "gbc") return RC_CONSOLE_GAMEBOY_COLOR;
    if (s == "gba") return RC_CONSOLE_GAMEBOY_ADVANCE;
    if (s == "nds") return RC_CONSOLE_NINTENDO_DS;
    if (s == "ps1") return RC_CONSOLE_PLAYSTATION;
    return 0;
}

namespace {
struct Reply {
    rc_client_server_callback_t cb;
    void* data;
    std::string body;
    int status;
};
long long wall() { return (long long)std::time(nullptr); }
}  // namespace

struct Session::Impl {
    Params p;
    Host h;
    rc_client_t* client = nullptr;
    rc_libretro_memory_regions_t regions{};
    bool regions_ok = false, started = false, ready = false;
    GameCache gc;
    std::vector<Recent> recent;
    long long last_block_msg = 0;

    std::mutex mu;
    std::condition_variable cv;
    struct Job { std::string url, ct, post; rc_client_server_callback_t cb; void* data; };
    std::deque<Job> queue;
    std::deque<Reply> done;
    std::thread worker;
    bool quit = false;

    void log(const std::string& m) { if (h.log) h.log("cheevos: " + m); }
    long long now() const { return p.now ? p.now() : wall(); }

    void worker_main() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> l(mu);
                cv.wait(l, [&] { return quit || !queue.empty(); });
                if (quit) return;
                j = std::move(queue.front());
                queue.pop_front();
            }
            Reply r{j.cb, j.data, "", 0};
            std::string body;
            int st = 0;
            if (!h.http || !h.http(j.url, j.ct, j.post, &body, &st)) { body.clear(); st = 0; }
            r.body = std::move(body);
            r.status = st;
            std::lock_guard<std::mutex> l(mu);
            done.push_back(std::move(r));
        }
    }
    void deliver(const Reply& r) {
        rc_api_server_response_t sr{};
        sr.body = r.body.c_str();
        sr.body_length = r.body.size();
        sr.http_status_code = r.status ? r.status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        if (!r.status) sr.body = nullptr, sr.body_length = 0;
        r.cb(&sr, r.data);
    }
    void drain() {
        for (;;) {
            Reply r;
            {
                std::lock_guard<std::mutex> l(mu);
                if (done.empty()) return;
                r = std::move(done.front());
                done.pop_front();
            }
            deliver(r);
        }
    }

    void rebuild_cache() {
        GameCache g;
        g.game_key = p.game_key;
        const rc_client_game_t* gi = rc_client_get_game_info(client);
        if (gi) { g.title = gi->title ? gi->title : ""; g.game_id = gi->id; }
        if (g.title.empty()) g.title = p.game_title_hint;
        rc_client_achievement_list_t* l = rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
        if (l) {
            for (uint32_t b = 0; b < l->num_buckets; b++)
                for (uint32_t i = 0; i < l->buckets[b].num_achievements; i++) {
                    const rc_client_achievement_t* a = l->buckets[b].achievements[i];
                    if (a->bucket == RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED) continue;
                    Ach x;
                    x.id = a->id;
                    x.title = a->title ? a->title : "";
                    x.desc = a->description ? a->description : "";
                    x.badge = a->badge_name;
                    x.points = a->points;
                    x.unlock_time = a->unlocked ? (a->unlock_time ? (long long)a->unlock_time : now()) : 0;
                    x.hardcore = (a->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE) != 0;
                    g.total++;
                    g.points_total += x.points;
                    if (a->unlocked) { g.unlocked++; g.points_unlocked += x.points; }
                    g.achs.push_back(std::move(x));
                }
            rc_client_destroy_achievement_list(l);
        }
        g.updated = now();
        gc = std::move(g);
        if (!p.cache_dir.empty() && !save_cache(p.cache_dir, gc)) log("cache write failed");
    }

    void on_event(const rc_client_event_t* e) {
        switch (e->type) {
            case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
                const rc_client_achievement_t* a = e->achievement;
                std::string t = a->title ? a->title : "";
                osd::push("Achievement unlocked: " + t + " (" + std::to_string(a->points) + ")", osd::Kind::Good, 5.0);
                Recent r;
                r.game_key = p.game_key;
                r.game_title = gc.title;
                r.title = t;
                r.desc = a->description ? a->description : "";
                r.points = a->points;
                r.time = now();
                r.hardcore = rc_client_get_hardcore_enabled(client) != 0;
                recent_push(&recent, r);
                if (!p.cache_dir.empty()) save_recent(p.cache_dir, recent);
                rebuild_cache();
                log("unlocked '" + t + "'");
                break;
            }
            case RC_CLIENT_EVENT_GAME_COMPLETED: osd::push("Game mastered! All achievements unlocked", osd::Kind::Good, 6.0); break;
            case RC_CLIENT_EVENT_SERVER_ERROR:
                log(std::string("server error: ") + (e->server_error && e->server_error->error_message ? e->server_error->error_message : "?"));
                break;
            case RC_CLIENT_EVENT_DISCONNECTED: osd::push("Achievements: offline, unlocks will be sent later", osd::Kind::Warn, 4.0); break;
            case RC_CLIENT_EVENT_RECONNECTED: osd::push("Achievements: back online", osd::Kind::Good, 3.0); break;
            case RC_CLIENT_EVENT_RESET: log("hardcore toggled: reset requested"); break;
            default: break;
        }
    }
};

/* ---- rc_client C callbacks ---- */
static uint32_t RC_CCONV c_read(uint32_t addr, uint8_t* buf, uint32_t n, rc_client_t* c) {
    auto* d = (Session::Impl*)rc_client_get_userdata(c);
    return d && d->regions_ok ? rc_libretro_memory_read(&d->regions, addr, buf, n) : 0;
}
static void RC_CCONV c_server(const rc_api_request_t* req, rc_client_server_callback_t cb, void* data, rc_client_t* c) {
    auto* d = (Session::Impl*)rc_client_get_userdata(c);
    Session::Impl::Job j{req->url ? req->url : "", req->content_type ? req->content_type : "application/x-www-form-urlencoded",
                         req->post_data ? req->post_data : "", cb, data};
    if (d->h.inline_http) {
        std::string body;
        int st = 0;
        if (!d->h.http || !d->h.http(j.url, j.ct, j.post, &body, &st)) { body.clear(); st = 0; }
        d->deliver(Reply{cb, data, body, st});
        return;
    }
    std::lock_guard<std::mutex> l(d->mu);
    d->queue.push_back(std::move(j));
    d->cv.notify_one();
}
static void RC_CCONV c_event(const rc_client_event_t* e, rc_client_t* c) {
    if (auto* d = (Session::Impl*)rc_client_get_userdata(c)) d->on_event(e);
}
static void RC_CCONV c_log(const char* m, const rc_client_t* c) {
    if (auto* d = (Session::Impl*)rc_client_get_userdata(c)) d->log(m);   // never contains the token: rcheevos redacts it
}
static void RC_CCONV c_login(int result, const char* err, rc_client_t* c, void*) {
    auto* d = (Session::Impl*)rc_client_get_userdata(c);
    if (result != RC_OK) {
        d->log(std::string("login failed: ") + (err ? err : "?"));
        osd::push("Achievements: sign-in failed (open Settings > Account)", osd::Kind::Warn, 5.0);
        return;
    }
    const rc_client_user_t* u = rc_client_get_user_info(c);
    d->log(std::string("signed in as ") + (u && u->display_name ? u->display_name : "?"));
}
static void RC_CCONV c_game(int result, const char* err, rc_client_t* c, void*) {
    auto* d = (Session::Impl*)rc_client_get_userdata(c);
    if (result != RC_OK) {
        d->log(std::string("game load failed: ") + (err ? err : "?"));
        if (result == RC_NO_GAME_LOADED) d->log("no achievements for this game");
        return;
    }
    d->ready = true;
    d->rebuild_cache();
    const bool hc = rc_client_get_hardcore_enabled(c) != 0;
    if (d->gc.total)
        osd::push(d->gc.title + ": " + std::to_string(d->gc.unlocked) + "/" + std::to_string(d->gc.total) + " achievements" + (hc ? " (hardcore)" : ""), osd::Kind::Info, 4.0);
    else
        osd::push("No achievements for this game", osd::Kind::Info, 3.0);
    d->log("game ready: " + d->gc.title + " " + std::to_string(d->gc.unlocked) + "/" + std::to_string(d->gc.total));
}

static void RC_CCONV mem_info(uint32_t id, rc_libretro_core_memory_info_t* info);
static Session::Impl* g_mem_owner = nullptr;
static void RC_CCONV mem_info(uint32_t id, rc_libretro_core_memory_info_t* info) {
    info->data = nullptr;
    info->size = 0;
    if (g_mem_owner && g_mem_owner->h.core_memory) g_mem_owner->h.core_memory(id, &info->data, &info->size);
}

Session::Session() : d_(new Impl) {}
Session::~Session() { stop(); delete d_; }

static bool read_all(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out->empty();
}

bool Session::start(const Params& p, const Host& h) {
    Impl& d = *d_;
    d.p = p;
    d.h = h;
    auto no = [&](const std::string& why) { d.log("off: " + why); return false; };
    if (!p.cfg.enabled || !p.cfg.signed_in()) return no("not enabled or not signed in");
    const unsigned console = console_for_system(p.system);
    if (!console) return no("unsupported system '" + p.system + "'");

    char hash[33] = {0};
    bool hashed;
    if (console == RC_CONSOLE_PLAYSTATION) hashed = rc_hash_generate_from_file(hash, console, p.rom_path.c_str()) != 0;
    else {
        std::vector<uint8_t> buf;
        hashed = read_all(p.rom_path, &buf) && rc_hash_generate_from_buffer(hash, console, buf.data(), buf.size()) != 0;
    }
    if (!hashed) return no("could not hash " + p.rom_path + " (unsupported disc format?)");

    g_mem_owner = &d;
    const retro_memory_map mm{h.mmap.empty() ? nullptr : h.mmap.data(), (unsigned)h.mmap.size()};
    if (!rc_libretro_memory_init(&d.regions, h.mmap.empty() ? nullptr : &mm, mem_info, console) || d.regions.count == 0)
        return no("core exposes no usable memory");
    d.regions_ok = true;

    d.client = rc_client_create(c_read, c_server);
    rc_client_set_userdata(d.client, &d);
    rc_client_enable_logging(d.client, RC_CLIENT_LOG_LEVEL_WARN, c_log);
    rc_client_set_event_handler(d.client, c_event);
    rc_client_set_hardcore_enabled(d.client, p.cfg.hardcore ? 1 : 0);
    d.recent = load_recent(p.cache_dir);
    if (!h.inline_http) d.worker = std::thread([&d] { d.worker_main(); });
    d.started = true;
    rc_client_begin_login_with_token(d.client, p.cfg.username.c_str(), p.cfg.token.c_str(), c_login, nullptr);
    rc_client_begin_load_game(d.client, hash, c_game, nullptr);
    d.log(std::string("started, hash ") + hash + (p.cfg.hardcore ? ", hardcore" : ", softcore"));
    return true;
}

void Session::frame() {
    if (!d_->started) return;
    d_->drain();
    rc_client_do_frame(d_->client);
}
void Session::idle() {
    if (!d_->started) return;
    d_->drain();
    rc_client_idle(d_->client);
}
void Session::reset() { if (d_->started) rc_client_reset(d_->client); }

void Session::stop() {
    Impl& d = *d_;
    if (d.worker.joinable()) {
        { std::lock_guard<std::mutex> l(d.mu); d.quit = true; }
        d.cv.notify_all();
        d.worker.join();
    }
    if (d.client) {
        d.drain();
        rc_client_destroy(d.client);   // unlock requests still in flight are re-sent by RA next session (server keeps them)
        d.client = nullptr;
    }
    if (d.regions_ok) { rc_libretro_memory_destroy(&d.regions); d.regions_ok = false; }
    if (g_mem_owner == &d) g_mem_owner = nullptr;
    d.started = d.ready = false;
}

bool Session::active() const { return d_->started; }
bool Session::hardcore() const { return d_->started && d_->client && rc_client_get_hardcore_enabled(d_->client) != 0; }
bool Session::game_ready() const { return d_->ready; }
unsigned Session::unlocked() const { return d_->gc.unlocked; }
const GameCache& Session::cache() const { return d_->gc; }

bool Session::allow(Feature f) {
    if (!hardcore_blocks(hardcore(), f)) return true;
    long long t = (long long)std::time(nullptr);
    if (t != d_->last_block_msg) { d_->last_block_msg = t; osd::push(block_message(f), osd::Kind::Warn, 2.5); }
    return false;
}
bool Session::allow_speed(double s) {
    if (speed_allowed(hardcore(), s)) return true;
    return allow(s < 1.0 ? Feature::SlowMotion : Feature::FastForward);
}

}  // namespace cheevos
