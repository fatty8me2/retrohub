#include "logic.h"

#include <algorithm>
#include <cctype>
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
      << "  \"last_game\": \"" << json_escape(s.last_game) << "\",\n"
      << "  \"smash_exe\": \"" << json_escape(s.smash_exe) << "\",\n"
      << "  \"gb_core\": \"" << json_escape(s.gb_core) << "\",\n"
      << "  \"fullscreen\": " << (s.fullscreen ? "true" : "false") << ",\n"
      << "  \"sync_folder\": \"" << json_escape(s.sync_folder) << "\",\n"
      << "  \"sync_auto\": " << (s.sync_auto ? "true" : "false") << ",\n"
      << "  \"sync_states\": " << (s.sync_states ? "true" : "false") << ",\n"
      << "  \"vault_dir\": \"" << json_escape(s.vault_dir) << "\"\n"
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
    out->input_delay = std::max(1, std::min(6, out->input_delay));
    out->last_game = j.str("last_game", out->last_game);
    out->smash_exe = j.str("smash_exe", out->smash_exe);
    std::string core = j.str("gb_core", out->gb_core);
    out->gb_core = (core == "gambatte" || core == "sameboy") ? core : "sameboy";
    out->fullscreen = json_bool(j, "fullscreen", out->fullscreen);
    out->sync_folder = j.str("sync_folder", out->sync_folder);
    out->sync_auto = json_bool(j, "sync_auto", out->sync_auto);
    out->sync_states = json_bool(j, "sync_states", out->sync_states);
    out->vault_dir = j.str("vault_dir", out->vault_dir);
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

std::string launch_json(const std::string& game, int launch, int delay, unsigned seed, const std::string& core) {
    std::string o = "{\"kind\":\"launch\",\"game\":\"" + json_escape(game) + "\",\"launch\":" + std::to_string(launch) +
                    ",\"delay\":" + std::to_string(delay) + ",\"seed\":" + std::to_string(seed);
    if (!core.empty()) o += ",\"core\":\"" + json_escape(core) + "\"";
    return o + "}";
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
        case 2: return "n64run could not start: bad arguments or the ROM could not be read.";
        case 3: return "Emulator files are missing (emu/ folder). Reinstall or run scripts/fetch-emu.ps1.";
        case 4: return "Connection lost during the match.";
        case 5: return "The game went out of sync (desync) and was stopped.";
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

} // namespace hubui
