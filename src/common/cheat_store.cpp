#include "cheat_store.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "json.h"

namespace cheatstore {
namespace fs = std::filesystem;

/* ---------------- store ---------------- */

bool Store::is_on(const std::string& game, const std::string& id) const {
    auto it = g_.find(game);
    return it != g_.end() && it->second.on.count(id) > 0;
}
int Store::on_count(const std::string& game) const {
    auto it = g_.find(game);
    return it == g_.end() ? 0 : (int)it->second.on.size();
}
void Store::set_on(const std::string& game, const std::string& id, bool on, int64_t now) {
    if (game.empty() || id.empty()) return;
    GameState& s = g_[game];
    if (on) s.on.insert(id); else s.on.erase(id);
    s.updated = now;
}
const GameState* Store::game(const std::string& game) const {
    auto it = g_.find(game);
    return it == g_.end() ? nullptr : &it->second;
}
std::string Store::add_custom(const std::string& game, const std::string& desc, const std::string& text,
                              cheats::Target target, int64_t now, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return std::string(); };
    if (game.empty()) return fail("No game selected.");
    std::string d = desc;
    while (!d.empty() && std::isspace((unsigned char)d.back())) d.pop_back();
    while (!d.empty() && std::isspace((unsigned char)d.front())) d.erase(d.begin());
    if (d.empty()) return fail("Give the cheat a name.");
    if (d.size() > 80) d.resize(80);
    std::vector<std::string> lines = split_codes(text);
    if (lines.empty()) return fail("Type or paste the code first.");
    if (lines.size() > 64) return fail("Too many lines in one cheat (64 max).");
    cheats::Format fmt = cheats::Format::Raw;
    for (size_t i = 0; i < lines.size(); ++i) {
        cheats::Format f;
        if (!detect_format(target, lines[i], &f)) return fail("That does not look like a valid code for this console.");
        if (i && f != fmt) return fail("All lines of one cheat must use the same code format.");
        fmt = f;
    }
    GameState& s = g_[game];
    std::string base = d;
    uint64_t h = 1469598103934665603ull;
    for (auto& l : lines) for (unsigned char c : l) { h ^= c; h *= 1099511628211ull; }
    for (unsigned char c : base) { h ^= c; h *= 1099511628211ull; }
    char buf[40];
    std::snprintf(buf, sizeof buf, "custom-%08x", (unsigned)(h ^ (h >> 32)));
    std::string id = buf;
    for (auto& c : s.custom) if (c.id == id) return id;   // same name + codes: already there
    Custom c;
    c.id = id; c.desc = d; c.format = fmt; c.codes = lines;
    s.custom.push_back(c);
    s.on.insert(id);   // a code you just added is one you want on
    s.updated = now;
    return id;
}
bool Store::value_of(const std::string& game, const std::string& id, uint32_t* out) const {
    auto it = g_.find(game);
    if (it == g_.end()) return false;
    auto v = it->second.values.find(id);
    if (v == it->second.values.end()) return false;
    *out = v->second;
    return true;
}
void Store::set_value(const std::string& game, const std::string& id, uint32_t v, int64_t now) {
    if (game.empty() || id.empty()) return;
    GameState& s = g_[game];
    s.values[id] = v;
    s.updated = now;
}
void Store::remove_custom(const std::string& game, const std::string& id, int64_t now) {
    auto it = g_.find(game);
    if (it == g_.end()) return;
    auto& v = it->second.custom;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Custom& c) { return c.id == id; }), v.end());
    it->second.on.erase(id);
    it->second.updated = now;
}
void Store::set_backup(const std::string& game, const std::string& name, int64_t now) {
    GameState& s = g_[game];
    s.backup = name;
    s.updated = now;
}

std::string Store::file_path(const std::string& vault_dir) {
    std::string d = vault_dir;
    if (!d.empty() && d.back() != '\\' && d.back() != '/') d += '\\';
    return d + "_settings\\cheats.json";
}

