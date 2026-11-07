#include "cases_audit.h"

#include "art_match.h"
#include "gametdb.h"
#include "json.h"
#include "winutil.h"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace hubui {
namespace cases_audit {

std::string source_of_art_name(const std::string& n) {
    if (n.empty()) return "";
    if (n.compare(0, 8, "gametdb:") == 0) return "GameTDB";
    if (n.compare(0, 8, "nupicon:") == 0) return "icon";
    return "libretro";
}

bool console_has_scan(const std::string& sys) { return gametdb::has_fullcover(sys); }

Row make_row(int index, const Item& it, const hubart::SlotInfo& front, const hubart::SlotInfo& full) {
    Row r;
    r.index = index;
    r.section = it.section;
    r.sys = it.sys;
    r.title = it.title;
    r.key = it.key;
    r.cid = it.cid;
    const bool scan = console_has_scan(it.ghost ? hubart_match::system_of_key(it.sys, it.key) : it.sys);   // a PS2 disc filed under GameCube has no GameTDB scan
    r.full = !scan ? "n/a" : full.state == hubart::SlotState::Ready ? "yes" : "no";
    if (r.full == "yes") { r.spine = "real"; r.back = "real"; }
    if (front.state == hubart::SlotState::Ready) { r.art_name = front.name; r.front = source_of_art_name(front.name); if (r.front.empty()) r.front = "libretro"; }
    else if (front.state == hubart::SlotState::Failed) r.front = "GENERATED";
    else r.front = "PENDING";
    if (r.full == "yes") {
        r.front = "scan";
        if (r.art_name.empty()) r.art_name = full.name;
    }
    if (front.state == hubart::SlotState::Failed) r.note = "front: " + front.err + (front.transient ? " (transient)" : "");
    if (scan && full.state == hubart::SlotState::Failed) r.note += (r.note.empty() ? "" : "; ") + std::string("full: ") + full.err + (full.transient ? " (transient)" : "");
    else if (scan && full.state == hubart::SlotState::Pending) r.note += (r.note.empty() ? "" : "; ") + std::string("full: pending");
    return r;
}

namespace {
std::string clean(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return o;
}
}

std::string tsv_header() { return "index\tsection\tsys\ttitle\tkey\tcatalog_id\tfront\tfull\tspine\tback\tart_name\tnote\n"; }

std::string tsv_row(const Row& r) {
    return std::to_string(r.index) + "\t" + clean(r.section) + "\t" + clean(r.sys) + "\t" + clean(r.title) + "\t" + clean(r.key) + "\t" + clean(r.cid) + "\t" +
           clean(r.front) + "\t" + clean(r.full) + "\t" + clean(r.spine) + "\t" + clean(r.back) + "\t" + clean(r.art_name) + "\t" + clean(r.note) + "\n";
}

std::string summary(const std::vector<Row>& rows) {
    std::map<std::string, int> sec, front;
    int fy = 0, fn = 0, sr = 0, br = 0;
    for (const Row& r : rows) {
        ++sec[r.section]; ++front[r.front];
        if (r.full == "yes") ++fy; else if (r.full == "no") ++fn;
        if (r.spine == "real") ++sr;
        if (r.back == "real") ++br;
    }
    std::string s = "total=" + std::to_string(rows.size()) + " own=" + std::to_string(sec["own"]) + " friend=" + std::to_string(sec["friend"]) + " pack=" + std::to_string(sec["pack"]);
    s += " | front: scan=" + std::to_string(front["scan"]) + " GameTDB=" + std::to_string(front["GameTDB"]) + " libretro=" + std::to_string(front["libretro"]) +
         " icon=" + std::to_string(front["icon"]) + " GENERATED=" + std::to_string(front["GENERATED"]) + " PENDING=" + std::to_string(front["PENDING"]);
    s += " | full yes=" + std::to_string(fy) + " no=" + std::to_string(fn) + " | spine real=" + std::to_string(sr) + " | back real=" + std::to_string(br);
    return s;
}

std::vector<Item> items_for(const std::vector<hub::GameEntry>& lib, const std::vector<fg::Ghost>& ghosts) {
    std::vector<Item> v;
    v.reserve(lib.size() + ghosts.size());
    for (const hub::GameEntry& e : lib) {
        Item it;
        it.section = "own"; it.sys = hub::system_short(e.system); it.title = e.title; it.key = e.key; it.cid = e.catalog_id; it.entry = e;
        v.push_back(std::move(it));
    }
    for (const fg::Ghost& g : ghosts) {
        Item it;
        it.section = g.uids.empty() ? "pack" : "friend"; it.sys = g.sys; it.title = g.title; it.key = g.key; it.cid = g.cid; it.hint = g.hint; it.ghost = true;
        v.push_back(std::move(it));
    }
    return v;
}

std::vector<std::pair<std::string, std::vector<hubnet::LibGame>>> parse_friend_libs(const std::string& text) {
    std::vector<std::pair<std::string, std::vector<hubnet::LibGame>>> out;
    json::Value j;
    if (!json::parse(text, &j) || !j.is_object()) return out;
    const json::Value* libs = j.get("libs");
    if (!libs || !libs->is_object()) return out;
    for (const auto& kv : libs->o) {
        const json::Value* games = kv.second.get("games");
        if (!games || !games->is_array()) continue;
        std::vector<hubnet::LibGame> v;
        for (const json::Value& g : games->a) {
            hubnet::LibGame x;
            x.sys = g.str("s"); x.key = g.str("k"); x.cid = g.str("c"); x.title = g.str("t");
            if (!x.sys.empty() && !x.key.empty()) v.push_back(std::move(x));
        }
        if (!v.empty()) out.push_back({kv.first, std::move(v)});
    }
    return out;
}

const std::vector<std::pair<std::string, const std::vector<hubnet::LibGame>*>>& injected_libs() {
    static std::vector<std::pair<std::string, std::vector<hubnet::LibGame>>> store;
    static std::vector<std::pair<std::string, const std::vector<hubnet::LibGame>*>> ptrs;
    static bool done = false;
    if (!done) {
        done = true;
        const char* p = std::getenv("HUB_CASES_AUDIT_FRIENDS");
        std::string text;
        if (p && *p && std::getenv("HUB_SCREENSHOT") && winutil::read_file(p, &text)) {
            store = parse_friend_libs(text);
            for (auto& s : store) ptrs.push_back({s.first, &s.second});
        }
    }
    return ptrs;
}

std::vector<PackRow> parse_pack_tsv(const std::string& text) {
    std::vector<PackRow> v;
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string line = text.substr(a, b - a);
        a = b + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        size_t p = 0;
        for (;;) { size_t q = line.find('\t', p); if (q == std::string::npos) { f.push_back(line.substr(p)); break; } f.push_back(line.substr(p, q - p)); p = q + 1; }
        if (f.size() < 3 || f[0].empty() || f[0][0] == '#' || f[1].empty() || f[2].empty()) continue;
        v.push_back({f[0], f[1], f[2], f.size() > 3 ? f[3] : std::string()});
    }
    return v;
}

