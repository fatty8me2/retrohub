#include "cheats_db.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "games.h"
#include "json.h"
#include "miniz.h"
#include "winutil.h"

namespace hub {
namespace {

using cheats::Cheat;
using cheats::Format;

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool format_from(const std::string& s, Format* f) {
    std::string l = lower(s);
    if (l == "gameshark" || l == "gs") *f = Format::GameShark;
    else if (l == "ar" || l == "actionreplay") *f = Format::ActionReplay;
    else if (l == "codebreaker" || l == "cb") *f = Format::CodeBreaker;
    else if (l == "gamegenie" || l == "gg") *f = Format::GameGenie;
    else if (l == "ar_gc") *f = Format::ActionReplayGC;
    else if (l == "gecko") *f = Format::Gecko;
    else if (l == "raw") *f = Format::Raw;
    else return false;
    return true;
}

/* Split "Name (USA, Europe) [!]" into the bare name and the list of tag texts. */
std::string strip_tags(std::string name, std::vector<std::string>* tags) {
    std::string out;
    int depth = 0;
    std::string cur;
    for (char c : name) {
        if (c == '(' || c == '[') {
            if (depth++ == 0) cur.clear();
        } else if (c == ')' || c == ']') {
            if (depth > 0 && --depth == 0 && tags) tags->push_back(cur);
        } else if (depth > 0) cur += c;
        else out += c;
    }
    return out;
}

std::string strip_ext(const std::string& n) {
    size_t dot = n.rfind('.');
    if (dot != std::string::npos && n.size() - dot <= 5) {
        std::string e = lower(n.substr(dot + 1));
        static const char* x[] = {"z64", "v64", "n64", "gba", "gb", "gbc", "zip", "7z", "cht", "bin", "cue", "iso", "chd", "rom"};
        for (auto* s : x) if (e == s) return n.substr(0, dot);
    }
    return n;
}

std::string stem_of(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return strip_ext(p == std::string::npos ? path : path.substr(p + 1));
}

}  // namespace

std::string cheats_region_of(const std::string& name) {
    std::vector<std::string> tags;
    strip_tags(name, &tags);
    std::string r;
    for (auto& t : tags) {
        std::string tok;
        for (size_t i = 0; i <= t.size(); ++i) {
            if (i == t.size() || t[i] == ',' || t[i] == ' ') {
                std::string l = lower(tok);
                tok.clear();
                if (l == "usa" || l == "u" || l == "ntsc") r += 'U';
                else if (l == "europe" || l == "e" || l == "pal" || l == "australia" || l == "a" || l == "germany" ||
                         l == "france" || l == "spain" || l == "italy" || l == "g" || l == "f" || l == "s" || l == "i")
                    r += 'E';
                else if (l == "japan" || l == "j") r += 'J';
                else if (l == "world" || l == "w") return "";
            } else tok += t[i];
        }
    }
    std::string o;
    for (char c : std::string("UEJ")) if (r.find(c) != std::string::npos) o += c;
    return o;
}

std::string cheats_norm_key(const std::string& name) {
    std::string s = lower(strip_tags(strip_ext(name), nullptr));
    std::string t;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '&') t += " and ";
        else if (c == '\'') continue;
        else if (std::isalnum(c)) t += (char)c;
        else t += ' ';
    }
    std::vector<std::string> toks;
    std::string cur;
    for (size_t i = 0; i <= t.size(); ++i) {
        if (i == t.size() || t[i] == ' ') {
            if (!cur.empty() && cur != "the" && cur != "version") toks.push_back(cur);
            cur.clear();
        } else cur += t[i];
    }
    std::sort(toks.begin(), toks.end());
    std::string k;
    for (auto& x : toks) { if (!k.empty()) k += ' '; k += x; }
    return k;
}

bool cheats_is_master_desc(const std::string& desc) {
    std::string d = lower(desc);
    return d.find("enable code") != std::string::npos || d.find("master code") != std::string::npos ||
           d.find("must be on") != std::string::npos || d.find("(m)") != std::string::npos;
}

bool cheats_is_featured_desc(const std::string& desc) {
    if (cheats_is_master_desc(desc)) return false;
    std::string d = lower(desc);
    static const char* kw[] = {"unlock all", "all characters", "all hidden", "all stages", "all levels", "all items",
                               "all courses", "all cups", "all tracks", "all pokemon", "have all", "infinite", "unlimited",
                               "never lose", "invincib", "invulnerab", "max money", "max health", "max stat", "max level",
                               "999", "walk through wall", "moon jump", "all moves", "all weapons", "all keys",
                               "all upgrades", "all abilities", "all maps", "max coins", "max lives", "max hp", "max ammo",
                               "god mode", "master ball", "rare candy", "level select", "stage select", "all unlocked",
                               "everything unlocked", "unlock everything", "open all", "all mini", "full health", "debug"};
    for (auto* k : kw) if (d.find(k) != std::string::npos) return true;
    return false;
}

