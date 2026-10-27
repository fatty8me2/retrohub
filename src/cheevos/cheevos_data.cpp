#include "cheevos_data.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "json.h"
#include "winutil.h"

namespace cheevos {

using json::Value;

static void put(Value& o, const char* k, Value v) { o.o.emplace_back(k, std::move(v)); }

std::string config_to_json(const Config& c) {
    Value o = json::make_object();
    put(o, "enabled", json::make_bool(c.enabled));
    put(o, "hardcore", json::make_bool(c.hardcore));
    put(o, "username", json::make_string(c.username));
    put(o, "token", json::make_string(c.token));
    return json::stringify(o);
}
bool config_from_json(const std::string& text, Config* out) {
    Value v;
    if (!json::parse(text, &v) || !v.is_object()) return false;
    out->enabled = v.boolean("enabled");
    out->hardcore = v.boolean("hardcore");
    out->username = v.str("username");
    out->token = v.str("token");
    return true;
}

static bool slurp(const std::string& p, std::string* s) {
    std::ifstream f(std::filesystem::u8path(p), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *s = ss.str();
    return true;
}
static bool spit(const std::string& p, const std::string& s) {
    std::error_code ec;
    std::filesystem::path fp = std::filesystem::u8path(p);
    std::filesystem::create_directories(fp.parent_path(), ec);
    std::filesystem::path tmp = fp;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << s;
        if (!f) return false;
    }
    std::filesystem::rename(tmp, fp, ec);
    if (ec) { std::filesystem::remove(fp, ec); std::filesystem::rename(tmp, fp, ec); }
    return !ec;
}

bool load_config(const std::string& path, Config* out) {
    std::string t;
    return slurp(path, &t) && config_from_json(t, out);
}
bool save_config(const std::string& path, const Config& c) { return spit(path, config_to_json(c)); }

std::string cache_to_json(const GameCache& g) {
    Value o = json::make_object();
    put(o, "game_key", json::make_string(g.game_key));
    put(o, "title", json::make_string(g.title));
    put(o, "game_id", json::make_int(g.game_id));
    put(o, "total", json::make_int(g.total));
    put(o, "unlocked", json::make_int(g.unlocked));
    put(o, "points_total", json::make_int(g.points_total));
    put(o, "points_unlocked", json::make_int(g.points_unlocked));
    put(o, "updated", json::make_int(g.updated));
    Value a = json::make_array();
    for (const Ach& x : g.achs) {
        Value e = json::make_object();
        put(e, "id", json::make_int(x.id));
        put(e, "title", json::make_string(x.title));
        put(e, "desc", json::make_string(x.desc));
        put(e, "badge", json::make_string(x.badge));
        put(e, "points", json::make_int(x.points));
        put(e, "unlock_time", json::make_int(x.unlock_time));
        put(e, "hardcore", json::make_bool(x.hardcore));
        a.a.push_back(std::move(e));
    }
    put(o, "achievements", std::move(a));
    return json::stringify(o);
}
bool cache_from_json(const std::string& text, GameCache* out) {
    Value v;
    if (!json::parse(text, &v) || !v.is_object()) return false;
    GameCache g;
    g.game_key = v.str("game_key");
    g.title = v.str("title");
    g.game_id = (unsigned)v.i64("game_id");
    g.total = (unsigned)v.i64("total");
    g.unlocked = (unsigned)v.i64("unlocked");
    g.points_total = (unsigned)v.i64("points_total");
    g.points_unlocked = (unsigned)v.i64("points_unlocked");
    g.updated = v.i64("updated");
    if (const Value* a = v.get("achievements"); a && a->is_array())
        for (const Value& e : a->a) {
            Ach x;
            x.id = (unsigned)e.i64("id");
            x.title = e.str("title");
            x.desc = e.str("desc");
            x.badge = e.str("badge");
            x.points = (unsigned)e.i64("points");
            x.unlock_time = e.i64("unlock_time");
            x.hardcore = e.boolean("hardcore");
            g.achs.push_back(std::move(x));
        }
    *out = std::move(g);
    return true;
}

std::string recent_to_json(const std::vector<Recent>& r) {
    Value a = json::make_array();
    for (const Recent& x : r) {
        Value e = json::make_object();
        put(e, "game_key", json::make_string(x.game_key));
        put(e, "game_title", json::make_string(x.game_title));
        put(e, "title", json::make_string(x.title));
        put(e, "desc", json::make_string(x.desc));
        put(e, "points", json::make_int(x.points));
        put(e, "time", json::make_int(x.time));
        put(e, "hardcore", json::make_bool(x.hardcore));
        a.a.push_back(std::move(e));
    }
    Value o = json::make_object();
    put(o, "recent", std::move(a));
    return json::stringify(o);
}
bool recent_from_json(const std::string& text, std::vector<Recent>* out) {
    Value v;
    if (!json::parse(text, &v) || !v.is_object()) return false;
    out->clear();
    if (const Value* a = v.get("recent"); a && a->is_array())
        for (const Value& e : a->a) {
            Recent x;
            x.game_key = e.str("game_key");
            x.game_title = e.str("game_title");
            x.title = e.str("title");
            x.desc = e.str("desc");
            x.points = (unsigned)e.i64("points");
            x.time = e.i64("time");
            x.hardcore = e.boolean("hardcore");
            out->push_back(std::move(x));
        }
    return true;
}
void recent_push(std::vector<Recent>* r, const Recent& n, size_t cap) {
    for (size_t i = 0; i < r->size();)
        if ((*r)[i].game_key == n.game_key && (*r)[i].title == n.title) r->erase(r->begin() + (long)i); else i++;
    r->insert(r->begin(), n);
    if (r->size() > cap) r->resize(cap);
}

std::string safe_key(const std::string& k) {
    std::string s;
    for (unsigned char c : k) s += (isalnum(c) || c == '-' || c == '_' || c == '.') ? (char)c : '_';
    if (s.empty() || s[0] == '.') s.insert(0, "g");
    return s;
}
static std::string join(const std::string& d, const std::string& f) {
    if (d.empty() || d.back() == '/' || d.back() == '\\') return d + f;
    return d + "\\" + f;
}
std::string cache_path(const std::string& dir, const std::string& k) { return join(dir, "game_" + safe_key(k) + ".json"); }
std::string recent_path(const std::string& dir) { return join(dir, "recent.json"); }
bool save_cache(const std::string& dir, const GameCache& g) { return spit(cache_path(dir, g.game_key), cache_to_json(g)); }
bool load_cache(const std::string& dir, const std::string& k, GameCache* out) {
    std::string t;
    return slurp(cache_path(dir, k), &t) && cache_from_json(t, out);
}
bool save_recent(const std::string& dir, const std::vector<Recent>& r) { return spit(recent_path(dir), recent_to_json(r)); }
std::vector<Recent> load_recent(const std::string& dir) {
    std::vector<Recent> r;
    std::string t;
    if (slurp(recent_path(dir), &t)) recent_from_json(t, &r);
    return r;
}

std::string progress_text(const GameCache& g) {
    if (!g.total) return "No achievements";
    char b[96];
    std::snprintf(b, sizeof b, "%u of %u unlocked (%u%%)", g.unlocked, g.total, g.unlocked * 100 / g.total);
    return b;
}

bool hardcore_blocks(bool hc, Feature f) {
    (void)f;
    return hc;   // every listed feature is blocked in hardcore
}
const char* block_message(Feature f) {
    switch (f) {
        case Feature::LoadState: return "Hardcore: save states are off";
        case Feature::Rewind: return "Hardcore: rewind is off";
        case Feature::Cheats: return "Hardcore: cheats are off";
        case Feature::FastForward: return "Hardcore: fast-forward is off";
        case Feature::SlowMotion: return "Hardcore: slow motion is off";
    }
    return "Hardcore mode";
}
bool speed_allowed(bool hc, double s) { return !hc || (s >= 1.0 && s <= 1.0); }

}  // namespace cheevos
