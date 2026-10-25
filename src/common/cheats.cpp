/* STUB (agent/cheat-ui): minimal implementation of the subset of cheats.h that the hub UI uses, so this branch builds
 * on its own. The real cheats.cpp (agent/cheat-engine) REPLACES this file on merge; keep the header identical. */
#include "cheats.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "json.h"

namespace cheats {

const char* format_name(Format f) {
    switch (f) {
        case Format::GameShark: return "gameshark";
        case Format::ActionReplay: return "ar";
        case Format::CodeBreaker: return "codebreaker";
        case Format::GameGenie: return "gamegenie";
        default: return "raw";
    }
}
bool parse_format(const std::string& s0, Format* out) {
    std::string s;
    for (char c : s0) s += (char)std::tolower((unsigned char)c);
    if (s == "gameshark" || s == "gs") *out = Format::GameShark;
    else if (s == "ar" || s == "actionreplay") *out = Format::ActionReplay;
    else if (s == "codebreaker" || s == "cb") *out = Format::CodeBreaker;
    else if (s == "gamegenie" || s == "gg") *out = Format::GameGenie;
    else if (s == "raw") *out = Format::Raw;
    else return false;
    return true;
}
bool target_for_system(const std::string& system, Target* out) {
    std::string s;
    for (char c : system) s += (char)std::tolower((unsigned char)c);
    if (s == "n64") *out = Target::N64;
    else if (s == "gba") *out = Target::GBA;
    else if (s == "gb" || s == "gbc") *out = Target::GB;
    else if (s == "nds") *out = Target::DS;
    else if (s == "ps1") *out = Target::PS1;
    else return false;
    return true;
}

bool parse_json(const std::string& text, CheatSet* out, std::string* err) {
    json::Value v;
    if (!json::parse(text, &v) || !v.is_object()) { if (err) *err = "malformed cheats file"; return false; }
    CheatSet s;
    s.version = (int)v.i64("version", 1);
    if (const json::Value* a = v.get("cheats"))
        for (auto& c : a->a) {
            Cheat ch;
            ch.id = c.str("id");
            ch.desc = c.str("desc");
            parse_format(c.str("format", "raw"), &ch.format);
            ch.enabled = c.boolean("enabled");
            if (const json::Value* codes = c.get("codes")) for (auto& x : codes->a) ch.codes.push_back(x.s);
            s.cheats.push_back(ch);
        }
    *out = s;
    return true;
}
std::string to_json(const CheatSet& set) {
    json::Value root = json::make_object();
    root.o.push_back({"version", json::make_int(set.version)});
    json::Value arr = json::make_array();
    for (auto& c : set.cheats) {
        json::Value o = json::make_object();
        o.o.push_back({"id", json::make_string(c.id)});
        o.o.push_back({"desc", json::make_string(c.desc)});
        o.o.push_back({"format", json::make_string(format_name(c.format))});
        json::Value codes = json::make_array();
        for (auto& l : c.codes) codes.a.push_back(json::make_string(l));
        o.o.push_back({"codes", codes});
        o.o.push_back({"enabled", json::make_bool(c.enabled)});
        arr.a.push_back(o);
    }
    root.o.push_back({"cheats", arr});
    return json::stringify(root);
}
bool load_file(const std::string& path, CheatSet* out, std::string* err) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) { if (err) *err = "cannot open"; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_json(ss.str(), out, err);
}
bool save_file(const std::string& path, const CheatSet& set, std::string* err) {
    namespace fs = std::filesystem;
    fs::path p = fs::u8path(path), tmp = p;
    tmp += ".tmp";
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "cannot write"; return false; }
        f << to_json(set);
    }
    fs::rename(tmp, p, ec);
    if (ec) { if (err) *err = ec.message(); return false; }
    return true;
}

bool normalize_code(Target, Format, const std::string& line, std::string* out, std::string*) {
    std::string o;
    for (char c : line) o += (char)std::toupper((unsigned char)c);
    *out = o;
    return true;
}

uint64_t set_hash(const CheatSet& set, Target) {
    std::vector<std::string> ls;
    for (auto& c : set.cheats) if (c.enabled) for (auto& l : c.codes) ls.push_back(l);
    if (ls.empty()) return kEmptyHash;
    std::sort(ls.begin(), ls.end());
    uint64_t h = kEmptyHash;
    for (auto& l : ls) { for (unsigned char ch : l) { h ^= ch; h *= 0x100000001B3ull; } h ^= 0xFF; h *= 0x100000001B3ull; }
    return h;
}

std::string FileWatch::stamp() const {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(std::filesystem::u8path(path_), ec);
    auto sz = std::filesystem::file_size(std::filesystem::u8path(path_), ec);
    return ec ? std::string() : std::to_string(t.time_since_epoch().count()) + ":" + std::to_string(sz);
}
bool FileWatch::changed() {
    std::string s = stamp();
    if (s == stamp_) return false;
    stamp_ = s;
    return true;
}

}  // namespace cheats