namespace {

struct Section {
    std::string sys, name, region;
    int gc_rev = -1;           // gc: revision of "GALE01r2" (2); -1 = every revision
    Format fmt = Format::Raw;
    size_t begin = 0, end = 0;   // line range in Db::raw
};

struct Db {
    std::string raw;
    std::vector<Section> sections;
    std::unordered_map<std::string, std::vector<size_t>> index;   // sys + '|' + key -> sections
    std::vector<std::pair<std::string, std::vector<Cheat>>> featured_names;   // filled into fidx
    struct Feat { std::string sys_any; std::string region; std::vector<Cheat> cheats; };
    std::unordered_map<std::string, std::vector<Feat>> fidx;      // key -> curated
};

std::mutex g_mu;
std::string g_dir;
Db* g_db = nullptr;
bool g_loaded = false;

std::string dir() {
    return g_dir.empty() ? exe_dir() + "data/" : g_dir;
}

bool slurp(const std::string& p, std::string* out) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    out->clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
    std::fclose(f);
    return true;
}

/* GameCube sections are named by Dolphin's game ini ("GALE01r2", "GM4"): lower-case ID without the revision suffix. */
std::string gc_id_key(const std::string& stem) {
    std::string k = lower(stem);
    size_t r = k.find('r', 3);
    if (r != std::string::npos && r >= 4 && r + 1 < k.size() && std::isdigit((unsigned char)k[r + 1])) k.resize(r);
    return k;
}

/* "GALE01r2" -> 2; -1 when the name has no revision suffix. */
int gc_rev_of(const std::string& stem) {
    std::string k = lower(stem);
    size_t r = k.find('r', 3);
    if (r == std::string::npos || r < 4 || r + 1 >= k.size()) return -1;
    for (size_t i = r + 1; i < k.size(); ++i) if (!std::isdigit((unsigned char)k[i])) return -1;
    return std::atoi(k.c_str() + r + 1);
}

void load_featured(Db* db, const std::string& text) {
    json::Value v;
    if (!json::parse(text, &v) || !v.is_object()) return;
    for (auto& kv : v.o) {
        if (!kv.second.is_array()) continue;
        Db::Feat f;
        f.region = cheats_region_of(kv.first);
        int n = 0;
        for (auto& c : kv.second.a) {
            if (!c.is_object()) continue;
            const json::Value* ver = c.get("verified");
            if (ver && ver->is_bool() && !ver->b) continue;
            Cheat ch;
            ch.desc = c.str("desc");
            if (!format_from(c.str("format", "raw"), &ch.format)) continue;
            const json::Value* codes = c.get("codes");
            if (!codes || !codes->is_array()) continue;
            for (auto& x : codes->a) if (x.is_string() && !x.s.empty()) ch.codes.push_back(x.s);
            if (ch.desc.empty() || ch.codes.empty()) continue;
            ch.id = "feat-" + std::to_string(++n);
            f.cheats.push_back(std::move(ch));
        }
        if (!f.cheats.empty()) db->fidx[cheats_norm_key(kv.first)].push_back(std::move(f));
    }
}