std::string pack_tsv(const std::vector<PackRow>& rows) {
    std::string o = "# sys\ttitle\tkey\thint\n";
    for (const PackRow& r : rows) o += clean(r.sys) + "\t" + clean(r.title) + "\t" + clean(r.key) + "\t" + clean(r.hint) + "\n";
    return o;
}

const std::vector<PackRow>& injected_pack() {
    static std::vector<PackRow> rows;
    static bool done = false;
    if (!done) {
        done = true;
        const char* p = std::getenv("HUB_CASES_AUDIT_PACK");
        std::string text;
        if (p && *p && std::getenv("HUB_SCREENSHOT") && winutil::read_file(p, &text)) rows = parse_pack_tsv(text);
    }
    return rows;
}

namespace {
struct State {
    bool inited = false, finished = false;
    std::string out;
    std::vector<Item> items;
    std::vector<Row> rows;
    std::vector<char> final_front, final_full;
    std::vector<hubart::SlotInfo> front, full;
    std::chrono::steady_clock::time_point t0;
    double limit_s = 150.0;
    int frames = 0;
};
State S;

hubart::SlotInfo poll(const Item& it, hubart::Kind kind) {
    int w = 0, h = 0;
    if (it.ghost) {
        hubart::get_ghost(it.sys, it.key, it.cid, it.title, kind, &w, &h, it.hint);
        return hubart::slot_info(hubart::ghost_slot_id(it.sys, it.key, it.title, kind));
    }
    hubart::get(it.entry, kind, &w, &h);
    return hubart::slot_info(hubart::game_slot_id(it.entry.key, kind));
}
}