std::string Store::to_json() const {
    json::Value root = json::make_object();
    json::Value games = json::make_object();
    for (auto& kv : g_) {
        const GameState& s = kv.second;
        if (s.on.empty() && s.custom.empty() && s.backup.empty() && s.values.empty()) continue;
        json::Value o = json::make_object();
        json::Value on = json::make_array();
        for (auto& id : s.on) on.a.push_back(json::make_string(id));
        o.o.push_back({"on", on});
        json::Value cu = json::make_array();
        for (auto& c : s.custom) {
            json::Value x = json::make_object();
            x.o.push_back({"id", json::make_string(c.id)});
            x.o.push_back({"desc", json::make_string(c.desc)});
            x.o.push_back({"format", json::make_string(cheats::format_name(c.format))});
            json::Value codes = json::make_array();
            for (auto& l : c.codes) codes.a.push_back(json::make_string(l));
            x.o.push_back({"codes", codes});
            cu.a.push_back(x);
        }
        o.o.push_back({"custom", cu});
        if (!s.values.empty()) {
            json::Value vals = json::make_object();
            for (auto& kv2 : s.values) vals.o.push_back({kv2.first, json::make_int((int64_t)kv2.second)});
            o.o.push_back({"vals", vals});
        }
        if (!s.backup.empty()) o.o.push_back({"backup", json::make_string(s.backup)});
        o.o.push_back({"t", json::make_int(s.updated)});
        games.o.push_back({kv.first, o});
    }
    root.o.push_back({"games", games});
    return json::stringify(root);
}

bool Store::from_json(const std::string& text) {
    json::Value v;
    if (!json::parse(text, &v) || !v.is_object()) return false;
    std::map<std::string, GameState> out;
    if (const json::Value* games = v.get("games")) {
        if (!games->is_object()) return false;
        for (auto& kv : games->o) {
            if (!kv.second.is_object() || kv.first.empty()) continue;
            GameState s;
            if (const json::Value* on = kv.second.get("on")) for (auto& x : on->a) if (x.is_string() && !x.s.empty()) s.on.insert(x.s);
            if (const json::Value* cu = kv.second.get("custom"))
                for (auto& x : cu->a) {
                    Custom c;
                    c.id = x.str("id"); c.desc = x.str("desc");
                    if (c.id.empty() || !cheats::parse_format(x.str("format", "raw"), &c.format)) continue;
                    if (const json::Value* codes = x.get("codes")) for (auto& l : codes->a) if (l.is_string()) c.codes.push_back(l.s);
                    if (c.codes.empty()) continue;
                    s.custom.push_back(c);
                }
            if (const json::Value* vals = kv.second.get("vals"))
                if (vals->is_object())
                    for (auto& x : vals->o)
                        if (x.second.is_number() && x.second.i >= 0 && x.second.i <= 0xFFFFFFFFll) s.values[x.first] = (uint32_t)x.second.i;
            s.backup = kv.second.str("backup");
            s.updated = kv.second.i64("t", 0);
            out[kv.first] = s;
        }
    }
    g_ = out;
    return true;
}

void Store::merge_from(const Store& other) {
    for (auto& kv : other.g_) {
        auto it = g_.find(kv.first);
        if (it == g_.end()) { g_[kv.first] = kv.second; continue; }
        GameState& mine = it->second;
        const GameState& theirs = kv.second;
        std::string keep_backup = mine.backup.empty() ? theirs.backup : mine.backup;   // a backup record is never lost
        if (theirs.updated > mine.updated) mine = theirs;
        else if (theirs.updated == mine.updated) {   // tie: union, so neither PC loses a custom code
            for (auto& id : theirs.on) mine.on.insert(id);
            for (auto& c : theirs.custom) {
                bool have = false;
                for (auto& m : mine.custom) if (m.id == c.id) have = true;
                if (!have) mine.custom.push_back(c);
            }
        }
        if (mine.backup.empty()) mine.backup = keep_backup;
    }
}