Db* db() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_loaded) return g_db;
    g_loaded = true;
    g_db = new Db();
    std::string packed;
    if (slurp(dir() + "cheats_db.bin", &packed)) {
        size_t n = 0;
        void* p = tinfl_decompress_mem_to_heap(packed.data(), packed.size(), &n, TINFL_FLAG_PARSE_ZLIB_HEADER);
        if (p) {
            g_db->raw.assign((const char*)p, n);
            mz_free(p);
        }
    }
    const std::string& r = g_db->raw;
    if (r.compare(0, 7, "RHCHT1\n") == 0) {
        size_t pos = 7;
        Section* cur = nullptr;
        while (pos < r.size()) {
            size_t nl = r.find('\n', pos);
            if (nl == std::string::npos) nl = r.size();
            if (r[pos] == '@') {
                if (cur) cur->end = pos;
                std::string line = r.substr(pos + 1, nl - pos - 1);
                size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
                if (t2 != std::string::npos) {
                    Section s;
                    s.sys = line.substr(0, t1);
                    Format f = Format::Raw;
                    format_from(line.substr(t1 + 1, t2 - t1 - 1), &f);
                    s.fmt = f;
                    s.name = line.substr(t2 + 1);
                    s.region = cheats_region_of(s.name);
                    s.gc_rev = s.sys == "gc" ? gc_rev_of(s.name) : -1;
                    s.begin = nl + 1;
                    s.end = r.size();
                    g_db->sections.push_back(std::move(s));
                    cur = &g_db->sections.back();
                }
            }
            pos = nl + 1;
        }
        if (cur) cur->end = r.size();
        for (size_t i = 0; i < g_db->sections.size(); ++i) {
            auto& s = g_db->sections[i];
            if (s.sys == "gc") g_db->index["gc|" + gc_id_key(s.name)].push_back(i);   // by game ID: "GALE01r2" -> "gale01"
            else g_db->index[s.sys + "|" + cheats_norm_key(s.name)].push_back(i);
        }
    }
    std::string fj;
    if (slurp(dir() + "cheats_featured.json", &fj)) load_featured(g_db, fj);
    return g_db;
}

/* Region sets: empty = any/world. */
bool region_ok(const std::string& cht, const std::string& entry) {
    if (cht.empty() || entry.empty()) return true;
    for (char c : cht) if (entry.find(c) != std::string::npos) return true;
    return false;
}

std::string serial_region(System s, const std::string& serial) {
    if (serial.size() != 4) return "";
    if (s != System::N64 && s != System::GBA && s != System::GB && s != System::GBC) return "";
    switch (std::toupper((unsigned char)serial[3])) {
        case 'E': return "U";
        case 'J': return "J";
        case 'P': case 'D': case 'F': case 'I': case 'S': case 'X': case 'Y': case 'L': case 'H': case 'U': return "E";
    }
    return "";
}

}  // namespace

void cheats_db_set_dir(const std::string& d) {
    std::lock_guard<std::mutex> lk(g_mu);
    delete g_db;
    g_db = nullptr;
    g_loaded = false;
    g_dir = d;
    if (!g_dir.empty() && g_dir.back() != '/' && g_dir.back() != '\\') g_dir += '/';
}

size_t cheats_db_game_count() { return db()->sections.size(); }