void tick(const std::vector<hub::GameEntry>& lib, const std::vector<fg::Ghost>& ghosts, bool scanning) {
    if (S.finished) return;
    if (!S.inited) {
        const char* o = std::getenv("HUB_CASES_AUDIT");
        if (!o || !*o || !std::getenv("HUB_SCREENSHOT")) { S.finished = true; return; }
        // wait for the library scan (an empty list at start-up is not the library)
        static const auto first = std::chrono::steady_clock::now();
        const bool waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count() > 40.0;
        if ((scanning || lib.empty()) && !waited) return;   // the library scan has not landed yet (an empty library is only believed after 40 s)
        S.inited = true;
        S.out = o;
        if (const char* l = std::getenv("HUB_CASES_AUDIT_SECS")) S.limit_s = std::max(5.0, std::atof(l));
        S.items = items_for(lib, ghosts);
        S.front.assign(S.items.size(), {});
        S.full.assign(S.items.size(), {});
        S.final_front.assign(S.items.size(), 0);
        S.final_full.assign(S.items.size(), 0);
        S.t0 = std::chrono::steady_clock::now();
    }
    ++S.frames;
    if (S.frames < 3) return;
    const bool late = std::chrono::duration<double>(std::chrono::steady_clock::now() - S.t0).count() > S.limit_s;
    bool all = true;
    int requested = 0;
    for (size_t i = 0; i < S.items.size(); ++i) {
        const Item& it = S.items[i];
        if (!S.final_front[i]) {
            if (requested < 24) {   // a few at a time: the worker is one thread and the LRU holds 400 textures
                ++requested;
                hubart::SlotInfo s = poll(it, hubart::Kind::Boxart);
                S.front[i] = s;
                if (s.state == hubart::SlotState::Ready || (s.state == hubart::SlotState::Failed && (!s.transient || late))) S.final_front[i] = 1;
            }
            if (!S.final_front[i]) all = false;
        }
        if (!S.final_full[i]) {
            if (!console_has_scan(it.ghost ? hubart_match::system_of_key(it.sys, it.key) : it.sys)) S.final_full[i] = 1;
            else {
                if (requested < 24) {
                    ++requested;
                    hubart::SlotInfo s = poll(it, hubart::Kind::FullCover);
                    S.full[i] = s;
                    if (s.state == hubart::SlotState::Ready || (s.state == hubart::SlotState::Failed && (!s.transient || late))) S.final_full[i] = 1;
                }
                if (!S.final_full[i]) all = false;
            }
        }
    }
    if (!all && !late) return;
    std::string text = tsv_header();
    std::vector<Row> rows;
    for (size_t i = 0; i < S.items.size(); ++i) {
        rows.push_back(make_row((int)i, S.items[i], S.front[i], S.full[i]));
        text += tsv_row(rows.back());
    }
    winutil::write_file_atomic(S.out, text);
    std::fprintf(stderr, "cases audit: %s\n", summary(rows).c_str());
    S.finished = true;
    SDL_Event q{};
    q.type = SDL_QUIT;
    SDL_PushEvent(&q);
}

} // namespace cases_audit
} // namespace hubui
