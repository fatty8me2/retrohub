#include "logic.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hubui {

/* ---------------- JSON ---------------- */

const Json* Json::get(const std::string& key) const {
    if (type != Obj) return nullptr;
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return &vals[i];
    return nullptr;
}
std::string Json::str(const std::string& key, const std::string& def) const {
    const Json* j = get(key);
    return (j && j->type == Str) ? j->s : def;
}
double Json::num(const std::string& key, double def) const {
    const Json* j = get(key);
    return (j && j->type == Num) ? j->n : def;
}

namespace {

struct Parser {
    const std::string& t;
    size_t p = 0;
    explicit Parser(const std::string& s) : t(s) {}

    void ws() { while (p < t.size() && std::isspace((unsigned char)t[p])) ++p; }

    static void put_utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    }
    bool hex4(unsigned* v) {
        if (p + 4 > t.size()) return false;
        unsigned r = 0;
        for (int i = 0; i < 4; ++i) {
            char c = t[p++];
            r <<= 4;
            if (c >= '0' && c <= '9') r |= c - '0';
            else if (c >= 'a' && c <= 'f') r |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') r |= c - 'A' + 10;
            else return false;
        }
        *v = r;
        return true;
    }
    bool string(std::string* o) {
        if (p >= t.size() || t[p] != '"') return false;
        ++p;
        o->clear();
        while (p < t.size()) {
            char c = t[p++];
            if (c == '"') return true;
            if (c != '\\') { *o += c; continue; }
            if (p >= t.size()) return false;
            char e = t[p++];
            switch (e) {
                case '"': *o += '"'; break;
                case '\\': *o += '\\'; break;
                case '/': *o += '/'; break;
                case 'b': *o += '\b'; break;
                case 'f': *o += '\f'; break;
                case 'n': *o += '\n'; break;
                case 'r': *o += '\r'; break;
                case 't': *o += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(&cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                        p += 2;
                        unsigned lo;
                        if (!hex4(&lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(*o, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(Json* j, int depth) {
        if (depth > 16) return false;
        ws();
        if (p >= t.size()) return false;
        char c = t[p];
        if (c == '{') {
            ++p;
            j->type = Json::Obj;
            ws();
            if (p < t.size() && t[p] == '}') { ++p; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!string(&k)) return false;
                ws();
                if (p >= t.size() || t[p++] != ':') return false;
                Json v;
                if (!value(&v, depth + 1)) return false;
                j->keys.push_back(k);
                j->vals.push_back(std::move(v));
                ws();
                if (p >= t.size()) return false;
                char d = t[p++];
                if (d == '}') return true;
                if (d != ',') return false;
            }
        }
        if (c == '[') {
            ++p;
            j->type = Json::Arr;
            ws();
            if (p < t.size() && t[p] == ']') { ++p; return true; }
            for (;;) {
                Json v;
                if (!value(&v, depth + 1)) return false;
                j->vals.push_back(std::move(v));
                ws();
                if (p >= t.size()) return false;
                char d = t[p++];
                if (d == ']') return true;
                if (d != ',') return false;
            }
        }
        if (c == '"') { j->type = Json::Str; return string(&j->s); }
        if (t.compare(p, 4, "true") == 0) { p += 4; j->type = Json::Bool; j->b = true; return true; }
        if (t.compare(p, 5, "false") == 0) { p += 5; j->type = Json::Bool; j->b = false; return true; }
        if (t.compare(p, 4, "null") == 0) { p += 4; j->type = Json::Null; return true; }
        const char* b = t.c_str() + p;
        char* e = nullptr;
        double d = std::strtod(b, &e);
        if (e == b) return false;
        p += (size_t)(e - b);
        j->type = Json::Num;
        j->n = d;
        return true;
    }
};

} // namespace

bool json_parse(const std::string& text, Json* out) {
    Parser ps(text);
    Json j;
    if (!ps.value(&j, 0)) return false;
    ps.ws();
    if (ps.p != text.size()) return false;
    *out = std::move(j);
    return true;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

/* ---------------- settings ---------------- */

std::string settings_to_json(const Settings& s) {
    std::ostringstream o;
    o << "{\n"
      << "  \"name\": \"" << json_escape(s.name) << "\",\n"
      << "  \"rom_folders\": [";
    for (size_t i = 0; i < s.rom_folders.size(); ++i)
        o << (i ? ", " : "") << "\"" << json_escape(s.rom_folders[i]) << "\"";
    o << "],\n"
      << "  \"server\": \"" << json_escape(s.server) << "\",\n"
      << "  \"input_delay\": " << s.input_delay << ",\n"
      << "  \"delay_auto\": " << (s.delay_auto ? "true" : "false") << ",\n"
      << "  \"last_game\": \"" << json_escape(s.last_game) << "\",\n"
      << "  \"smash_exe\": \"" << json_escape(s.smash_exe) << "\",\n"
      << "  \"gb_core\": \"" << json_escape(s.gb_core) << "\",\n"
      << "  \"fullscreen\": " << (s.fullscreen ? "true" : "false") << ",\n"
      << "  \"sync_folder\": \"" << json_escape(s.sync_folder) << "\",\n"
      << "  \"sync_auto\": " << (s.sync_auto ? "true" : "false") << ",\n"
      << "  \"sync_states\": " << (s.sync_states ? "true" : "false") << ",\n"
      << "  \"sync_card_dismissed\": " << (s.sync_card_dismissed ? "true" : "false") << ",\n"
      << "  \"vault_dir\": \"" << json_escape(s.vault_dir) << "\",\n"
      << "  \"download_art\": " << (s.download_art ? "true" : "false") << ",\n"
      << "  \"sidebar_mode\": " << s.sidebar_mode << ",\n"
      << "  \"show_friends\": " << (s.show_friends ? "true" : "false") << ",\n"
      << "  \"appear_offline\": " << (s.appear_offline ? "true" : "false") << ",\n"
      << "  \"notif_sound\": " << (s.notif_sound ? "true" : "false") << "\n"
      << "}\n";
    return o.str();
}

static bool json_bool(const Json& j, const char* key, bool def) {
    const Json* v = j.get(key);
    return (v && v->type == Json::Bool) ? v->b : def;
}

bool settings_from_json(const std::string& text, Settings* out) {
    Json j;
    if (!json_parse(text, &j) || j.type != Json::Obj) return false;
    out->name = j.str("name", out->name);
    const Json* rf = j.get("rom_folders");
    if (rf && rf->type == Json::Arr) {
        out->rom_folders.clear();
        for (auto& v : rf->vals)
            if (v.type == Json::Str && !v.s.empty() &&
                std::find(out->rom_folders.begin(), out->rom_folders.end(), v.s) == out->rom_folders.end())
                out->rom_folders.push_back(v.s);
    } else {
        /* migrate the old single-folder setting */
        std::string old = j.str("rom_folder");
        if (!old.empty() && std::find(out->rom_folders.begin(), out->rom_folders.end(), old) == out->rom_folders.end())
            out->rom_folders.push_back(old);
    }
    out->server = j.str("server", out->server);
    out->input_delay = (int)j.num("input_delay", out->input_delay);
    out->input_delay = std::max(1, std::min(8, out->input_delay));
    if (const Json* da = j.get("delay_auto")) if (da->type == Json::Bool) out->delay_auto = da->b;
    out->last_game = j.str("last_game", out->last_game);
    out->smash_exe = j.str("smash_exe", out->smash_exe);
    std::string core = j.str("gb_core", out->gb_core);
    out->gb_core = (core == "gambatte" || core == "sameboy") ? core : "sameboy";
    out->fullscreen = json_bool(j, "fullscreen", out->fullscreen);
    out->sync_folder = j.str("sync_folder", out->sync_folder);
    out->sync_auto = json_bool(j, "sync_auto", out->sync_auto);
    out->sync_states = json_bool(j, "sync_states", out->sync_states);
    out->sync_card_dismissed = json_bool(j, "sync_card_dismissed", out->sync_card_dismissed);
    out->vault_dir = j.str("vault_dir", out->vault_dir);
    out->download_art = json_bool(j, "download_art", out->download_art);
    out->sidebar_mode = std::max(0, std::min(2, (int)j.num("sidebar_mode", out->sidebar_mode)));
    out->show_friends = json_bool(j, "show_friends", out->show_friends);
    out->appear_offline = json_bool(j, "appear_offline", out->appear_offline);
    out->notif_sound = json_bool(j, "notif_sound", out->notif_sound);
    return true;
}

bool load_settings(const std::string& path, Settings* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return settings_from_json(ss.str(), out);
}

bool save_settings(const std::string& path, const Settings& s) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << settings_to_json(s);
        if (!f.good()) return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

/* ---------------- lobby rules ---------------- */

Selectability game_selectable(const std::string& game_id, int max_players,
                              const std::vector<PeerInfo>& peers, int host_slot) {
    Selectability r;
    if ((int)peers.size() > max_players) { r.reason = "too many players (max " + std::to_string(max_players) + ")"; return r; }
    /* ownership token: the catalog md5 if advertised, else the key itself */
    auto token = [&](const PeerInfo& p, std::string* t) {
        auto it = p.games.find(game_id);
        if (it != p.games.end()) { *t = it->second; return true; }
        if (std::find(p.keys.begin(), p.keys.end(), game_id) != p.keys.end()) { *t = game_id; return true; }
        return false;
    };
    const PeerInfo* host = nullptr;
    for (auto& p : peers) if (p.slot == host_slot) host = &p;
    std::string t;
    for (auto& p : peers) {
        if (!p.hello) { r.reason = "waiting for " + p.name; return r; }
        if (!token(p, &t)) { r.reason = p.name + " doesn't have this game"; return r; }
    }
    if (host) {
        std::string ref;
        token(*host, &ref);
        for (auto& p : peers) {
            token(p, &t);
            if (t != ref) { r.reason = "different ROM version (" + p.name + ")"; return r; }
        }
    }
    r.ok = true;
    return r;
}

std::string hello_json(const std::string& version, const std::map<std::string, std::string>& games,
                       const std::vector<std::string>& keys) {
    std::string o = "{\"kind\":\"hello\",\"version\":\"" + json_escape(version) + "\",\"games\":{";
    bool first = true;
    for (auto& g : games) {
        if (!first) o += ",";
        first = false;
        o += "\"" + json_escape(g.first) + "\":\"" + json_escape(g.second) + "\"";
    }
    o += "},\"keys\":[";
    for (size_t i = 0; i < keys.size(); ++i) o += (i ? ",\"" : "\"") + json_escape(keys[i]) + "\"";
    return o + "]}";
}

std::vector<std::string> hello_messages(const std::string& version, const std::map<std::string, std::string>& games,
                                        const std::vector<std::string>& keys, size_t max_bytes) {
    std::vector<std::string> out;
    size_t i = 0;
    int part = 0;
    do {
        std::string o = "{\"kind\":\"hello\",\"version\":\"" + json_escape(version) + "\",\"part\":" + std::to_string(part);
        if (part == 0) {
            o += ",\"games\":{";
            bool f = true;
            for (auto& g : games) {
                if (!f) o += ",";
                f = false;
                o += "\"" + json_escape(g.first) + "\":\"" + json_escape(g.second) + "\"";
            }
            o += "}";
        }
        o += ",\"keys\":[";
        bool first = true;
        while (i < keys.size()) {
            std::string k = "\"" + json_escape(keys[i]) + "\"";
            if (!first && o.size() + k.size() + 3 > max_bytes) break;
            o += (first ? "" : ",") + k;
            first = false;
            ++i;
        }
        out.push_back(o + "]}");
        ++part;
    } while (i < keys.size());
    return out;
}

bool parse_hello(const Json& body, PeerInfo* out) {
    if (body.str("kind") != "hello") return false;
    bool cont = body.num("part", 0) > 0;   // continuation of a long key list
    if (cont) {
        const Json* k = body.get("keys");
        if (k && k->type == Json::Arr)
            for (auto& v : k->vals)
                if (v.type == Json::Str) out->keys.push_back(v.s);
        return true;
    }
    const Json* g = body.get("games");
    out->games.clear();
    out->keys.clear();
    if (g && g->type == Json::Obj)
        for (size_t i = 0; i < g->keys.size(); ++i)
            if (g->vals[i].type == Json::Str) out->games[g->keys[i]] = g->vals[i].s;
    const Json* k = body.get("keys");
    if (k && k->type == Json::Arr)
        for (auto& v : k->vals)
            if (v.type == Json::Str) out->keys.push_back(v.s);
    out->hello = true;
    return true;
}

std::string launch_json(const std::string& game, int launch, int delay, unsigned seed, const std::string& core, bool share_host_save,
                        const PortPlan* plan) {
    std::string o = "{\"kind\":\"launch\",\"game\":\"" + json_escape(game) + "\",\"launch\":" + std::to_string(launch) +
                    ",\"delay\":" + std::to_string(delay) + ",\"seed\":" + std::to_string(seed);
    if (!core.empty()) o += ",\"core\":\"" + json_escape(core) + "\"";
    o += std::string(",\"share_save\":") + (share_host_save ? "true" : "false");
    if (plan) o += ",\"ports\":[" + plan->ports + "],\"players\":" + std::to_string(plan->players);
    return o + "}";
}

bool parse_launch_plan(const Json& m, std::string* ports, int* players) {
    const Json* pa = m.get("ports");
    const Json* pl = m.get("players");
    if (!pa || pa->type != Json::Arr || pa->vals.size() != 4 || !pl || pl->type != Json::Num) return false;
    int owner[4];
    for (int i = 0; i < 4; ++i) {
        const Json& v = pa->vals[(size_t)i];
        if (v.type != Json::Num || v.n != (double)(int)v.n || v.n < -1 || v.n > 3) return false;
        owner[i] = (int)v.n;
    }
    bool seen[4] = {false, false, false, false};
    int distinct = 0;
    bool gap = false;
    for (int i = 0; i < 4; ++i) {
        if (owner[i] < 0) { gap = true; continue; }
        if (gap) return false;   // owners are packed first
        if (!seen[owner[i]]) { seen[owner[i]] = true; ++distinct; }
    }
    if (distinct < 1 || pl->n != (double)distinct) return false;
    // each owner's ports must be consecutive (build_ports never interleaves)
    for (int i = 1; i < 4; ++i)
        if (owner[i] >= 0 && owner[i] != owner[i - 1])
            for (int k = 0; k < i - 1; ++k) if (owner[k] == owner[i]) return false;
    if (ports) {
        ports->clear();
        for (int i = 0; i < 4; ++i) *ports += (i ? "," : "") + std::to_string(owner[i]);
    }
    if (players) *players = distinct;
    return true;
}

std::string delay_arg(int delay) { return delay <= 0 ? " --delay auto" : " --delay " + std::to_string(std::min(delay, 8)); }

PortPlan build_ports(std::vector<MemberPads> m) {
    std::sort(m.begin(), m.end(), [](const MemberPads& a, const MemberPads& b) { return a.slot < b.slot; });
    PortPlan p;
    if (m.size() > 4) m.resize(4);
    p.players = (int)m.size();
    std::vector<int> eff(m.size(), 1);
    int used = (int)m.size();
    for (size_t i = 0; i < m.size() && used < 4; ++i)
        if (m[i].locals >= 2) { eff[i] = 2; ++used; }
    std::vector<int> ports;
    for (size_t i = 0; i < m.size(); ++i)
        for (int k = 0; k < eff[i]; ++k) ports.push_back(m[i].slot);
    for (int i = 0; i < 4; ++i) {
        if (i) p.ports += ",";
        p.ports += i < (int)ports.size() ? std::to_string(ports[i]) : "-1";
    }
    p.locals = eff;
    return p;
}

SyncAction sync_request(SyncGate& g, bool manual, bool game_running, bool job_running) {
    if (game_running || job_running) {
        g.pending = true;
        g.manual = g.manual || manual;
        return SyncAction::Queue;
    }
    return SyncAction::Start;
}
bool sync_take_pending(SyncGate& g, bool game_running, bool job_running, bool* manual) {
    if (!g.pending || game_running || job_running) return false;
    if (manual) *manual = g.manual;
    g.pending = false;
    g.manual = false;
    return true;
}

std::string ports_arg(std::vector<int> slots) {
    std::sort(slots.begin(), slots.end());
    std::string o;
    for (int i = 0; i < 4; ++i) {
        if (i) o += ",";
        o += (i < (int)slots.size()) ? std::to_string(slots[i]) : "-1";
    }
    return o;
}

std::string exit_code_text(unsigned long code) {
    switch (code) {
        case 0: return "Game closed normally.";
        case 2: return "The game couldn't start: the room's player/port setup or the ROM was rejected. Leave the room, rejoin and try again.";
        case 3: return "Emulator files are missing (emu/ folder). Reinstall or run scripts/fetch-emu.ps1.";
        case 4: return "Connection lost during the match.";
        case 5: return "The game went out of sync (desync) and was stopped.";
        case 6: return "Couldn't share the host's save - check your connection and try again.";
        default: return "Game exited with code " + std::to_string(code) + ".";
    }
}

std::string normalize_code(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isspace(c)) continue;
        o += (char)std::toupper(c);
        if (o.size() == 6) break;
    }
    return o;
}

/* ---------------- library list helpers ---------------- */

static std::string lower(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}

std::vector<int> filter_sort(const std::vector<ListItem>& items, const std::string& system_filter,
                             const std::string& query) {
    std::string q = lower(query);
    size_t b = q.find_first_not_of(' ');
    size_t e = q.find_last_not_of(' ');
    q = (b == std::string::npos) ? "" : q.substr(b, e - b + 1);
    std::vector<int> v;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!system_filter.empty() && items[i].system != system_filter) continue;
        if (!q.empty() && lower(items[i].title).find(q) == std::string::npos) continue;
        v.push_back((int)i);
    }
    std::stable_sort(v.begin(), v.end(), [&](int a, int b2) {
        const ListItem &x = items[a], &y = items[b2];
        if (x.pinned != y.pinned) return x.pinned;
        std::string lx = lower(x.title), ly = lower(y.title);
        if (lx != ly) return lx < ly;
        return x.key < y.key;
    });
    return v;
}

std::vector<int> pick_recent(const std::vector<unsigned long long>& last_played, int n) {
    std::vector<int> v;
    for (size_t i = 0; i < last_played.size(); ++i)
        if (last_played[i] > 0) v.push_back((int)i);
    std::stable_sort(v.begin(), v.end(), [&](int a, int b) { return last_played[a] > last_played[b]; });
    if ((int)v.size() > n) v.resize(n);
    return v;
}

/* ---------------- home composition / layout ---------------- */

std::string system_shelf_title(const std::string& s) {
    if (s == "n64") return "Nintendo 64";
    if (s == "gc") return "GameCube";
    if (s == "gba") return "Game Boy Advance";
    if (s == "gbc") return "Game Boy Color";
    if (s == "gb") return "Game Boy";
    return s;
}

const std::vector<std::string>& genre_order() {
    static const std::vector<std::string> g = {"Party", "Fighting", "Racing", "Shooter", "Platformer", "Adventure",
                                                "RPG", "Strategy", "Sports", "Puzzle", "Action"};
    return g;
}

std::string effective_genre(const std::string& genre) {
    for (auto& g : genre_order())
        if (g == genre) return g;
    return "Other";
}

std::vector<Shelf> compose_shelves(const std::vector<ShelfGame>& g, int max_recent) {
    auto by_title = [&](std::vector<int>& v) {
        std::stable_sort(v.begin(), v.end(), [&](int a, int b) {
            if (g[a].pinned != g[b].pinned) return g[a].pinned;
            std::string x = lower(g[a].title), y = lower(g[b].title);
            if (x != y) return x < y;
            return g[a].key < g[b].key;
        });
    };
    std::vector<Shelf> out;
    std::vector<unsigned long long> lp;
    for (auto& e : g) lp.push_back(e.last_played);
    Shelf recent{"recent", "Continue Playing", pick_recent(lp, max_recent)};
    if (!recent.items.empty()) out.push_back(recent);
    Shelf favs{"favs", "Favorites", {}};
    for (size_t i = 0; i < g.size(); ++i)
        if (g[i].favorite) favs.items.push_back((int)i);
    by_title(favs.items);
    if (!favs.items.empty()) out.push_back(favs);
    for (const std::string& gn : genre_order()) {
        Shelf sh{"genre:" + gn, gn, {}};
        for (size_t i = 0; i < g.size(); ++i)
            if (effective_genre(g[i].genre) == gn) sh.items.push_back((int)i);
        by_title(sh.items);
        if (!sh.items.empty()) out.push_back(sh);
    }
    static const char* sys_order[] = {"n64", "gc", "gba", "gbc", "gb"};
    std::vector<bool> placed(g.size(), false);
    for (const char* sys : sys_order) {
        Shelf sh{std::string("genre:Other:") + sys, "Other - " + system_shelf_title(sys), {}};
        for (size_t i = 0; i < g.size(); ++i)
            if (effective_genre(g[i].genre) == "Other" && g[i].system == sys) { sh.items.push_back((int)i); placed[i] = true; }
        by_title(sh.items);
        if (!sh.items.empty()) out.push_back(sh);
    }
    /* Unknown system codes still must appear exactly once. */
    Shelf rest{"genre:Other", "Other", {}};
    for (size_t i = 0; i < g.size(); ++i)
        if (effective_genre(g[i].genre) == "Other" && !placed[i]) rest.items.push_back((int)i);
    by_title(rest.items);
    if (!rest.items.empty()) out.push_back(rest);
    return out;
}

int rect_hit(const std::vector<FRect>& rects, float x, float y) {
    for (size_t i = 0; i < rects.size(); ++i)
        if (x >= rects[i].x0 && x < rects[i].x1 && y >= rects[i].y0 && y < rects[i].y1) return (int)i;
    return -1;
}
float shelf_wheel_delta(float wx, float wy, float tile_w, float gap) { return (wx - wy) * 1.5f * (tile_w + gap); }
bool drag_exceeds(float dx, float dy, float t) { return dx * dx + dy * dy > t * t; }
float shelf_page_step(float view_w, float tile_w, float gap, float bleed) {
    int n = std::max(1, (int)((view_w - bleed * 2 + gap) / (tile_w + gap)));
    return n * (tile_w + gap);
}
float shelf_clamp(float v, float m) { return std::max(0.f, std::min(std::max(0.f, m), v)); }
float shelf_reveal(float left, float right, float target, float view_w, float margin, float max_scroll) {
    if (left - target < margin) target = left - margin;
    else if (right - target > view_w - margin) target = right - view_w + margin;
    return shelf_clamp(target, max_scroll);
}
float shelf_max_scroll(int count, float tw, float gap, float bleed, float view_w) {
    float content = bleed * 2 + count * tw + std::max(0, count - 1) * gap;
    return std::max(0.f, content - view_w);
}

std::vector<int> pick_unowned(const std::vector<bool>& owned, const std::vector<bool>& online, int max_n) {
    std::vector<int> v;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < owned.size(); ++i) {
            bool on = i < online.size() && online[i];
            if (owned[i] || on != (pass == 0)) continue;
            if ((int)v.size() >= max_n) return v;
            v.push_back((int)i);
        }
    return v;
}

int pick_hero(const std::vector<ShelfGame>& g, const std::string& preferred_key) {
    if (g.empty()) return -1;
    for (size_t i = 0; i < g.size(); ++i)
        if (!preferred_key.empty() && g[i].key == preferred_key) return (int)i;
    int best = -1;
    for (size_t i = 0; i < g.size(); ++i)
        if (g[i].last_played > 0 && (best < 0 || g[i].last_played > g[best].last_played)) best = (int)i;
    if (best >= 0) return best;
    for (size_t i = 0; i < g.size(); ++i)
        if (g[i].favorite) return (int)i;
    return 0;
}

GridLayout grid_layout(float avail, float min_w, float max_w, float gap) {
    GridLayout r;
    r.cols = std::max(1, (int)((avail + gap) / (min_w + gap)));
    r.tile_w = std::min(max_w, (avail - gap * (r.cols - 1)) / r.cols);
    if (r.tile_w < 1.f) r.tile_w = 1.f;
    return r;
}

float shelf_tile_w(float avail, float min_w, float max_w, float gap) {
    int n = std::max(2, (int)((avail + gap) / (max_w + gap)));
    float w = (avail - gap * n) / (n + 0.4f);
    return std::max(min_w, std::min(max_w, w));
}

float approach(float v, float target, float dt, float rate) {
    if (dt <= 0) return v;
    return v + (target - v) * (1.f - std::exp(-rate * dt));
}

ArtFit fit_art(float aw, float ah, float bw, float bh, float lo, float hi) {
    ArtFit f;
    if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0) { f.w = bw; f.h = bh; return f; }
    float art_a = aw / ah, box_a = bw / bh, r = art_a / box_a;
    if (r >= lo && r <= hi) {   /* crop to fill */
        f.cover = true;
        f.w = bw; f.h = bh;
        if (art_a > box_a) {   // art wider: crop left/right
            float keep = box_a / art_a;
            f.u0 = (1 - keep) * 0.5f; f.u1 = f.u0 + keep;
        } else {               // art taller: crop top/bottom
            float keep = art_a / box_a;
            f.v0 = (1 - keep) * 0.5f; f.v1 = f.v0 + keep;
        }
        return f;
    }
    f.cover = false;
    if (art_a > box_a) { f.w = bw; f.h = bw / art_a; }
    else { f.h = bh; f.w = bh * art_a; }
    f.x = (bw - f.w) * 0.5f;
    f.y = (bh - f.h) * 0.5f;
    return f;
}

