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
                               "preferred_copy", "progress",
                               "profile.avatar", "profile.fav_game", "profile.status", nullptr};
const int kFieldCount = 11;

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

/* Keys without a per-key stamp (legacy files, hand-built Prefs) inherit the field stamp. */
void fill_key_stamps(const std::vector<std::string>& live, std::map<std::string, long long>* kts, long long field_t) {
    for (const auto& k : live) if (!kts->count(k)) (*kts)[k] = field_t > 0 ? field_t : 1;
}

bool set_keyset(Prefs* p, std::vector<std::string>* live, std::map<std::string, long long>* kts, std::vector<std::string> nv,
                const char* field, long long t) {
    nv = norm(std::move(nv));
    fill_key_stamps(*live, kts, p->stamp(field));
    if (nv == *live) return false;
    if (!t) t = now_ms();
    for (const auto& k : nv) if (!std::binary_search(live->begin(), live->end(), k)) (*kts)[k] = t;
    for (const auto& k : *live) if (!std::binary_search(nv.begin(), nv.end(), k)) (*kts)[k] = -t;
    *live = std::move(nv);
    p->ts[field] = t;
    (std::string(field) == "favorites" ? p->legacy_favorites : p->legacy_hidden) = false;
    return true;
}

void merge_keyset(const std::vector<std::string>& la, const std::map<std::string, long long>& ka, long long fa,
                  const std::vector<std::string>& lb, const std::map<std::string, long long>& kb, long long fb,
                  bool a_legacy, bool b_legacy,
                  std::vector<std::string>* out, std::map<std::string, long long>* kout) {
    std::map<std::string, long long> A = ka, B = kb;
    fill_key_stamps(la, &A, fa);
    fill_key_stamps(lb, &B, fb);
    /* A legacy side (older client: one stamp for the whole list) that is newer than the other side's key and lacks it removed it. */
    auto legacy_tombs = [](bool legacy, long long f, std::map<std::string, long long>* self, const std::map<std::string, long long>& other) {
        if (!legacy) return;
        for (const auto& kv : other) {
            long long s = kv.second < 0 ? -kv.second : kv.second;
            if (!self->count(kv.first) && f > s) (*self)[kv.first] = -f;
        }
    };
    {
        std::map<std::string, long long> A0 = A, B0 = B;
        legacy_tombs(a_legacy, fa, &A, B0);
        legacy_tombs(b_legacy, fb, &B, A0);
    }
    kout->clear();
    for (const auto& kv : A) (*kout)[kv.first] = kv.second;
    for (const auto& kv : B) {
        auto it = kout->find(kv.first);
        if (it == kout->end()) { (*kout)[kv.first] = kv.second; continue; }
        long long x = it->second, y = kv.second;
        long long ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
        if (ay > ax || (ay == ax && y > x)) it->second = y;   // tie: present beats tombstone
    }
    out->clear();
    for (const auto& kv : *kout) if (kv.second > 0) out->push_back(kv.first);
}

/* ---- per-key merge of map-like fields (renames, covers, preferred_copy, progress, user_shelves by name) ---- */

bool is_map_field(const std::string& f) {
    return f == "renames" || f == "covers" || f == "preferred_copy" || f == "progress" || f == "user_shelves";
}

template <class V> void fill_map_stamps(const std::map<std::string, V>& live, std::map<std::string, long long>* kts, long long ft) {
    for (const auto& kv : live) if (!kts->count(kv.first)) (*kts)[kv.first] = ft > 0 ? ft : 1;
}

/* Stamp the difference between two states of a map: new/changed keys get +t, removed keys -t (tombstone). */
template <class V> void stamp_map_change(std::map<std::string, long long>* kts, const std::map<std::string, V>& old_m,
                                         const std::map<std::string, V>& new_m, long long t) {
    for (const auto& kv : new_m) {
        auto it = old_m.find(kv.first);
        if (it == old_m.end() || !(it->second == kv.second)) (*kts)[kv.first] = t;
    }
    for (const auto& kv : old_m) if (!new_m.count(kv.first)) (*kts)[kv.first] = -t;
}

