#include "json.h"
#include "art_match.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace hubart_match {

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

std::string thumb_name(const std::string& n) {
    std::string r = n;
    for (auto& c : r) {
        switch (c) {
        case '&': case '*': case '/': case ':': case '`': case '<': case '>': case '?': case '\\': case '|':
            c = '_';
            break;
        default: break;
        }
    }
    return r;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
            c == '(' || c == ')' || c == '!' || c == '\'') {
            r += (char)c;
        } else {
            r += '%';
            r += hex[c >> 4];
            r += hex[c & 15];
        }
    }
    return r;
}

std::string thumb_repo(const std::string& s) {
    if (s == "n64") return "Nintendo_-_Nintendo_64";
    if (s == "gba") return "Nintendo_-_Game_Boy_Advance";
    if (s == "gbc") return "Nintendo_-_Game_Boy_Color";
    if (s == "gb") return "Nintendo_-_Game_Boy";
    if (s == "gc") return "Nintendo_-_GameCube";
    if (s == "3ds") return "Nintendo_-_Nintendo_3DS";
    if (s == "wiiu") return "Nintendo_-_Wii_U";
    if (s == "nds") return "Nintendo_-_Nintendo_DS";
    if (s == "ps2") return "Sony_-_PlayStation_2";
    if (s == "ps1") return "Sony_-_PlayStation";
    if (s == "nes") return "Nintendo_-_Nintendo_Entertainment_System";
    if (s == "snes") return "Nintendo_-_Super_Nintendo_Entertainment_System";
    return "";
}

std::string dat_url(const std::string& s) {
    const char* base = "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/";
    if (s == "n64") return std::string(base) + "no-intro/Nintendo%20-%20Nintendo%2064.dat";
    if (s == "gba") return std::string(base) + "no-intro/Nintendo%20-%20Game%20Boy%20Advance.dat";
    if (s == "gbc") return std::string(base) + "no-intro/Nintendo%20-%20Game%20Boy%20Color.dat";
    if (s == "gb") return std::string(base) + "no-intro/Nintendo%20-%20Game%20Boy.dat";
    if (s == "gc") return std::string(base) + "redump/Nintendo%20-%20GameCube.dat";
    if (s == "3ds") return std::string(base) + "no-intro/Nintendo%20-%20Nintendo%203DS.dat";
    if (s == "nds") return std::string(base) + "no-intro/Nintendo%20-%20Nintendo%20DS.dat";
    if (s == "ps2") return std::string(base) + "redump/Sony%20-%20PlayStation%202.dat";
    if (s == "ps1") return std::string(base) + "redump/Sony%20-%20PlayStation.dat";
    if (s == "nes") return std::string(base) + "no-intro/Nintendo%20-%20Nintendo%20Entertainment%20System.dat";
    if (s == "snes") return std::string(base) + "no-intro/Nintendo%20-%20Super%20Nintendo%20Entertainment%20System.dat";
    return "";
}

const char* kind_dir(int kind) {
    switch (kind) {
    case 0: return "Named_Boxarts";
    case 1: return "Named_Titles";
    default: return "Named_Snaps";
    }
}

std::string thumb_url(const std::string& system_short, int kind, const std::string& nointro_name) {
    std::string repo = thumb_repo(system_short);
    if (repo.empty() || nointro_name.empty()) return "";
    return "https://raw.githubusercontent.com/libretro-thumbnails/" + repo + "/master/" +
           kind_dir(kind) + "/" + url_encode(thumb_name(nointro_name)) + ".png";
}

std::vector<std::string> thumb_urls(const std::string& system_short, int kind, const std::string& nointro_name) {
    std::vector<std::string> out;
    std::string u = thumb_url(system_short, kind, nointro_name);
    if (u.empty()) return out;
    out.push_back(u);
    if (system_short == "gbc") out.push_back(thumb_url("gb", kind, nointro_name));
    return out;
}

/* ---- DAT parsing ---- */

struct Tok { std::string s; bool quoted; };

static std::vector<Tok> tokenize(const std::string& line) {
    std::vector<Tok> t;
    size_t i = 0, n = line.size();
    while (i < n) {
        char c = line[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        if (c == '"') {
            size_t j = i + 1;
            std::string v;
            while (j < n && line[j] != '"') {
                if (line[j] == '\\' && j + 1 < n) ++j;
                v += line[j++];
            }
            t.push_back({v, true});
            i = j + 1;
        } else {
            size_t j = i;
            while (j < n && !std::isspace((unsigned char)line[j])) ++j;
            t.push_back({line.substr(i, j - i), false});
            i = j;
        }
    }
    return t;
}

std::vector<DatRec> parse_dat(const std::string& text) {
    std::vector<DatRec> out;
    bool in_game = false;
    std::string gname, gserial;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        auto t = tokenize(line);
        if (t.empty()) continue;
        if (!t[0].quoted && t[0].s == "game") { in_game = true; gname.clear(); gserial.clear(); continue; }
        if (!in_game) continue;
        if (!t[0].quoted && t[0].s == ")" && t.size() == 1) { in_game = false; continue; }
        if (!t[0].quoted && t[0].s == "name" && t.size() >= 2) { gname = t[1].s; continue; }
        if (!t[0].quoted && t[0].s == "serial" && t.size() >= 2) { gserial = t[1].s; continue; }
        if (!t[0].quoted && t[0].s == "rom") {
            DatRec r;
            r.name = gname;
            r.serial = gserial;
            for (size_t i = 1; i < t.size(); ++i) {
                if (t[i].quoted || t[i].s == "(" || t[i].s == ")") continue;
                if (i + 1 >= t.size()) break;
                const std::string& k = t[i].s;
                const std::string& v = t[i + 1].s;
                if (k == "md5") r.md5 = upper(v);
                else if (k == "crc") r.crc = upper(v);
                else if (k == "serial") r.serial = v;
                else continue;
                ++i;
            }
            if (!r.name.empty()) out.push_back(r);
        }
    }
    return out;
}

