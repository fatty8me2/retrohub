#include "dolphin_cheats.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace dolphincheats {

const char* const kOwnedPrefix = "RetroHub: ";

namespace {

struct Block {
    std::string header;   // the original "[Name]" line ("" = text before the first section)
    std::string name;     // "Name"
    std::vector<std::string> lines;
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

bool crlf(const std::string& t) { return t.find("\r\n") != std::string::npos; }

std::vector<Block> parse(const std::string& text) {
    std::vector<Block> v(1);
    std::string cur;
    auto push_line = [&](std::string line) {
        std::string t = trim(line);
        if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
            Block b;
            b.header = t;
            b.name = t.substr(1, t.size() - 2);
            v.push_back(std::move(b));
        } else v.back().lines.push_back(std::move(line));
    };
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            push_line(cur);
            cur.clear();
        } else cur += text[i];
    }
    if (!cur.empty()) push_line(cur);
    return v;
}

std::string join(const std::vector<Block>& v, bool use_crlf) {
    const char* eol = use_crlf ? "\r\n" : "\n";
    std::string out;
    for (const Block& b : v) {
        if (!b.header.empty()) out += b.header + eol;
        for (const std::string& l : b.lines) out += l + eol;
    }
    return out;
}

bool is_code_section(const std::string& n) {
    return n == "Gecko" || n == "Gecko_Enabled" || n == "ActionReplay" || n == "ActionReplay_Enabled";
}

bool owned_name_line(const std::string& line) {
    std::string t = trim(line);
    const size_t n = std::string(kOwnedPrefix).size();
    return t.size() > 1 + n && t[0] == '$' && t.compare(1, n, kOwnedPrefix) == 0;
}

/* Remove every RetroHub entry ($name line and the lines under it) from the four code sections. */
void strip_owned(std::vector<Block>* v) {
    for (Block& b : *v) {
        if (!is_code_section(b.name)) continue;
        std::vector<std::string> keep;
        bool dropping = false;
        for (const std::string& l : b.lines) {
            std::string t = trim(l);
            if (!t.empty() && t[0] == '$') dropping = owned_name_line(l);
            if (!dropping) keep.push_back(l);
        }
        b.lines = std::move(keep);
    }
}

std::string clean_name(const std::string& desc) {
    std::string n;
    for (char c : desc) n += (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    n = trim(n);
    while (!n.empty() && n[0] == '$') n.erase(n.begin());
    n = trim(n);
    if (n.size() > 80) n.resize(80);
    if (n.empty()) n = "Cheat";
    return n;
}

Block* find_block(std::vector<Block>* v, const std::string& name) {
    for (size_t i = v->size(); i-- > 0;)
        if ((*v)[i].name == name && !(*v)[i].header.empty()) return &(*v)[i];
    return nullptr;
}

/* Append lines to a section (created at the end of the file when missing), before its trailing blank lines. */
void add_lines(std::vector<Block>* v, const std::string& name, const std::vector<std::string>& add) {
    if (add.empty()) return;
    Block* b = find_block(v, name);
    if (!b) {
        Block nb;
        nb.header = "[" + name + "]";
        nb.name = name;
        v->push_back(std::move(nb));
        b = &v->back();
    }
    size_t at = b->lines.size();
    while (at > 0 && trim(b->lines[at - 1]).empty()) --at;
    b->lines.insert(b->lines.begin() + at, add.begin(), add.end());
}

}  // namespace

std::string patch_game_ini(const std::string& ini, const cheats::CheatSet& enabled, int* added) {
    bool cr = crlf(ini);
    std::vector<Block> v = parse(ini);
    strip_owned(&v);
    std::vector<std::string> gecko, gecko_on, ar, ar_on;
    int n = 0;
    for (const cheats::Cheat& c : enabled.cheats) {
        if (c.codes.empty()) continue;
        std::vector<std::string> lines;
        bool ok = true;
        for (const std::string& raw : c.codes) {
            std::string t;
            if (!cheats::normalize_code(cheats::Target::GC, c.format, raw, &t, nullptr)) { ok = false; break; }
            lines.push_back(t);
        }
        if (!ok) continue;
        bool is_gecko = c.format == cheats::Format::Gecko;
        std::string name = "$" + std::string(kOwnedPrefix) + clean_name(c.desc);
        auto& body = is_gecko ? gecko : ar;
        auto& on = is_gecko ? gecko_on : ar_on;
        body.push_back(name);
        for (auto& l : lines) body.push_back(l);
        on.push_back(name);
        ++n;
    }
    add_lines(&v, "ActionReplay", ar);
    add_lines(&v, "ActionReplay_Enabled", ar_on);
    add_lines(&v, "Gecko", gecko);
    add_lines(&v, "Gecko_Enabled", gecko_on);
    if (added) *added = n;
    return join(v, cr);
}

int count_owned(const std::string& ini) {
    int n = 0;
    for (const Block& b : parse(ini))
        if (b.name == "Gecko" || b.name == "ActionReplay")
            for (const std::string& l : b.lines) if (owned_name_line(l)) ++n;
    return n;
}

std::string enable_cheats(const std::string& dolphin_ini) {
    bool cr = crlf(dolphin_ini);
    std::vector<Block> v = parse(dolphin_ini);
    Block* core = find_block(&v, "Core");
    if (!core) {
        Block nb;
        nb.header = "[Core]";
        nb.name = "Core";
        v.push_back(std::move(nb));
        core = &v.back();
    }
    bool done = false;
    for (std::string& l : core->lines) {
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        if (lower(trim(l.substr(0, eq))) == "enablecheats") { l = "EnableCheats = True"; done = true; }
    }
    if (!done) {
        size_t at = core->lines.size();
        while (at > 0 && trim(core->lines[at - 1]).empty()) --at;
        core->lines.insert(core->lines.begin() + at, "EnableCheats = True");
    }
    return join(v, cr);
}

std::string game_ini_name(const std::string& id) {
    if (id.size() < 4 || id.size() > 6) return std::string();
    for (char c : id) if (!std::isalnum((unsigned char)c)) return std::string();
    return id + ".ini";
}

namespace {
bool slurp(const fs::path& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}
bool spit(const fs::path& p, const std::string& text, std::string* err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "cannot write " + p.u8string(); return false; }
    f.write(text.data(), (std::streamsize)text.size());
    if (!f) { if (err) *err = "write failed: " + p.u8string(); return false; }
    return true;
}
}  // namespace

int apply_for_game(const std::string& user_dir, const std::string& game_id, const cheats::CheatSet& enabled, std::string* err) {
    std::string name = game_ini_name(game_id);
    if (name.empty()) { if (err) *err = "no GameCube disc ID"; return -1; }
    fs::path gini = fs::u8path(user_dir) / "GameSettings" / fs::u8path(name);
    std::string cur;
    bool existed = slurp(gini, &cur);
    int added = 0;
    std::string next = patch_game_ini(cur, enabled, &added);
    if (!existed && added == 0) return 0;               // nothing to write, nothing to clean
    if (!existed || next != cur) { if (!spit(gini, next, err)) return -1; }
    if (added > 0) {
        fs::path dini = fs::u8path(user_dir) / "Config" / "Dolphin.ini";
        std::string dcur;
        slurp(dini, &dcur);
        std::string dnext = enable_cheats(dcur);
        if (dnext != dcur && !spit(dini, dnext, err)) return -1;
    }
    return added;
}

}  // namespace dolphincheats