template <class V> bool set_map_field(Prefs* p, std::map<std::string, V>* slot, std::map<std::string, V> nv, const char* field, long long t) {
    auto& kts = p->map_ts[field];
    fill_map_stamps(*slot, &kts, p->stamp(field));
    if (*slot == nv) return false;
    if (!t) t = now_ms();
    stamp_map_change(&kts, *slot, nv, t);
    *slot = std::move(nv);
    p->ts[field] = t;
    p->legacy_maps.erase(field);
    return true;
}

/* Per key: the newer stamp wins (a tombstone is a stamp); equal stamps: present beats tombstone, then the larger value. A legacy side
 * (an older client wrote one stamp for the whole map) that is newer than the other side's key and lacks it removed it. */
template <class V>
void merge_map(const std::map<std::string, V>& la, std::map<std::string, long long> A, long long fa, bool a_legacy,
               const std::map<std::string, V>& lb, std::map<std::string, long long> B, long long fb, bool b_legacy,
               std::map<std::string, V>* out, std::map<std::string, long long>* kout) {
    fill_map_stamps(la, &A, fa);
    fill_map_stamps(lb, &B, fb);
    auto legacy_tombs = [](bool legacy, long long f, std::map<std::string, long long>* self, const std::map<std::string, long long>& other) {
        if (!legacy) return;
        for (const auto& kv : other) {
            long long s = kv.second < 0 ? -kv.second : kv.second;
            if (!self->count(kv.first) && f > s) (*self)[kv.first] = -f;
        }
    };
    {
        std::map<std::string, long long> A0 = A, B0 = B;
        legacy_tombs(a_legacy, fa, &A, B0);
        legacy_tombs(b_legacy, fb, &B, A0);
    }
    out->clear();
    kout->clear();
    std::set<std::string> keys;
    for (const auto& kv : A) keys.insert(kv.first);
    for (const auto& kv : B) keys.insert(kv.first);
    for (const auto& k : keys) {
        auto ia = A.find(k), ib = B.find(k);
        auto va = la.find(k), vb = lb.find(k);
        const bool pa = ia != A.end() && ia->second > 0 && va != la.end();
        const bool pb = ib != B.end() && ib->second > 0 && vb != lb.end();
        const long long sa = ia == A.end() ? 0 : (ia->second < 0 ? -ia->second : ia->second);
        const long long sb = ib == B.end() ? 0 : (ib->second < 0 ? -ib->second : ib->second);
        bool take_b;
        if (sa != sb) take_b = sb > sa;
        else if (pa != pb) take_b = pb;
        else if (pa && pb) take_b = va->second < vb->second;
        else take_b = false;
        const bool present = take_b ? pb : pa;
        const long long st = take_b ? sb : sa;
        if (!st) continue;
        (*kout)[k] = present ? st : -st;
        if (present) (*out)[k] = take_b ? vb->second : va->second;
    }
}

/* Reconcile parsed per-key stamps with the live map (tombstones carry no live key; live keys with no stamp inherit the field stamp). */
template <class V> void sanitize_map_stamps(Prefs* p, const char* field, const std::map<std::string, V>& live) {
    auto& kts = p->map_ts[field];
    for (auto it = kts.begin(); it != kts.end(); ++it) {
        const bool isl = live.count(it->first) != 0;
        if (isl && it->second < 0) it->second = -it->second;
        else if (!isl && it->second > 0) it->second = -it->second;
    }
    fill_map_stamps(live, &kts, p->stamp(field));
}

