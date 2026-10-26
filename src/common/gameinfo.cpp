#include "gameinfo.h"
#include "json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <sstream>

namespace gameinfo {

namespace {

char fold_latin1(unsigned c) {   // U+00C0..U+00FF -> base letter, 0 = none
    static const char* m = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (c < 0xC0 || c > 0xFF) return 0;
    char r = m[c - 0xC0];
    return (r == '*' || r == '/') ? 0 : r;
}

std::string lower_ascii(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}

/* Words of a title, lowercase ASCII, accents folded. */
std::vector<std::string> words_of(const std::string& s) {
    std::vector<std::string> w;
    std::string cur;
    auto flush = [&] { if (!cur.empty()) { w.push_back(cur); cur.clear(); } };
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xC3 && i + 1 < s.size()) {
            char f = fold_latin1(0xC0 + ((unsigned char)s[i + 1] - 0x80));
            ++i;
            if (f) cur += (char)std::tolower((unsigned char)f); else flush();
        } else if (c == '&') { flush(); w.push_back("and"); }
        else if (c == '\'' || c == 0x27) { /* apostrophe joins: "Link's" -> "links" */ }
        else if (c >= 0x80) { flush(); while (i + 1 < s.size() && ((unsigned char)s[i + 1] & 0xC0) == 0x80) ++i; }
        else if (std::isalnum(c)) cur += (char)std::tolower(c);
        else flush();
    }
    flush();
    return w;
}

std::string url_enc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string join_ids(const std::vector<std::string>& ids) {
    std::string o;
    for (size_t i = 0; i < ids.size(); ++i) { if (i) o += "%7C"; o += ids[i]; }
    return o;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    size_t p = 0;
    while (true) {
        size_t q = s.find(sep, p);
        v.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return v;
}

int to_int(const std::string& s) { return s.empty() ? 0 : std::atoi(s.c_str()); }

}  // namespace

std::string normalize(const std::string& title) {
    std::vector<std::string> w = words_of(title);
    std::string o;
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] == "version") continue;
        if (i == 0 && w[i] == "the" && w.size() > 1) continue;
        o += w[i];
    }
    return o;
}

std::string normalize_serial(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '.') continue;
        o += (c == '_') ? '-' : (char)std::toupper(c);
    }
    return o;
}

std::string clean_title(const std::string& s) {
    std::string o;
    int depth = 0;
    for (char c : s) {
        if (c == '(' || c == '[') ++depth;
        else if ((c == ')' || c == ']') && depth > 0) --depth;
        else if (!depth) o += c;
    }
    auto trim = [](std::string& t) {
        while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
        size_t a = 0;
        while (a < t.size() && std::isspace((unsigned char)t[a])) ++a;
        t.erase(0, a);
    };
    trim(o);
    for (const char* art : {", The", ", A", ", An"}) {
        size_t n = std::string(art).size();
        size_t p = o.find(art);
        if (p != std::string::npos && (p + n == o.size() || o.compare(p + n, 3, " - ") == 0 || o.compare(p + n, 2, ": ") == 0)) {
            std::string rest = o.substr(p + n);
            o = std::string(art + 2) + " " + o.substr(0, p) + rest;
            break;
        }
    }
    for (size_t p; (p = o.find(" - ")) != std::string::npos;) o.replace(p, 3, ": ");
    trim(o);
    return o;
}

/* ============================ offline db ============================ */

size_t Db::load(const std::string& tsv) {
    rows_.clear();
    by_serial_.clear();
    by_title_.clear();
    std::istringstream in(tsv);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f = split(line, '\t');
        if (f.size() < 12) continue;
        Row r;
        r.sys = f[0];
        r.title = f[1];
        r.dev = f[3]; r.pub = f[4]; r.genre = f[5];
        r.year = to_int(f[6]); r.month = to_int(f[7]); r.players = to_int(f[8]);
        r.esrb = f[9]; r.famitsu = to_int(f[10]); r.edge = to_int(f[11]);
        size_t idx = rows_.size();
        rows_.push_back(r);
        for (const std::string& s : split(f[2], ','))
            if (!s.empty()) by_serial_.emplace(r.sys + "|" + normalize_serial(s), idx);
        by_title_.emplace(r.sys + "|" + normalize(r.title), idx);
    }
    return rows_.size();
}