bool Store::load(const std::string& vault_dir) {
    std::ifstream f(fs::u8path(file_path(vault_dir)), std::ios::binary);
    if (!f) { g_.clear(); return true; }
    std::stringstream ss;
    ss << f.rdbuf();
    return from_json(ss.str());
}
bool Store::save(const std::string& vault_dir) const {
    fs::path p = fs::u8path(file_path(vault_dir)), tmp = p;
    tmp += ".tmp";
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << to_json();
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

/* ---------------- rows ---------------- */

std::vector<Row> build_rows(const Store& st, const std::string& game, const std::vector<DbItem>& db, cheats::Target target) {
    std::vector<Row> pop, all, mine;
    for (auto& d : db) {
        Row r;
        r.cheat = d.cheat;
        r.cheat.enabled = st.is_on(game, d.cheat.id);
        r.featured = d.featured;
        r.group = d.featured ? 0 : 1;
        r.target = target;
        r.spec = cheats::value_spec(d.cheat, target);
        if (r.spec.valid) {
            uint32_t v = 0;
            r.value_set = st.value_of(game, d.cheat.id, &v);
            r.value = cheats::value_clamp(r.spec, r.value_set ? v : r.spec.min);
        }
        (d.featured ? pop : all).push_back(r);
    }
    if (const GameState* g = st.game(game))
        for (auto& c : g->custom) {
            Row r;
            r.cheat.id = c.id; r.cheat.desc = c.desc; r.cheat.format = c.format; r.cheat.codes = c.codes;
            r.cheat.enabled = g->on.count(c.id) > 0;
            r.custom = true;
            r.group = 2;
            mine.push_back(r);
        }
    pop.insert(pop.end(), all.begin(), all.end());
    pop.insert(pop.end(), mine.begin(), mine.end());
    return pop;
}

static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::vector<Row> filter_rows(const std::vector<Row>& rows, const std::string& q0) {
    std::string q = lower(q0);
    while (!q.empty() && q.back() == ' ') q.pop_back();
    while (!q.empty() && q.front() == ' ') q.erase(q.begin());
    if (q.empty()) return rows;
    std::vector<Row> out;
    for (auto& r : rows) if (lower(r.cheat.desc).find(q) != std::string::npos) out.push_back(r);
    return out;
}

cheats::Cheat effective_cheat(const Row& r) {
    cheats::Cheat out;
    if (r.spec.valid && cheats::value_apply(r.cheat, r.value, r.target, &out)) return out;
    return r.cheat;
}

cheats::CheatSet runner_set(const std::vector<Row>& rows) {
    cheats::CheatSet s;
    for (auto& r : rows) s.cheats.push_back(effective_cheat(r));
    return s;
}
cheats::CheatSet enabled_only(const std::vector<Row>& rows) {
    cheats::CheatSet s;
    for (auto& r : rows) if (r.cheat.enabled) s.cheats.push_back(effective_cheat(r));
    return s;
}
std::vector<std::string> enabled_names(const std::vector<Row>& rows) {
    std::vector<std::string> v;
    for (auto& r : rows) if (r.cheat.enabled) v.push_back(r.cheat.desc);
    return v;
}

/* ---------------- custom codes ---------------- */

std::vector<std::string> split_codes(const std::string& text) {
    std::vector<std::string> out;
    std::string line;
    auto flush = [&]() {
        std::string t;
        bool sp = false;
        for (char c : line) {
            if (c == ':' || c == '\t' || c == ' ') { sp = true; continue; }
            if (sp && !t.empty()) t += ' ';
            sp = false;
            t += (char)std::toupper((unsigned char)c);
        }
        if (!t.empty()) out.push_back(t);
        line.clear();
    };
    for (char c : text) {
        if (c == '\n' || c == '\r' || c == ',' || c == ';') flush();
        else line += c;
    }
    flush();
    return out;
}

static bool all_hex(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}

bool detect_format(cheats::Target t, const std::string& line, cheats::Format* out) {
    using cheats::Format;
    using cheats::Target;
    size_t sp = line.find(' ');
    std::string a = line.substr(0, sp), b = sp == std::string::npos ? std::string() : line.substr(sp + 1);
    if (b.find(' ') != std::string::npos) return false;
    if (t == Target::GC) {
        if (sp == std::string::npos) { if (cheats::gc_is_encrypted_ar(line)) { *out = Format::ActionReplayGC; return true; } return false; }
        if (a.size() == 8 && all_hex(a) && b.size() == 8 && all_hex(b)) { *out = Format::Gecko; return true; }   // 8+8 is ambiguous with decrypted AR; Gecko is the common case
        return false;
    }
    if (t == Target::GB && sp == std::string::npos) {
        if (a.size() == 8 && all_hex(a)) { *out = Format::GameShark; return true; }
        // Game Genie: XXX-XXX or XXX-XXX-XXX
        if ((a.size() == 7 || a.size() == 11) && a[3] == '-' && (a.size() == 7 || a[7] == '-')) {
            std::string h;
            for (char c : a) if (c != '-') h += c;
            if (all_hex(h)) { *out = Format::GameGenie; return true; }
        }
        return false;
    }
    if (sp == std::string::npos || a.size() != 8 || !all_hex(a)) return false;
    if (b.size() == 4 && all_hex(b)) {
        if (t == Target::N64 || t == Target::PS1) { *out = Format::GameShark; return true; }
        if (t == Target::GBA) { *out = Format::CodeBreaker; return true; }
        return false;
    }
    if (b.size() == 8 && all_hex(b) && (t == Target::GBA || t == Target::DS)) { *out = Format::ActionReplay; return true; }
    return false;
}

/* ---------------- online ---------------- */

std::string online_block(int enabled_count, const std::string& mine, const std::vector<std::string>& peers) {
    if (enabled_count <= 0) return "";
    for (auto& p : peers) {
        if (p.empty()) return "Cheats are on, but a player's RetroHub is too old to use them. Update everyone, or turn the cheats off.";
        if (p != mine) return "Cheats are on, but a player runs RetroHub " + p + " (yours is " + mine + "). Everyone needs the same version for cheats.";
    }
    return "";
}

std::string summary_line(const std::vector<std::string>& names, size_t max_names) {
    if (names.empty()) return "";
    std::string s = "Cheats on: ";
    for (size_t i = 0; i < names.size() && i < max_names; ++i) { if (i) s += ", "; s += names[i]; }
    if (names.size() > max_names) s += " (+" + std::to_string(names.size() - max_names) + " more)";
    return s;
}

bool extract_launch(const std::string& body, cheats::CheatSet* out) {
    json::Value v;
    if (!json::parse(body, &v) || !v.is_object()) return false;
    const json::Value* c = v.get("cheats");
    if (!c || !c->is_object()) return false;
    return cheats::parse_json(json::stringify(*c), out, nullptr);
}
std::string launch_fragment(const cheats::CheatSet& e) {
    if (e.cheats.empty()) return "";
    return ",\"cheats\":" + cheats::to_json(e);
}

/* ---------------- save safety ---------------- */

static fs::path backup_dir(const std::string& game_dir, const std::string& name) {
    return fs::u8path(game_dir) / "cheat_backups" / fs::u8path(name);
}

static int copy_tree(const fs::path& from, const fs::path& to, std::string* err) {
    std::error_code ec;
    if (!fs::exists(from, ec)) return 0;
    fs::create_directories(to, ec);
    int n = 0;
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        fs::path rel = fs::relative(it->path(), from, ec);
        if (it->is_directory(ec)) { fs::create_directories(to / rel, ec); continue; }
        fs::create_directories((to / rel).parent_path(), ec);
        fs::copy_file(it->path(), to / rel, fs::copy_options::overwrite_existing, ec);
        if (ec) { if (err) *err = ec.message(); return -1; }
        ++n;
    }
    return n;
}

bool backup_save(const std::string& game_dir, const std::string& battery, const std::string& stamp, std::string* err) {
    std::error_code ec;
    fs::path dst = backup_dir(game_dir, stamp);
    fs::create_directories(dst, ec);
    return copy_tree(fs::u8path(battery), dst, err) >= 0;
}

bool has_backup(const std::string& game_dir, const std::string& name) {
    std::error_code ec;
    return !name.empty() && fs::is_directory(backup_dir(game_dir, name), ec);
}

int restore_save(const std::string& game_dir, const std::string& battery, const std::string& name,
                 const std::string& undo_stamp, std::string* err) {
    if (!has_backup(game_dir, name)) { if (err) *err = "No backup found."; return -1; }
    if (!backup_save(game_dir, battery, undo_stamp + "-before-restore", err)) return -1;
    return copy_tree(backup_dir(game_dir, name), fs::u8path(battery), err);
}

}  // namespace cheatstore