json::Value key_stamps(const std::map<std::string, long long>& m) {
    json::Value o = json::make_object();
    for (const auto& kv : m) o.o.push_back({kv.first, json::make_int(kv.second)});
    return o;
}
void read_key_stamps(const json::Value* v, std::map<std::string, long long>* out) {
    out->clear();
    if (!v || !v->is_object()) return;
    for (const auto& kv : v->o) if (kv.second.is_number() && kv.second.i != 0) (*out)[kv.first] = kv.second.i;
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

json::Value shelf_json(const Shelf& s) {
    json::Value e = json::make_object();
    e.o.push_back({"name", json::make_string(s.name)});
    e.o.push_back({"keys", str_array(s.keys)});
    if (s.smart) {
        json::Value r = json::make_object();
        r.o.push_back({"systems", str_array(s.rule.systems)});
        r.o.push_back({"genre", json::make_string(s.rule.genre)});
        r.o.push_back({"min_players", json::make_int(s.rule.min_players)});
        r.o.push_back({"status", json::make_string(s.rule.status)});
        r.o.push_back({"favorites_only", json::make_bool(s.rule.favorites_only)});
        r.o.push_back({"not_played_days", json::make_int(s.rule.not_played_days)});
        r.o.push_back({"decade", json::make_int(s.rule.decade)});
        e.o.push_back({"rule", std::move(r)});
    }
    return e;
}
bool read_shelf(const json::Value& e, Shelf* s) {
    if (!e.is_object()) return false;
    s->name = e.str("name");
    read_array(e.get("keys"), &s->keys);
    if (const json::Value* r = e.get("rule"); r && r->is_object()) {
        s->smart = true;
        read_array(r->get("systems"), &s->rule.systems);
        s->rule.genre = r->str("genre");
        s->rule.min_players = (int)r->i64("min_players");
        s->rule.status = r->str("status");
        s->rule.favorites_only = r->boolean("favorites_only");
        s->rule.not_played_days = (int)r->i64("not_played_days");
        s->rule.decade = (int)r->i64("decade");
    }
    return !s->name.empty();
}
std::map<std::string, Shelf> shelf_map(const std::vector<Shelf>& v) {
    std::map<std::string, Shelf> m;
    for (const auto& s : v) m.emplace(s.name, s);   // first one wins on a duplicate name
    return m;
}
constexpr size_t kNoteMax = 500;
std::map<std::string, Progress> clean_progress(const std::map<std::string, Progress>& in) {
    std::map<std::string, Progress> out;
    for (auto kv : in) {
        if (kv.first.empty()) continue;
        if (kv.second.note.size() > kNoteMax) {   // cut at a character boundary
            size_t n = kNoteMax;
            while (n > 0 && ((unsigned char)kv.second.note[n] & 0xC0) == 0x80) --n;
            kv.second.note.resize(n);
        }
        if (kv.second.status.empty() && kv.second.note.empty()) continue;
        out[kv.first] = kv.second;
    }
    return out;
}

/* Value of one field as a canonical string (for tie-breaking). */
std::string field_repr(const Prefs& p, const std::string& f) {
    Prefs one;
    if (f == "favorites") one.favorites = p.favorites;
    else if (f == "hidden") one.hidden = p.hidden;
    else if (f == "theme") one.theme = p.theme;
    else if (f == "profile.avatar") one.profile.avatar = p.profile.avatar;
    else if (f == "profile.fav_game") one.profile.fav_game = p.profile.fav_game;
    else if (f == "profile.status") one.profile.status = p.profile.status;
    return serialize(one);
}

void copy_field(const Prefs& from, Prefs* to, const std::string& f) {
    if (f == "favorites") to->favorites = from.favorites;
    else if (f == "hidden") to->hidden = from.hidden;
    else if (f == "theme") to->theme = from.theme;
    else if (f == "profile.avatar") to->profile.avatar = from.profile.avatar;
    else if (f == "profile.fav_game") to->profile.fav_game = from.profile.fav_game;
    else if (f == "profile.status") to->profile.status = from.profile.status;
}

}  // namespace