/* ---- matching ---- */

std::string normalize_title(const std::string& s) {
    std::string r;
    int depth = 0;
    for (unsigned char c : s) {
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth > 0) --depth; continue; }
        if (depth) continue;
        if (std::isalnum(c)) r += (char)std::tolower(c);
    }
    return r;
}

static bool has(const std::string& low, const char* needle) { return low.find(needle) != std::string::npos; }

int release_score(const std::string& name) {
    std::string l = lower(name);
    int s;
    if (has(l, "(usa")) s = 0;
    else if (has(l, "europe")) s = 1;
    else if (has(l, "world")) s = 2;
    else s = 3;
    if (has(l, "(proto") || has(l, "(beta") || has(l, "(demo") || has(l, "(sample") || has(l, "(unl") ||
        has(l, "(kiosk") || has(l, "(promo") || has(l, "(aftermarket") || has(l, "(pirate"))
        s += 10;
    if (has(l, "(rev ") || has(l, "(alt") || has(l, "(disc 2") || has(l, "(disc 3") || has(l, "(disc 4"))
        s += 1;
    return s;
}

static std::string gc_id4(const std::string& serial) {
    /* Redump: "DL-DOL-GW7E-USA" -> "GW7E"; header disc ID "GALE01" -> "GALE" */
    std::string s = upper(serial);
    size_t a = s.find('-');
    if (a != std::string::npos) {
        std::vector<std::string> parts;
        size_t p = 0;
        while (true) {
            size_t q = s.find('-', p);
            parts.push_back(s.substr(p, q == std::string::npos ? q : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (parts.size() >= 3) return parts[2];
    }
    return s.substr(0, 4);
}

static bool serial_equal(const std::string& system, const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    if (system == "gc") return gc_id4(a) == gc_id4(b);
    if (system == "3ds") {   // no-intro serial "CTR-P-EKJA" vs the 4-char game code "EKJA"
        std::string ua = upper(a), ub = upper(b);
        return ua.size() >= ub.size() && (ua.compare(ua.size() - ub.size(), ub.size(), ub) == 0 || ua.find("-" + ub + "-") != std::string::npos);
    }
    return upper(a) == upper(b);
}

/* Lowest-score record among candidates; ties keep the first. */
template <class Pred>
static const DatRec* best(const std::vector<DatRec>& dat, Pred pred) {
    const DatRec* b = nullptr;
    int bs = 1 << 30;
    for (const auto& r : dat) {
        if (!pred(r)) continue;
        int s = release_score(r.name);
        if (s < bs) { bs = s; b = &r; }
    }
    return b;
}

std::string match_name(const std::vector<DatRec>& dat, const Query& q, std::string* how) {
    auto done = [&](const DatRec* r, const char* h) -> std::string {
        if (!r) return "";
        if (how) *how = h;
        return r->name;
    };
    if (!q.md5.empty()) {
        std::string m = upper(q.md5);
        if (auto r = best(dat, [&](const DatRec& d) { return d.md5 == m; })) return done(r, "md5");
    }
    if (!q.crc.empty()) {
        std::string c = upper(q.crc);
        while (c.size() < 8) c = "0" + c;
        if (auto r = best(dat, [&](const DatRec& d) { return d.crc == c; })) return done(r, "crc");
    }
    if (!q.serial.empty()) {
        if (auto r = best(dat, [&](const DatRec& d) { return serial_equal(q.system, d.serial, q.serial); }))
            return done(r, "serial");
    }
    std::string nt = normalize_title(q.title);
    if (nt.size() >= 3) {
        if (auto r = best(dat, [&](const DatRec& d) { return normalize_title(d.name) == nt; }))
            return done(r, "title");
        /* Game Boy headers truncate the title: allow a prefix match there only. */
        if ((q.system == "gb" || q.system == "gbc") && nt.size() >= 6) {
            if (auto r = best(dat, [&](const DatRec& d) {
                    std::string dn = normalize_title(d.name);
                    return dn.size() > nt.size() && dn.compare(0, nt.size(), nt) == 0;
                }))
                return done(r, "title");
        }
    }
    return "";
}

/* ---- JSON string map (shared reader/writer, json.h) ---- */

std::string map_to_json(const std::map<std::string, std::string>& m) {
    std::string o = "{\n";
    bool first = true;
    for (const auto& kv : m) {
        if (!first) o += ",\n";
        first = false;
        o += "  " + json::quote(kv.first) + ": " + json::quote(kv.second);
    }
    o += "\n}\n";
    return o;
}

bool map_from_json(const std::string& t, std::map<std::string, std::string>* out) {
    out->clear();
    json::Value root;
    if (!json::parse(t, &root, json::ParseOptions{64, true}) || root.type != json::Value::Object) return false;
    for (const auto& kv : root.o) {
        if (kv.second.type != json::Value::String) { out->clear(); return false; }
        (*out)[kv.first] = kv.second.s;
    }
    return true;
}

} // namespace hubart_match