CheatList cheats_list_for(const GameEntry& e) {
    CheatList out;
    const char* ss = system_short(e.system);
    std::string sys = ss ? ss : "";
    std::vector<std::string> dbsys;
    if (sys == "n64" || sys == "gba" || sys == "ps1" || sys == "gc") dbsys = {sys};
    else if (sys == "gb") dbsys = {"gb", "gbc"};
    else if (sys == "gbc") dbsys = {"gbc", "gb"};
    else return out;

    Db* d = hub::db();
    // Candidate names.
    std::vector<std::string> names;
    if (!e.catalog_id.empty())
        if (const GameInfo* gi = find_game(e.catalog_id)) {
            if (!gi->nointro.empty()) names.push_back(gi->nointro);
            names.push_back(gi->title);
        }
    names.push_back(e.title);
    if (!e.path.empty()) names.push_back(stem_of(e.path));
    if (!e.save_stem.empty()) names.push_back(e.save_stem);

    std::string region = serial_region(e.system, e.serial);
    if (region.empty())
        for (auto& n : names) {
            std::string r = cheats_region_of(n);
            for (char c : r) if (region.find(c) == std::string::npos) region += c;
        }

    std::set<std::string> keys;
    for (auto& n : names) {
        std::string k = cheats_norm_key(n);
        if (!k.empty()) keys.insert(k);
    }
    /* GameCube: the 6-char disc ID (header) is the key, for curated entries and for Dolphin's per-game sections. */
    std::string gc_id = e.system == System::GC ? lower(e.serial) : std::string();
    if (gc_id.size() >= 4 && gc_id.size() <= 6) {
        keys.insert(cheats_norm_key(gc_id));
        if (e.gc_rev >= 0) keys.insert(cheats_norm_key(gc_id + "r" + std::to_string(e.gc_rev)));   // curated "GALE01r2"
    } else gc_id.clear();
    if (e.system == System::GC) region.clear();   // IDs already pin the region

    std::vector<Cheat> found;
    std::set<std::string> seen;
    auto add = [&](Cheat c) {
        std::string sig = lower(c.desc);
        for (auto& x : c.codes) sig += "|" + x;
        if (seen.insert(sig).second) found.push_back(std::move(c));
    };

    std::vector<Cheat> feat;
    for (auto& k : keys) {
        auto fi = d->fidx.find(k);
        if (fi == d->fidx.end()) continue;
        for (auto& f : fi->second)
            if (region_ok(f.region, region))
                for (auto& c : f.cheats) feat.push_back(c);
    }

    for (auto& ds : dbsys) {
        std::vector<const Section*> hits;
        if (ds == "gc") {
            for (size_t n : {gc_id.size(), (size_t)4, (size_t)3}) {   // GALE01, then GALE and GAL (Dolphin merges the shorter files in)
                if (gc_id.empty() || n > gc_id.size()) continue;
                auto it = d->index.find("gc|" + gc_id.substr(0, n));
                if (it == d->index.end()) continue;
                for (size_t i : it->second) hits.push_back(&d->sections[i]);
            }
        } else
        for (auto& k : keys) {
            auto it = d->index.find(ds + "|" + k);
            if (it == d->index.end()) continue;
            for (size_t i : it->second) hits.push_back(&d->sections[i]);
        }
        // Region-unknown entries accept everything but only USA/world-tagged when such exist.
        std::vector<const Section*> pick;
        for (auto* s : hits) {
            if (!region_ok(s->region, region)) continue;
            /* Dolphin keeps one ini per disc revision ("GALE01r1"): their addresses differ, so a revision-specific list
             * is only offered when the disc's version byte is known and equal. */
            if (s->gc_rev >= 0 && s->gc_rev != e.gc_rev) continue;
            pick.push_back(s);
        }
        for (auto* s : pick) {
            size_t pos = s->begin;
            while (pos < s->end) {
                size_t nl = d->raw.find('\n', pos);
                if (nl == std::string::npos || nl > s->end) nl = s->end;
                std::string line = d->raw.substr(pos, nl - pos);
                pos = nl + 1;
                if (line.empty()) continue;
                Format f = s->fmt;
                if (line[0] == '!') {
                    size_t t = line.find('\t');
                    if (t == std::string::npos) continue;
                    format_from(line.substr(1, t - 1), &f);
                    line = line.substr(t + 1);
                }
                size_t t = line.find('\t');
                if (t == std::string::npos) continue;
                Cheat c;
                c.desc = line.substr(0, t);
                c.format = f;
                std::string codes = line.substr(t + 1), cur;
                for (size_t i = 0; i <= codes.size(); ++i) {
                    if (i == codes.size() || codes[i] == '+') {
                        if (!cur.empty()) c.codes.push_back(cur);
                        cur.clear();
                    } else cur += codes[i];
                }
                if (c.codes.empty()) continue;
                found.push_back(std::move(c));   // deduped below
            }
        }
        if (!found.empty()) break;   // GB: don't mix in GBC list when GB has hits
    }
    // Dedupe the db cheats, then split.
    std::vector<Cheat> db_cheats;
    std::swap(db_cheats, found);
    std::vector<Cheat> masters, pop, rest;
    auto place = [&](Cheat c, bool curated) {
        std::string sig = lower(c.desc);
        for (auto& x : c.codes) sig += "|" + x;
        if (!seen.insert(sig).second) return;
        if (cheats_is_master_desc(c.desc)) masters.push_back(std::move(c));
        else if (curated || cheats_is_featured_desc(c.desc)) pop.push_back(std::move(c));
        else rest.push_back(std::move(c));
    };
    seen.clear();
    std::set<std::string> curated_codes;   // a database entry with exactly a curated entry's codes is the same cheat under another name
    auto codes_sig = [](const Cheat& c) { std::string t = std::to_string((int)c.format); for (auto& x : c.codes) t += "|" + x; return t; };
    for (auto& c : feat) { curated_codes.insert(codes_sig(c)); place(std::move(c), true); }
    for (auto& c : db_cheats) { if (curated_codes.count(codes_sig(c))) continue; place(std::move(c), false); }
    out.masters = masters.size();
    out.featured = pop.size();
    int n = 0;
    for (auto* v : {&masters, &pop, &rest})
        for (auto& c : *v) {
            c.id = "db-" + std::to_string(++n);
            out.cheats.push_back(std::move(c));
        }
    return out;
}

std::vector<Cheat> cheats_for(const GameEntry& e, size_t* n_prefix) {
    CheatList l = cheats_list_for(e);
    if (n_prefix) *n_prefix = l.masters + l.featured;
    return std::move(l.cheats);
}

}  // namespace hub