bool set_favorites(Prefs* p, std::vector<std::string> k, long long t) { return set_keyset(p, &p->favorites, &p->favorites_ts, std::move(k), "favorites", t); }
bool set_favorite(Prefs* p, const std::string& key, bool fav, long long t) {
    std::set<std::string> s(p->favorites.begin(), p->favorites.end());
    if (fav) s.insert(key); else s.erase(key);
    return set_favorites(p, std::vector<std::string>(s.begin(), s.end()), t);
}
bool set_user_shelves(Prefs* p, std::vector<Shelf> v, long long t) {
    for (auto& s : v) { std::vector<std::string> seen; for (auto& k : s.keys) if (!k.empty() && std::find(seen.begin(), seen.end(), k) == seen.end()) seen.push_back(k); s.keys = seen; }
    auto& kts = p->map_ts["user_shelves"];
    const auto old_m = shelf_map(p->user_shelves);
    fill_map_stamps(old_m, &kts, p->stamp("user_shelves"));
    if (p->user_shelves == v) return false;
    if (!t) t = now_ms();
    stamp_map_change(&kts, old_m, shelf_map(v), t);
    p->user_shelves = std::move(v);
    p->ts["user_shelves"] = t;
    p->legacy_maps.erase("user_shelves");
    return true;
}
bool set_hidden(Prefs* p, std::vector<std::string> k, long long t) { return set_keyset(p, &p->hidden, &p->hidden_ts, std::move(k), "hidden", t); }
bool set_renames(Prefs* p, std::map<std::string, std::string> m, long long t) { return set_map_field(p, &p->renames, std::move(m), "renames", t); }
bool set_covers(Prefs* p, std::map<std::string, std::string> m, long long t) { return set_map_field(p, &p->covers, std::move(m), "covers", t); }
bool set_preferred_copy(Prefs* p, std::map<std::string, std::string> m, long long t) { return set_map_field(p, &p->preferred_copy, std::move(m), "preferred_copy", t); }
bool set_progress(Prefs* p, std::map<std::string, Progress> m, long long t) { return set_map_field(p, &p->progress, clean_progress(m), "progress", t); }
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
            if (read_shelf(e, &s)) p.user_shelves.push_back(std::move(s));
        }
    }
    read_array(v.get("hidden"), &p.hidden);
    p.hidden = norm(p.hidden);
    read_map(v.get("renames"), &p.renames);
    read_map(v.get("covers"), &p.covers);
    read_map(v.get("preferred_copy"), &p.preferred_copy);
    if (const json::Value* pg = v.get("progress"); pg && pg->is_object()) {
        for (const auto& kv : pg->o) {
            if (!kv.second.is_object()) continue;
            Progress g;
            g.status = kv.second.str("s");
            g.note = kv.second.str("n");
            p.progress[kv.first] = g;
        }
        p.progress = clean_progress(p.progress);
    }
    p.theme = v.str("theme");
    if (const json::Value* pr = v.get("profile"); pr && pr->is_object()) {
        p.profile.avatar = pr->str("avatar");
        p.profile.fav_game = pr->str("fav_game");
        p.profile.status = pr->str("status");
    }
    if (const json::Value* t = v.get("ts"); t && t->is_object())
        for (const auto& kv : t->o) if (kv.second.is_number()) p.ts[kv.first] = kv.second.i;
    if (const json::Value* mt = v.get("map_ts"); mt && mt->is_object())
        for (const auto& kv : mt->o) { std::map<std::string, long long> m; read_key_stamps(&kv.second, &m); p.map_ts[kv.first] = m; }
    for (const char* f : {"renames", "covers", "preferred_copy", "progress", "user_shelves"}) {
        const json::Value* mt = v.get("map_ts");
        const bool has = mt && mt->is_object() && mt->get(f);
        if (!has && p.stamp(f) > 0) p.legacy_maps.insert(f);
    }
    {
        std::map<std::string, Shelf> sm = shelf_map(p.user_shelves);
        sanitize_map_stamps(&p, "renames", p.renames);
        sanitize_map_stamps(&p, "covers", p.covers);
        sanitize_map_stamps(&p, "preferred_copy", p.preferred_copy);
        sanitize_map_stamps(&p, "progress", p.progress);
        sanitize_map_stamps(&p, "user_shelves", sm);
    }
    {   // fields from a newer client: kept verbatim
        static const std::set<std::string> known = {"version", "favorites", "user_shelves", "hidden", "favorites_ts", "hidden_ts", "renames", "covers",
                                                    "preferred_copy", "progress", "map_ts", "theme", "profile", "ts"};
        for (const auto& kv : v.o) if (!known.count(kv.first)) p.extra.push_back(kv);
    }
    read_key_stamps(v.get("favorites_ts"), &p.favorites_ts);
    read_key_stamps(v.get("hidden_ts"), &p.hidden_ts);
    p.legacy_favorites = !v.get("favorites_ts") && p.stamp("favorites") > 0;
    p.legacy_hidden = !v.get("hidden_ts") && p.stamp("hidden") > 0;
    {   // tombstones carry no live key; live keys with no stamp inherit the field stamp; stamps must agree with the live list
        for (auto* pr : {&p.favorites_ts, &p.hidden_ts}) {
            const auto& live = pr == &p.favorites_ts ? p.favorites : p.hidden;
            long long ft = p.ts.count(pr == &p.favorites_ts ? "favorites" : "hidden") ? p.ts[pr == &p.favorites_ts ? "favorites" : "hidden"] : 0;
            for (auto it = pr->begin(); it != pr->end();) {
                bool isl = std::binary_search(live.begin(), live.end(), it->first);
                if (isl && it->second < 0) it->second = -it->second;
                else if (!isl && it->second > 0) it->second = -it->second;
                ++it;
            }
            fill_key_stamps(live, pr, ft);
        }
    }
    if (out) *out = std::move(p);
    return true;
}