Meta Db::to_meta(const Row& r, bool by_serial) const {
    Meta m;
    m.found = true;
    m.by_serial = by_serial;
    m.title = r.title; m.developer = r.dev; m.publisher = r.pub; m.genre = r.genre; m.esrb = r.esrb;
    m.year = r.year; m.month = r.month; m.players = r.players; m.famitsu = r.famitsu; m.edge = r.edge;
    return m;
}

Meta Db::lookup(const Query& q) const {
    std::vector<std::string> systems{q.system};
    if (q.system == "gbc") systems.push_back("gb");
    else if (q.system == "gb") systems.push_back("gbc");
    if (!q.serial.empty()) {
        std::string ns = normalize_serial(q.serial);
        for (const auto& s : systems) {
            auto it = by_serial_.find(s + "|" + ns);
            if (it != by_serial_.end()) return to_meta(rows_[it->second], true);
        }
    }
    for (const std::string& t : {q.alt_title, q.title, clean_title(q.title)}) {
        if (t.empty()) continue;
        std::string n = normalize(t);
        if (n.empty()) continue;
        for (const auto& s : systems) {
            auto it = by_title_.find(s + "|" + n);
            if (it != by_title_.end()) return to_meta(rows_[it->second], false);
        }
    }
    return Meta();
}

const Db& shared_db(const std::string& dir) {
    static Db db;
    static std::once_flag once;
    std::call_once(once, [&] {
        std::string path = dir + "gameinfo.tsv";
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            std::string t;
            char buf[65536];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) t.append(buf, n);
            std::fclose(f);
            db.load(t);
        }
    });
    return db;
}

/* ============================ online ============================ */

std::string platform_qid(const std::string& s) {
    if (s == "n64") return "Q184839";
    if (s == "gc") return "Q182172";
    if (s == "gba") return "Q188642";
    if (s == "gb") return "Q186437";
    if (s == "gbc") return "Q203992";
    if (s == "nds") return "Q170323";
    if (s == "3ds") return "Q203597";
    if (s == "wiiu") return "Q56942";
    if (s == "ps1") return "Q10677";
    if (s == "ps2") return "Q10680";
    return "";
}

std::string url_search(const std::string& text) {
    return "https://www.wikidata.org/w/api.php?action=wbsearchentities&search=" + url_enc(text) +
           "&language=en&uselang=en&type=item&limit=8&format=json";
}
std::string url_entities(const std::vector<std::string>& ids) {
    return "https://www.wikidata.org/w/api.php?action=wbgetentities&ids=" + join_ids(ids) +
           "&props=claims%7Csitelinks%7Clabels&languages=en&sitefilter=enwiki&format=json";
}
std::string url_labels(const std::vector<std::string>& ids) {
    return "https://www.wikidata.org/w/api.php?action=wbgetentities&ids=" + join_ids(ids) +
           "&props=labels&languages=en&format=json";
}
std::string url_summary(const std::string& wiki_title) {
    std::string t = wiki_title;
    for (auto& c : t) if (c == ' ') c = '_';
    return "https://en.wikipedia.org/api/rest_v1/page/summary/" + url_enc(t);
}