bool sidebar_collapsed(int mode, float window_w, float narrow_below) {
    if (mode == 1) return false;
    if (mode == 2) return true;
    return window_w < narrow_below;
}

std::string format_play_time(unsigned long long s) {
    if (s == 0) return "Never played";
    if (s < 60) return "Under a minute";
    if (s < 3600) return std::to_string(s / 60) + " min";
    char b[32];
    std::snprintf(b, sizeof b, "%lluh %02llum", s / 3600, (s % 3600) / 60);
    return b;
}

std::string sanitize_player_name(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') continue;
        if (o.size() >= 16 && (c & 0xC0) != 0x80) break;   // never cut inside a UTF-8 sequence
        o += (char)c;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    while (!o.empty() && o[0] == ' ') o.erase(0, 1);
    return o.empty() ? "Player" : o;
}

/* ---------------- INI ---------------- */

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r");
    size_t e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}
static bool section_line(const std::string& line, std::string* name) {
    std::string t = trim(line);
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') { *name = lower(t.substr(1, t.size() - 2)); return true; }
    return false;
}
static bool key_line(const std::string& line, const std::string& lkey, std::string* val) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    if (lower(trim(line.substr(0, eq))) != lkey) return false;
    if (val) *val = trim(line.substr(eq + 1));
    return true;
}
static std::vector<std::string> split_lines(const std::string& t) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : t) {
        if (c == '\n') { v.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

std::string ini_get(const std::string& text, const std::string& section, const std::string& key, const std::string& def) {
    std::string cur, val, ls = lower(section), lk = lower(key);
    for (auto& line : split_lines(text)) {
        std::string n;
        if (section_line(line, &n)) { cur = n; continue; }
        if (cur == ls && key_line(line, lk, &val)) return val;
    }
    return def;
}

std::string ini_set(const std::string& text, const std::string& section, const std::string& key, const std::string& value) {
    std::vector<std::string> lines = split_lines(text);
    std::string ls = lower(section), lk = lower(key), cur;
    int sec_end = -1;   // index just after the last line of the target section
    bool in_sec = false, done = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string n;
        if (section_line(lines[i], &n)) {
            if (in_sec) { sec_end = (int)i; in_sec = false; }
            cur = n;
            if (cur == ls) in_sec = true;
            continue;
        }
        if (in_sec && key_line(lines[i], lk, nullptr)) { lines[i] = key + " = " + value; done = true; break; }
    }
    if (!done) {
        if (in_sec) sec_end = (int)lines.size();
        if (sec_end >= 0) {
            int at = sec_end;
            while (at > 0 && trim(lines[at - 1]).empty()) --at;   // insert before trailing blank lines
            lines.insert(lines.begin() + at, key + " = " + value);
        } else {
            if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
            lines.push_back("[" + section + "]");
            lines.push_back(key + " = " + value);
        }
    }
    std::string o;
    for (auto& l : lines) o += l + "\n";
    return o;
}

/* ---------------- vault meta ---------------- */

bool parse_vault_meta(const std::string& text, VaultMeta* out) {
    Json j;
    if (!json_parse(text, &j) || j.type != Json::Obj) return false;
    out->key = j.str("key");
    out->title = j.str("title");
    out->system = j.str("system");
    out->device = j.str("device");
    out->updated = (unsigned long long)j.num("updated", 0);
    return !out->key.empty();
}

/* ---------------- friends ---------------- */

static std::string lower_str(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

static int online_rank(const FriendRow& r) {
    if (r.state == FS_InLobby && r.joinable) return 0;
    if (r.state == FS_InGame) return 1;
    if (r.state == FS_InLobby) return 2;
    return 3;
}

FriendGroups group_friends(const std::vector<FriendRow>& rows) {
    FriendGroups g;
    for (int i = 0; i < (int)rows.size(); ++i) (rows[i].state != FS_Offline ? g.online : g.offline).push_back(i);
    std::stable_sort(g.online.begin(), g.online.end(), [&](int a, int b) {
        int ra = online_rank(rows[a]), rb = online_rank(rows[b]);
        if (ra != rb) return ra < rb;
        std::string x = lower_str(rows[a].name), y = lower_str(rows[b].name);
        return x != y ? x < y : rows[a].uid < rows[b].uid;
    });
    std::stable_sort(g.offline.begin(), g.offline.end(), [&](int a, int b) {
        if (rows[a].updated_ms != rows[b].updated_ms) return rows[a].updated_ms > rows[b].updated_ms;
        std::string x = lower_str(rows[a].name), y = lower_str(rows[b].name);
        return x != y ? x < y : rows[a].uid < rows[b].uid;
    });
    return g;
}

std::string format_ago_ms(long long now_ms, long long then_ms) {
    long long s = (now_ms - then_ms) / 1000;
    if (s < 60) return "just now";
    if (s < 3600) return std::to_string(s / 60) + " min ago";
    if (s < 86400) return std::to_string(s / 3600) + "h ago";
    return std::to_string(s / 86400) + "d ago";
}

std::string friend_status_line(const FriendRow& r, long long now_ms) {
    switch (r.state) {
        case FS_InGame: return r.game_title.empty() ? "Playing" : "Playing " + r.game_title;
        case FS_InLobby: {
            std::string s = "In lobby";
            if (!r.game_title.empty()) s += " \xC2\xB7 " + r.game_title;
            if (r.max_players > 0) s += " \xC2\xB7 " + std::to_string(r.players) + "/" + std::to_string(r.max_players);
            return s;
        }
        case FS_Online: return "Online";
        default: break;
    }
    if (r.updated_ms > 0 && now_ms > r.updated_ms) return "Last seen " + format_ago_ms(now_ms, r.updated_ms);
    return "Offline";
}

std::string normalize_friend_code(const std::string& in) {
    std::string t;
    for (unsigned char ch : in)
        if (ch != ' ' && ch != '-' && ch != '\t' && ch != '_') t += (char)std::toupper(ch);
    if (t.size() == 10 && t.compare(0, 2, "RH") == 0) t = t.substr(2);
    if (t.size() != 8) return "";
    for (char ch : t)
        if (!std::isalnum((unsigned char)ch)) return "";
    return "RH-" + t.substr(0, 4) + "-" + t.substr(4);
}

int invite_seconds_left(long long expires_at_ms, long long now_ms) {
    if (expires_at_ms <= 0 || now_ms >= expires_at_ms) return 0;
    return (int)((expires_at_ms - now_ms + 999) / 1000);
}

} // namespace hubui
