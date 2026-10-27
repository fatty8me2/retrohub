#include "prefs.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>

#include "json.h"
#include "library.h"
#include "runner_ctl.h"

namespace hub { namespace prefs {

const char* const kFields[] = {"favorites", "user_shelves", "hidden", "renames", "covers", "theme",
                               "profile.avatar", "profile.fav_game", "profile.status", nullptr};
const int kFieldCount = 9;

long long Prefs::stamp(const std::string& f) const {
    auto it = ts.find(f);
    return it == ts.end() ? 0 : it->second;
}

long long now_ms() {
    using namespace std::chrono;
    return (long long)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

namespace {

std::vector<std::string> norm(std::vector<std::string> v) {
    v.erase(std::remove(v.begin(), v.end(), std::string()), v.end());
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

template <class T> bool set_field(Prefs* p, T* slot, T val, const char* field, long long t) {
    if (*slot == val) return false;
    *slot = std::move(val);
    p->ts[field] = t ? t : now_ms();
    return true;
}

json::Value str_array(const std::vector<std::string>& v) {
    json::Value a = json::make_array();
    for (const auto& s : v) a.a.push_back(json::make_string(s));
    return a;
}
json::Value str_map(const std::map<std::string, std::string>& m) {
    json::Value o = json::make_object();
    for (const auto& kv : m) o.o.push_back({kv.first, json::make_string(kv.second)});
    return o;
}
void read_array(const json::Value* v, std::vector<std::string>* out) {
    out->clear();
    if (!v || !v->is_array()) return;
    for (const auto& e : v->a) if (e.is_string() && !e.s.empty()) out->push_back(e.s);
}
void read_map(const json::Value* v, std::map<std::string, std::string>* out) {
    out->clear();
    if (!v || !v->is_object()) return;
    for (const auto& kv : v->o) if (kv.second.is_string()) (*out)[kv.first] = kv.second.s;
}

/* Value of one field as a canonical string (for tie-breaking). */
std::string field_repr(const Prefs& p, const std::string& f) {
    Prefs one;
    if (f == "favorites") one.favorites = p.favorites;
    else if (f == "user_shelves") one.user_shelves = p.user_shelves;
    else if (f == "hidden") one.hidden = p.hidden;
    else if (f == "renames") one.renames = p.renames;
    else if (f == "covers") one.covers = p.covers;
    else if (f == "theme") one.theme = p.theme;
    else if (f == "profile.avatar") one.profile.avatar = p.profile.avatar;
    else if (f == "profile.fav_game") one.profile.fav_game = p.profile.fav_game;
    else if (f == "profile.status") one.profile.status = p.profile.status;
    return serialize(one);
}

void copy_field(const Prefs& from, Prefs* to, const std::string& f) {
    if (f == "favorites") to->favorites = from.favorites;
    else if (f == "user_shelves") to->user_shelves = from.user_shelves;
    else if (f == "hidden") to->hidden = from.hidden;
    else if (f == "renames") to->renames = from.renames;
    else if (f == "covers") to->covers = from.covers;
    else if (f == "theme") to->theme = from.theme;
    else if (f == "profile.avatar") to->profile.avatar = from.profile.avatar;
    else if (f == "profile.fav_game") to->profile.fav_game = from.profile.fav_game;
    else if (f == "profile.status") to->profile.status = from.profile.status;
}

}  // namespace

bool set_favorites(Prefs* p, std::vector<std::string> k, long long t) { return set_field(p, &p->favorites, norm(std::move(k)), "favorites", t); }
bool set_favorite(Prefs* p, const std::string& key, bool fav, long long t) {
    std::set<std::string> s(p->favorites.begin(), p->favorites.end());
    if (fav) s.insert(key); else s.erase(key);
    return set_favorites(p, std::vector<std::string>(s.begin(), s.end()), t);
}
bool set_user_shelves(Prefs* p, std::vector<Shelf> v, long long t) {
    for (auto& s : v) { std::vector<std::string> seen; for (auto& k : s.keys) if (!k.empty() && std::find(seen.begin(), seen.end(), k) == seen.end()) seen.push_back(k); s.keys = seen; }
    return set_field(p, &p->user_shelves, std::move(v), "user_shelves", t);
}
bool set_hidden(Prefs* p, std::vector<std::string> k, long long t) { return set_field(p, &p->hidden, norm(std::move(k)), "hidden", t); }
bool set_renames(Prefs* p, std::map<std::string, std::string> m, long long t) { return set_field(p, &p->renames, std::move(m), "renames", t); }
bool set_covers(Prefs* p, std::map<std::string, std::string> m, long long t) { return set_field(p, &p->covers, std::move(m), "covers", t); }
bool set_theme(Prefs* p, const std::string& v, long long t) { return set_field(p, &p->theme, v, "theme", t); }
bool set_profile_avatar(Prefs* p, const std::string& v, long long t) { return set_field(p, &p->profile.avatar, v, "profile.avatar", t); }
bool set_profile_fav_game(Prefs* p, const std::string& v, long long t) { return set_field(p, &p->profile.fav_game, v, "profile.fav_game", t); }
bool set_profile_status(Prefs* p, const std::string& v, long long t) { return set_field(p, &p->profile.status, v, "profile.status", t); }

bool parse(const std::string& text, Prefs* out) {
    Prefs p;
    json::Value v;
    if (!json::parse(text, &v) || !v.is_object()) { if (out) *out = Prefs(); return false; }
    read_array(v.get("favorites"), &p.favorites);
    p.favorites = norm(p.favorites);
    if (const json::Value* sh = v.get("user_shelves"); sh && sh->is_array()) {
        for (const auto& e : sh->a) {
            if (!e.is_object()) continue;
            Shelf s;
            s.name = e.str("name");
            read_array(e.get("keys"), &s.keys);
            if (!s.name.empty()) p.user_shelves.push_back(std::move(s));
        }
    }
    read_array(v.get("hidden"), &p.hidden);
    p.hidden = norm(p.hidden);
    read_map(v.get("renames"), &p.renames);
    read_map(v.get("covers"), &p.covers);
    p.theme = v.str("theme");
    if (const json::Value* pr = v.get("profile"); pr && pr->is_object()) {
        p.profile.avatar = pr->str("avatar");
        p.profile.fav_game = pr->str("fav_game");
        p.profile.status = pr->str("status");
    }
    if (const json::Value* t = v.get("ts"); t && t->is_object())
        for (const auto& kv : t->o) if (kv.second.is_number()) p.ts[kv.first] = kv.second.i;
    if (out) *out = std::move(p);
    return true;
}

std::string serialize(const Prefs& p) {
    json::Value o = json::make_object();
    auto add = [&](const char* k, json::Value v) { o.o.push_back({k, std::move(v)}); };
    add("version", json::make_int(1));
    add("favorites", str_array(p.favorites));
    json::Value shelves = json::make_array();
    for (const auto& s : p.user_shelves) {
        json::Value e = json::make_object();
        e.o.push_back({"name", json::make_string(s.name)});
        e.o.push_back({"keys", str_array(s.keys)});
        shelves.a.push_back(std::move(e));
    }
    add("user_shelves", std::move(shelves));
    add("hidden", str_array(p.hidden));
    add("renames", str_map(p.renames));
    add("covers", str_map(p.covers));
    add("theme", json::make_string(p.theme));
    json::Value pr = json::make_object();
    pr.o.push_back({"avatar", json::make_string(p.profile.avatar)});
    pr.o.push_back({"fav_game", json::make_string(p.profile.fav_game)});
    pr.o.push_back({"status", json::make_string(p.profile.status)});
    add("profile", std::move(pr));
    json::Value ts = json::make_object();
    for (const auto& kv : p.ts) ts.o.push_back({kv.first, json::make_int(kv.second)});
    add("ts", std::move(ts));
    return json::stringify(o);
}

Prefs merge(const Prefs& a, const Prefs& b) {
    Prefs r = a;
    r.ts.clear();
    for (int i = 0; i < kFieldCount; i++) {
        const std::string f = kFields[i];
        long long ta = a.stamp(f), tb = b.stamp(f);
        bool take_b;
        if (ta != tb) take_b = tb > ta;
        else take_b = field_repr(b, f) > field_repr(a, f);   // equal stamps: deterministic, order independent
        if (take_b) copy_field(b, &r, f);
        long long t = std::max(ta, tb);
        if (t) r.ts[f] = t;
    }
    return r;
}

std::string merge_text(const std::string& local, const std::string& remote) {
    Prefs a, b;
    parse(local, &a);
    parse(remote, &b);
    return serialize(merge(a, b));
}

std::string prefs_path(const std::string& root) {
    std::string d = root;
    while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    return d + "/_account/prefs.json";
}

bool load(const std::string& root, Prefs* out) {
    std::string t;
    if (!runctl::read_text(prefs_path(root), &t)) { if (out) *out = Prefs(); return false; }
    return parse(t, out);
}

bool save(const std::string& root, const Prefs& p) { return runctl::write_text_atomic(prefs_path(root), serialize(p)); }

Prefs merge_into_file(const std::string& root, const Prefs& incoming) {
    Prefs cur;
    load(root, &cur);
    Prefs m = merge(cur, incoming);
    if (serialize(m) != serialize(cur)) save(root, m);
    return m;
}

/* ---- library mirror ---- */
namespace {
std::mutex g_mu;
std::string g_root;
bool g_applying = false;
}

void set_vault_root(const std::string& root) { std::lock_guard<std::mutex> lk(g_mu); g_root = root; }

void mirror_favorite(const std::string& key, bool fav) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_root.empty() || g_applying || key.empty()) return;
    Prefs p;
    load(g_root, &p);
    if (set_favorite(&p, key, fav)) save(g_root, p);
}

int apply_favorites_to_library(const Prefs& p) {
    if (p.stamp("favorites") == 0) return 0;
    std::set<std::string> want(p.favorites.begin(), p.favorites.end());
    std::vector<std::string> have_v = hub::favorite_keys();
    std::set<std::string> have(have_v.begin(), have_v.end());
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_applying = true;
    }
    for (const auto& k : want) if (!have.count(k)) { hub::set_favorite(k, true); n++; }
    for (const auto& k : have) if (!want.count(k)) { hub::set_favorite(k, false); n++; }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_applying = false;
    }
    return n;
}

void seed_from_library() {
    std::string root;
    { std::lock_guard<std::mutex> lk(g_mu); root = g_root; }
    if (root.empty()) return;
    Prefs p;
    load(root, &p);
    if (p.stamp("favorites") != 0) return;
    std::vector<std::string> f = hub::favorite_keys();
    if (f.empty()) return;
    set_favorites(&p, f);
    save(root, p);
}

}}  // namespace hub::prefs