std::string serialize(const Prefs& p) {
    json::Value o = json::make_object();
    auto add = [&](const char* k, json::Value v) { o.o.push_back({k, std::move(v)}); };
    add("version", json::make_int(1));
    add("favorites", str_array(p.favorites));
    json::Value shelves = json::make_array();
    for (const auto& s : p.user_shelves) shelves.a.push_back(shelf_json(s));
    add("user_shelves", std::move(shelves));
    add("hidden", str_array(p.hidden));
    add("favorites_ts", key_stamps(p.favorites_ts));
    add("hidden_ts", key_stamps(p.hidden_ts));
    add("renames", str_map(p.renames));
    add("covers", str_map(p.covers));
    add("preferred_copy", str_map(p.preferred_copy));
    {
        json::Value pg = json::make_object();
        for (const auto& kv : p.progress) {
            json::Value e = json::make_object();
            e.o.push_back({"s", json::make_string(kv.second.status)});
            e.o.push_back({"n", json::make_string(kv.second.note)});
            pg.o.push_back({kv.first, std::move(e)});
        }
        add("progress", std::move(pg));
        json::Value mt = json::make_object();
        for (const char* f : {"renames", "covers", "preferred_copy", "progress", "user_shelves"}) {
            auto it = p.map_ts.find(f);
            mt.o.push_back({f, key_stamps(it == p.map_ts.end() ? std::map<std::string, long long>() : it->second)});
        }
        add("map_ts", std::move(mt));
    }
    add("theme", json::make_string(p.theme));
    json::Value pr = json::make_object();
    pr.o.push_back({"avatar", json::make_string(p.profile.avatar)});
    pr.o.push_back({"fav_game", json::make_string(p.profile.fav_game)});
    pr.o.push_back({"status", json::make_string(p.profile.status)});
    add("profile", std::move(pr));
    json::Value ts = json::make_object();
    for (const auto& kv : p.ts) ts.o.push_back({kv.first, json::make_int(kv.second)});
    add("ts", std::move(ts));
    for (const auto& kv : p.extra) add(kv.first.c_str(), kv.second);
    return json::stringify(o);
}

