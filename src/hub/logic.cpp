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
      << "  \"rom_folder\": \"" << json_escape(s.rom_folder) << "\",\n"
      << "  \"server\": \"" << json_escape(s.server) << "\",\n"
      << "  \"input_delay\": " << s.input_delay << ",\n"
      << "  \"last_game\": \"" << json_escape(s.last_game) << "\",\n"
      << "  \"smash_exe\": \"" << json_escape(s.smash_exe) << "\"\n"
      << "}\n";
    return o.str();
}

bool settings_from_json(const std::string& text, Settings* out) {
    Json j;
    if (!json_parse(text, &j) || j.type != Json::Obj) return false;
    out->name = j.str("name", out->name);
    out->rom_folder = j.str("rom_folder", out->rom_folder);
    out->server = j.str("server", out->server);
    out->input_delay = (int)j.num("input_delay", out->input_delay);
    out->input_delay = std::max(1, std::min(6, out->input_delay));
    out->last_game = j.str("last_game", out->last_game);
    out->smash_exe = j.str("smash_exe", out->smash_exe);
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
    const PeerInfo* host = nullptr;
    for (auto& p : peers) if (p.slot == host_slot) host = &p;
    for (auto& p : peers) {
        if (!p.hello) { r.reason = "waiting for " + p.name; return r; }
        if (!p.games.count(game_id)) { r.reason = p.name + " doesn't have this game"; return r; }
    }
    if (host) {
        const std::string& ref = host->games.at(game_id);
        for (auto& p : peers)
            if (p.games.at(game_id) != ref) { r.reason = "different ROM version (" + p.name + ")"; return r; }
    }
    r.ok = true;
    return r;
}

std::string hello_json(const std::string& version, const std::map<std::string, std::string>& games) {
    std::string o = "{\"kind\":\"hello\",\"version\":\"" + json_escape(version) + "\",\"games\":{";
    bool first = true;
    for (auto& g : games) {
        if (!first) o += ",";
        first = false;
        o += "\"" + json_escape(g.first) + "\":\"" + json_escape(g.second) + "\"";
    }
    return o + "}}";
}

bool parse_hello(const Json& body, PeerInfo* out) {
    if (body.str("kind") != "hello") return false;
    const Json* g = body.get("games");
    out->games.clear();
    if (g && g->type == Json::Obj)
        for (size_t i = 0; i < g->keys.size(); ++i)
            if (g->vals[i].type == Json::Str) out->games[g->keys[i]] = g->vals[i].s;
    out->hello = true;
    return true;
}

std::string launch_json(const std::string& game, int launch, int delay, unsigned seed) {
    return "{\"kind\":\"launch\",\"game\":\"" + json_escape(game) + "\",\"launch\":" + std::to_string(launch) +
           ",\"delay\":" + std::to_string(delay) + ",\"seed\":" + std::to_string(seed) + "}";
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

} // namespace hubui