namespace {

const json::Value* path(const json::Value& v, std::initializer_list<const char*> keys) {
    const json::Value* p = &v;
    for (const char* k : keys) { p = p->get(k); if (!p) return nullptr; }
    return p;
}

/* Statements of one property: each has a datavalue (id string or raw value) and platform qualifiers (P400). */
struct Stmt { std::string id, str; std::vector<std::string> platforms, sources; };

std::vector<Stmt> statements(const json::Value& claims, const char* prop) {
    std::vector<Stmt> out;
    const json::Value* arr = claims.get(prop);
    if (!arr || !arr->is_array()) return out;
    for (const auto& c : arr->a) {
        Stmt s;
        const json::Value* dv = path(c, {"mainsnak", "datavalue", "value"});
        if (!dv) continue;
        if (dv->is_object()) s.id = dv->str("id"); else if (dv->is_string()) s.str = dv->s;
        if (dv->is_object() && s.id.empty()) s.str = dv->str("time");
        auto quals = [&](const char* qp, std::vector<std::string>* dst) {
            const json::Value* q = path(c, {"qualifiers", qp});
            if (!q || !q->is_array()) return;
            for (const auto& x : q->a) {
                const json::Value* v = path(x, {"datavalue", "value"});
                if (v && v->is_object()) dst->push_back(v->str("id"));
            }
        };
        quals("P400", &s.platforms);
        quals("P447", &s.sources);
        out.push_back(s);
    }
    return out;
}

/* Statements that apply to our platform: qualified for it, else unqualified. Statements qualified for another platform are dropped. */
std::vector<Stmt> for_platform(const std::vector<Stmt>& v, const std::string& plat) {
    std::vector<Stmt> exact, plain;
    for (const auto& s : v) {
        if (s.platforms.empty()) plain.push_back(s);
        else if (std::find(s.platforms.begin(), s.platforms.end(), plat) != s.platforms.end()) exact.push_back(s);
    }
    return exact.empty() ? plain : exact;
}

int year_of(const std::string& t) {   // "+2001-11-24T00:00:00Z"
    size_t i = 0;
    while (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
    return std::atoi(t.c_str() + i);
}

std::string cap(std::string s) {
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

std::string esrb_letter(const std::string& label) {
    std::string l = lower_ascii(label);
    if (l == "everyone") return "E";
    if (l.rfind("everyone 10", 0) == 0) return "E10+";
    if (l == "teen") return "T";
    if (l.rfind("mature", 0) == 0) return "M";
    if (l.rfind("adults only", 0) == 0) return "AO";
    if (l == "early childhood") return "EC";
    if (l == "rating pending") return "";
    return label;
}

std::string short_score(std::string v) {
    if (v.size() > 4 && v.compare(v.size() - 4, 4, "/100") == 0) v.resize(v.size() - 4);
    return v;
}

bool get_json(const Fetcher& f, const std::string& url, json::Value* out, bool* net_err) {
    std::string body;
    int st = 0;
    if (!f(url, &body, &st) || st == 429 || st >= 500 || st == 0) { *net_err = true; return false; }
    if (st != 200) return false;
    return json::parse(body, out);
}

}  // namespace

ResolveResult resolve_online(const Query& q, const Fetcher& fetch, Online* out) {
    *out = Online();
    const std::string plat = platform_qid(q.system);
    if (plat.empty()) { out->status = Online::NotFound; return ResolveResult::NotFound; }

    std::vector<std::string> wants;
    for (const std::string& t : {q.alt_title, clean_title(q.title)}) {
        if (t.empty()) continue;
        std::string n = normalize(t);
        if (n.empty()) continue;
        bool dup = false;
        for (const auto& w : wants) if (normalize(w) == n) dup = true;
        if (!dup) wants.push_back(t);
    }
    bool net_err = false;
    std::string qid, label;
    json::Value ent;   // the matched entity
    for (const std::string& want : wants) {
        std::string wn = normalize(want);
        json::Value sr;
        if (!get_json(fetch, url_search(want), &sr, &net_err)) {
            if (net_err) return ResolveResult::NetworkError;
            continue;
        }
        std::vector<std::string> cands;
        const json::Value* arr = sr.get("search");
        if (arr && arr->is_array())
            for (const auto& s : arr->a) {
                if (normalize(s.str("label")) != wn && normalize(path(s, {"match", "text"}) ? path(s, {"match", "text"})->s : "") != wn) continue;
                std::string id = s.str("id");
                if (!id.empty() && cands.size() < 5) cands.push_back(id);
            }
        if (cands.empty()) continue;
        json::Value er;
        if (!get_json(fetch, url_entities(cands), &er, &net_err)) {
            if (net_err) return ResolveResult::NetworkError;
            continue;
        }
        const json::Value* ents = er.get("entities");
        if (!ents) continue;
        for (const std::string& id : cands) {   // search order = relevance
            const json::Value* e = ents->get(id);
            const json::Value* cl = e ? e->get("claims") : nullptr;
            if (!cl) continue;
            bool game = false, onplat = false;
            for (const auto& s : statements(*cl, "P31")) if (s.id == "Q7889") game = true;
            for (const auto& s : statements(*cl, "P400")) if (s.id == plat) onplat = true;
            if (game && onplat) { qid = id; ent = *e; break; }
        }
        if (!qid.empty()) break;
    }
    if (qid.empty()) { out->status = Online::NotFound; return ResolveResult::NotFound; }

    out->qid = qid;
    if (const json::Value* l = path(ent, {"labels", "en", "value"})) out->label = l->s;
    std::string wiki_title;
    if (const json::Value* t = path(ent, {"sitelinks", "enwiki", "title"})) wiki_title = t->s;
    const json::Value& cl = *ent.get("claims");

    // Statements we need labels for.
    std::set<std::string> need;
    auto first_ids = [&](const char* prop, size_t n) {
        std::vector<std::string> v;
        for (const auto& s : statements(cl, prop)) if (!s.id.empty() && v.size() < n) { v.push_back(s.id); need.insert(s.id); }
        return v;
    };
    std::vector<std::string> dev = first_ids("P178", 2), pub = first_ids("P123", 2), gen = first_ids("P136", 2);
    auto esrb = for_platform(statements(cl, "P852"), plat), pegi = for_platform(statements(cl, "P908"), plat);
    std::string esrb_id = esrb.empty() ? "" : esrb[0].id, pegi_id = pegi.empty() ? "" : pegi[0].id;
    if (!esrb_id.empty()) need.insert(esrb_id);
    if (!pegi_id.empty()) need.insert(pegi_id);
    struct Score { std::string value, src; };
    std::vector<Score> scores;
    for (const auto& s : for_platform(statements(cl, "P444"), plat)) {
        if (s.str.empty()) continue;
        Score sc{short_score(s.str), s.sources.empty() ? "" : s.sources[0]};
        if (!sc.src.empty()) need.insert(sc.src);
        scores.push_back(sc);
    }
    for (const auto& s : statements(cl, "P577")) {
        int y = year_of(s.str);
        if (y > 1970 && (out->year == 0 || y < out->year)) out->year = y;
    }
    std::map<std::string, std::string> names;
    if (!need.empty()) {
        std::vector<std::string> ids(need.begin(), need.end());
        if (ids.size() > 50) ids.resize(50);
        json::Value lr;
        if (!get_json(fetch, url_labels(ids), &lr, &net_err)) { if (net_err) return ResolveResult::NetworkError; }
        else if (const json::Value* es = lr.get("entities"))
            for (const auto& kv : es->o) if (const json::Value* v = path(kv.second, {"labels", "en", "value"})) names[kv.first] = v->s;
    }
    auto nm = [&](const std::string& id) { auto it = names.find(id); return it == names.end() ? std::string() : it->second; };
    auto join = [&](const std::vector<std::string>& ids) {
        std::string o;
        for (const auto& id : ids) { std::string n = nm(id); if (n.empty()) continue; if (!o.empty()) o += ", "; o += n; }
        return o;
    };
    out->developer = join(dev);
    out->publisher = join(pub);
    out->genre = cap(join(gen));
    if (!esrb_id.empty()) out->esrb = esrb_letter(nm(esrb_id));
    if (!pegi_id.empty()) out->pegi = nm(pegi_id);
    for (const auto& sc : scores) out->ratings.push_back({sc.src.empty() ? "Score" : nm(sc.src), sc.value});
    out->ratings.erase(std::remove_if(out->ratings.begin(), out->ratings.end(), [](const Rating& r) { return r.source.empty(); }), out->ratings.end());

    if (!wiki_title.empty()) {
        std::string body;
        int st = 0;
        if (!fetch(url_summary(wiki_title), &body, &st) || st == 429 || st >= 500) return ResolveResult::NetworkError;
        json::Value sm;
        if (st == 200 && json::parse(body, &sm) && sm.str("type") == "standard") {
            std::string item = sm.str("wikibase_item");
            if (item.empty() || item == qid) {   // the page must be the same Wikidata item
                out->description = sm.str("extract");
                if (const json::Value* u = path(sm, {"content_urls", "desktop", "page"})) out->page_url = u->s;
            }
        }
    }
    out->status = Online::Ok;
    return ResolveResult::Ok;
}

std::string cache_name(const Query& q) {
    std::string n = normalize(q.alt_title.empty() ? clean_title(q.title) : q.alt_title);
    if (n.size() > 80) n.resize(80);
    if (n.empty()) n = "untitled";
    return q.system + "_" + n + ".json";
}

std::string cache_to_json(const Online& o) {
    json::Value v = json::make_object();
    auto put = [&](const char* k, json::Value x) { v.o.emplace_back(k, std::move(x)); };
    put("v", json::make_int(1));
    put("status", json::make_string(o.status == Online::Ok ? "ok" : o.status == Online::NotFound ? "none" : "unknown"));
    put("fetched", json::make_int(o.fetched));
    put("qid", json::make_string(o.qid));
    put("label", json::make_string(o.label));
    put("description", json::make_string(o.description));
    put("url", json::make_string(o.page_url));
    put("esrb", json::make_string(o.esrb));
    put("pegi", json::make_string(o.pegi));
    put("developer", json::make_string(o.developer));
    put("publisher", json::make_string(o.publisher));
    put("genre", json::make_string(o.genre));
    put("year", json::make_int(o.year));
    json::Value ra = json::make_array();
    for (const auto& r : o.ratings) {
        json::Value p = json::make_array();
        p.a.push_back(json::make_string(r.source));
        p.a.push_back(json::make_string(r.text));
        ra.a.push_back(p);
    }
    put("ratings", ra);
    return json::stringify(v);
}

bool cache_from_json(const std::string& text, Online* o) {
    json::Value v;
    if (!json::parse(text, &v) || !v.is_object() || v.i64("v") != 1) return false;
    *o = Online();
    std::string st = v.str("status");
    o->status = st == "ok" ? Online::Ok : st == "none" ? Online::NotFound : Online::Unknown;
    o->fetched = v.i64("fetched");
    o->qid = v.str("qid"); o->label = v.str("label"); o->description = v.str("description"); o->page_url = v.str("url");
    o->esrb = v.str("esrb"); o->pegi = v.str("pegi"); o->developer = v.str("developer"); o->publisher = v.str("publisher");
    o->genre = v.str("genre"); o->year = (int)v.i64("year");
    if (const json::Value* ra = v.get("ratings"); ra && ra->is_array())
        for (const auto& p : ra->a) if (p.is_array() && p.a.size() == 2) o->ratings.push_back({p.a[0].s, p.a[1].s});
    return true;
}

bool cache_stale(const Online& o, int64_t now) {
    const int64_t day = 86400;
    if (o.status == Online::Ok) return now - o.fetched > 90 * day;
    if (o.status == Online::NotFound) return now - o.fetched > 7 * day;
    return true;
}

/* ============================ presentation ============================ */

std::vector<Chip> rating_chips(const Meta& m, const Online& o, size_t max) {
    std::vector<Chip> all;
    for (const auto& r : o.ratings) all.push_back({r.source, r.text});
    auto has = [&](const char* s) { for (const auto& c : all) if (lower_ascii(c.label) == s) return true; return false; };
    if (m.famitsu > 0 && !has("famitsu")) all.push_back({"Famitsu", std::to_string(m.famitsu) + "/40"});
    if (m.edge > 0 && !has("edge")) all.push_back({"Edge", std::to_string(m.edge) + "/10"});
    static const char* order[] = {"metacritic", "gamerankings", "famitsu", "edge", "ign", "gamespot"};
    auto rank = [&](const Chip& c) {
        std::string l = lower_ascii(c.label);
        for (size_t i = 0; i < sizeof order / sizeof *order; ++i) if (l == order[i]) return (int)i;
        return 100;
    };
    std::stable_sort(all.begin(), all.end(), [&](const Chip& a, const Chip& b) { return rank(a) < rank(b); });
    if (all.size() > max) all.resize(max);
    return all;
}

std::string age_badge(const Meta& m, const Online& o) {
    std::string e = !m.esrb.empty() ? m.esrb : o.esrb;
    if (!e.empty()) return "ESRB " + e;
    if (!o.pegi.empty()) return o.pegi;
    return "";
}

std::string players_text(int n) {
    if (n <= 0) return "";
    return n == 1 ? "1 player" : "1-" + std::to_string(n) + " players";
}

std::string release_text(const Meta& m, const Online& o) {
    static const char* mon[] = {"", "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    int y = m.year ? m.year : o.year;
    if (!y) return "";
    if (m.year && m.month >= 1 && m.month <= 12) return std::string(mon[m.month]) + " " + std::to_string(y);
    return std::to_string(y);
}

} // namespace gameinfo