Prefs merge(const Prefs& a, const Prefs& b) {
    Prefs r = a;
    r.ts.clear();
    r.legacy_favorites = r.legacy_hidden = false;
    r.legacy_maps.clear();
    r.map_ts.clear();
    auto mts = [](const Prefs& x, const char* f) { auto it = x.map_ts.find(f); return it == x.map_ts.end() ? std::map<std::string, long long>() : it->second; };
    auto leg = [](const Prefs& x, const char* f) { return x.legacy_maps.count(f) != 0; };
    merge_map(a.renames, mts(a, "renames"), a.stamp("renames"), leg(a, "renames"), b.renames, mts(b, "renames"), b.stamp("renames"), leg(b, "renames"), &r.renames, &r.map_ts["renames"]);
    merge_map(a.covers, mts(a, "covers"), a.stamp("covers"), leg(a, "covers"), b.covers, mts(b, "covers"), b.stamp("covers"), leg(b, "covers"), &r.covers, &r.map_ts["covers"]);
    merge_map(a.preferred_copy, mts(a, "preferred_copy"), a.stamp("preferred_copy"), leg(a, "preferred_copy"), b.preferred_copy, mts(b, "preferred_copy"), b.stamp("preferred_copy"), leg(b, "preferred_copy"), &r.preferred_copy, &r.map_ts["preferred_copy"]);
    merge_map(a.progress, mts(a, "progress"), a.stamp("progress"), leg(a, "progress"), b.progress, mts(b, "progress"), b.stamp("progress"), leg(b, "progress"), &r.progress, &r.map_ts["progress"]);
    {   // shelves: per name; the order comes from the side whose field stamp is newer
        std::map<std::string, Shelf> ma = shelf_map(a.user_shelves), mb = shelf_map(b.user_shelves), mr;
        merge_map(ma, mts(a, "user_shelves"), a.stamp("user_shelves"), leg(a, "user_shelves"), mb, mts(b, "user_shelves"), b.stamp("user_shelves"), leg(b, "user_shelves"), &mr, &r.map_ts["user_shelves"]);
        std::vector<std::string> oa, ob;
        for (const auto& s : a.user_shelves) oa.push_back(s.name);
        for (const auto& s : b.user_shelves) ob.push_back(s.name);
        long long ta = a.stamp("user_shelves"), tb = b.stamp("user_shelves");
        const std::vector<std::string>& base = ta != tb ? (tb > ta ? ob : oa) : (ob > oa ? ob : oa);
        r.user_shelves.clear();
        std::set<std::string> done;
        for (const auto& n : base) if (mr.count(n) && done.insert(n).second) r.user_shelves.push_back(mr[n]);
        for (const auto& kv : mr) if (done.insert(kv.first).second) r.user_shelves.push_back(kv.second);
    }
    {   // unknown fields (from a newer client): keep every one; when both sides have it, the newer "ts" stamp wins, then the larger text
        std::vector<std::pair<std::string, json::Value>> ex = a.extra;
        for (const auto& kb : b.extra) {
            auto it = std::find_if(ex.begin(), ex.end(), [&](const std::pair<std::string, json::Value>& x) { return x.first == kb.first; });
            if (it == ex.end()) { ex.push_back(kb); continue; }
            long long sa = a.stamp(kb.first), sb = b.stamp(kb.first);
            if (sa != sb ? sb > sa : json::stringify(kb.second) > json::stringify(it->second)) it->second = kb.second;
        }
        std::sort(ex.begin(), ex.end(), [](const std::pair<std::string, json::Value>& x, const std::pair<std::string, json::Value>& y) { return x.first < y.first; });
        r.extra = ex;
        auto known_field = [](const std::string& n) { for (int i = 0; i < kFieldCount; i++) if (n == kFields[i]) return true; return false; };
        for (const auto& kv : a.ts) if (!known_field(kv.first)) r.ts[kv.first] = std::max(kv.second, b.stamp(kv.first));
        for (const auto& kv : b.ts) if (!known_field(kv.first) && !r.ts.count(kv.first)) r.ts[kv.first] = kv.second;
    }
    merge_keyset(a.favorites, a.favorites_ts, a.stamp("favorites"), b.favorites, b.favorites_ts, b.stamp("favorites"), a.legacy_favorites, b.legacy_favorites, &r.favorites, &r.favorites_ts);
    merge_keyset(a.hidden, a.hidden_ts, a.stamp("hidden"), b.hidden, b.hidden_ts, b.stamp("hidden"), a.legacy_hidden, b.legacy_hidden, &r.hidden, &r.hidden_ts);
    for (int i = 0; i < kFieldCount; i++) {
        const std::string f = kFields[i];
        long long ta = a.stamp(f), tb = b.stamp(f);
        if (f == "favorites" || f == "hidden" || is_map_field(f)) { long long t = std::max(ta, tb); if (t) r.ts[f] = t; continue; }
        bool take_b;
        if (ta != tb) take_b = tb > ta;
        else take_b = field_repr(b, f) > field_repr(a, f);   // equal stamps: deterministic, order independent
        if (take_b) copy_field(b, &r, f);
        long long t = std::max(ta, tb);
        if (t) r.ts[f] = t;
    }
    return r;
}

void gc_tombstones(Prefs* p, long long now) {
    if (!now) now = now_ms();
    const long long cutoff = now - 90ll * 24 * 3600 * 1000;
    for (auto* m : {&p->favorites_ts, &p->hidden_ts})
        for (auto it = m->begin(); it != m->end();) {
            if (it->second < 0 && -it->second < cutoff) it = m->erase(it); else ++it;
        }
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

namespace { std::mutex& prefs_mu(); }
Prefs merge_into_file(const std::string& root, const Prefs& incoming) {
    std::lock_guard<std::mutex> lk(prefs_mu());
    Prefs cur;
    load(root, &cur);
    Prefs m = merge(cur, incoming);
    gc_tombstones(&m);
    if (serialize(m) != serialize(cur)) save(root, m);
    return m;
}

/* ---- library mirror ---- */
namespace {
std::mutex g_mu;   // one lock for every read-modify-write of prefs.json (mirror_favorite, merge_into_file, seed_from_library)
std::string g_root;
thread_local bool g_applying = false;   // per thread: a UI favorite toggled meanwhile on another thread is still mirrored
std::mutex& prefs_mu() { return g_mu; }
}

static void (*g_hook)() = nullptr;
void set_change_hook(void (*fn)()) { std::lock_guard<std::mutex> lk(g_mu); g_hook = fn; }

void set_vault_root(const std::string& root) { std::lock_guard<std::mutex> lk(g_mu); g_root = root; }

void mirror_favorite(const std::string& key, bool fav) {
    void (*hook)() = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_root.empty() || g_applying || key.empty()) return;
        Prefs p;
        load(g_root, &p);
        gc_tombstones(&p);
        if (set_favorite(&p, key, fav) && save(g_root, p)) hook = g_hook;
    }
    if (hook) hook();
}

int apply_favorites_to_library(const Prefs& p) {
    if (p.stamp("favorites") == 0) return 0;
    std::set<std::string> want(p.favorites.begin(), p.favorites.end());
    std::vector<std::string> have_v = hub::favorite_keys();
    std::set<std::string> have(have_v.begin(), have_v.end());
    int n = 0;
    g_applying = true;
    for (const auto& k : want) if (!have.count(k)) { hub::set_favorite(k, true); n++; }
    for (const auto& k : have) if (!want.count(k)) { hub::set_favorite(k, false); n++; }
    g_applying = false;
    return n;
}

void seed_from_library() {
    std::string root;
    { std::lock_guard<std::mutex> lk(g_mu); root = g_root; }
    if (root.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    Prefs p;
    load(root, &p);
    if (p.stamp("favorites") != 0) return;
    std::vector<std::string> f = hub::favorite_keys();
    if (f.empty()) return;
    set_favorites(&p, f);
    save(root, p);
}

}}  // namespace hub::prefs
