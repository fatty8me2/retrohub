#include "cheats.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "json.h"

namespace fs = std::filesystem;

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

static std::string lower_s(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static std::string upper_s(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }

bool parse_format(const std::string& in, Format* out) {
    std::string s = lower_s(in);
    if (s == "gameshark" || s == "gs") *out = Format::GameShark;
    else if (s == "ar" || s == "actionreplay" || s == "action replay") *out = Format::ActionReplay;
    else if (s == "codebreaker" || s == "cb") *out = Format::CodeBreaker;
    else if (s == "gamegenie" || s == "gg") *out = Format::GameGenie;
    else if (s == "raw") *out = Format::Raw;
    else return false;
    return true;
}

bool target_for_system(const std::string& system, Target* out) {
    std::string s = lower_s(system);
    if (s == "n64") *out = Target::N64;
    else if (s == "gba") *out = Target::GBA;
    else if (s == "gb" || s == "gbc") *out = Target::GB;
    else if (s == "nds" || s == "ds") *out = Target::DS;
    else if (s == "ps1" || s == "psx") *out = Target::PS1;
    else return false;
    return true;
}

/* ---------------------------------------------------------------- JSON */

bool parse_json(const std::string& text, CheatSet* out, std::string* err) {
    json::Value v;
    if (!json::parse(text, &v) || !v.is_object()) { if (err) *err = "not a JSON object"; return false; }
    CheatSet set;
    set.version = (int)v.i64("version", 1);
    const json::Value* arr = v.get("cheats");
    if (!arr || !arr->is_array()) { if (err) *err = "missing \"cheats\" array"; return false; }
    int idx = 0;
    for (const json::Value& c : arr->a) {
        idx++;
        if (!c.is_object()) continue;
        Cheat ch;
        ch.id = c.str("id");
        if (ch.id.empty()) ch.id = "cheat" + std::to_string(idx);
        bool dup = false;
        for (auto& e : set.cheats) dup |= e.id == ch.id;
        if (dup) continue;   // first wins
        ch.desc = c.str("desc");
        std::string f = c.str("format", "raw");
        if (!parse_format(f, &ch.format)) ch.format = Format::Raw;
        ch.enabled = c.boolean("enabled", false);
        ch.fire = (int)c.i64("fire", 0);
        if (const json::Value* codes = c.get("codes")) {
            if (codes->is_array()) { for (auto& cv : codes->a) if (cv.is_string()) ch.codes.push_back(cv.s); }
            else if (codes->is_string()) ch.codes.push_back(codes->s);
        }
        set.cheats.push_back(std::move(ch));
    }
    *out = std::move(set);
    return true;
}

std::string to_json(const CheatSet& set) {
    json::Value root = json::make_object();
    root.o.push_back({"version", json::make_int(set.version)});
    json::Value arr = json::make_array();
    for (const Cheat& c : set.cheats) {
        json::Value o = json::make_object();
        o.o.push_back({"id", json::make_string(c.id)});
        o.o.push_back({"desc", json::make_string(c.desc)});
        o.o.push_back({"format", json::make_string(format_name(c.format))});
        json::Value codes = json::make_array();
        for (auto& s : c.codes) codes.a.push_back(json::make_string(s));
        o.o.push_back({"codes", std::move(codes)});
        o.o.push_back({"enabled", json::make_bool(c.enabled)});
        if (c.fire > 0) o.o.push_back({"fire", json::make_int(c.fire)});
        arr.a.push_back(std::move(o));
    }
    root.o.push_back({"cheats", std::move(arr)});
    return json::stringify(root);
}

bool load_file(const std::string& path, CheatSet* out, std::string* err) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_json(ss.str(), out, err);
}

bool save_file(const std::string& path, const CheatSet& set, std::string* err) {
    fs::path p = fs::u8path(path), tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "cannot write " + tmp.u8string(); return false; }
        std::string t = to_json(set);
        f.write(t.data(), (std::streamsize)t.size());
        if (!f) { if (err) *err = "write failed"; return false; }
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {   // rename over an existing file can fail on Windows while a reader has it open
        fs::remove(p, ec);
        ec.clear();
        fs::rename(tmp, p, ec);
        if (ec) { if (err) *err = ec.message(); return false; }
    }
    return true;
}

/* ---------------------------------------------------------------- code validation */

static bool is_hex(const std::string& s, size_t from, size_t n) {
    if (from + n > s.size()) return false;
    for (size_t i = from; i < from + n; i++) if (!std::isxdigit((unsigned char)s[i])) return false;
    return true;
}

/* Strip everything except hex digits/dashes: "8033b21e:0008", "8033B21E  0008" -> "8033B21E0008". */
static std::string squeeze(const std::string& line) {
    std::string r;
    for (char c : line) if (!std::isspace((unsigned char)c) && c != ':' && c != ',') r += (char)std::toupper((unsigned char)c);
    return r;
}

/* hex word pair "AAAAAAAA VVVV" */
static bool pair_code(const std::string& line, size_t alen, size_t vlen, std::string* out) {
    std::string s = squeeze(line);
    if (s.size() != alen + vlen || !is_hex(s, 0, s.size())) return false;
    *out = s.substr(0, alen) + " " + s.substr(alen);
    return true;
}

static bool gg_code(const std::string& line, std::string* out) {
    std::string s = squeeze(line);   // "ABC-DEF" or "ABC-DEF-GHI"
    if (s.size() != 7 && s.size() != 11) return false;
    if (s[3] != '-' || (s.size() == 11 && s[7] != '-')) return false;
    for (size_t i = 0; i < s.size(); i++) if (i != 3 && i != 7 && !std::isxdigit((unsigned char)s[i])) return false;
    *out = s;
    return true;
}

bool normalize_code(Target target, Format fmt, const std::string& line, std::string* out, std::string* err) {
    auto bad = [&](const char* why) { if (err) *err = std::string(why) + ": '" + line + "'"; return false; };
    std::string r;
    switch (target) {
        case Target::N64:
            if (fmt == Format::CodeBreaker || fmt == Format::GameGenie) return bad("only GameShark codes work on N64");
            if (!pair_code(line, 8, 4, &r)) return bad("expected AAAAAAAA VVVV");
            break;
        case Target::GBA:
            if (fmt == Format::GameGenie) return bad("Game Genie codes are not supported on GBA");
            if (fmt == Format::CodeBreaker) { if (!pair_code(line, 8, 4, &r)) return bad("expected AAAAAAAA VVVV (CodeBreaker)"); }
            else if (fmt == Format::Raw) { if (!pair_code(line, 8, 8, &r) && !pair_code(line, 8, 4, &r)) return bad("expected AAAAAAAA VVVVVVVV"); }
            else if (!pair_code(line, 8, 8, &r)) return bad("expected AAAAAAAA VVVVVVVV");
            break;
        case Target::GB:
            if (fmt == Format::GameGenie) { if (!gg_code(line, &r)) return bad("expected ABC-DEF or ABC-DEF-GHI"); }
            else if (fmt == Format::CodeBreaker) return bad("CodeBreaker codes are not supported on Game Boy");
            else {
                std::string s = squeeze(line);
                if (s.size() == 8 && is_hex(s, 0, 8)) r = s;
                else if (fmt == Format::Raw && gg_code(line, &r)) {}
                else return bad("expected 8 hex digits (GameShark 01VVAAAA)");
            }
            break;
        case Target::DS:
            if (fmt == Format::GameGenie || fmt == Format::CodeBreaker) return bad("only Action Replay codes work on DS");
            if (!pair_code(line, 8, 8, &r)) return bad("expected AAAAAAAA VVVVVVVV");
            break;
        case Target::PS1:
            if (fmt == Format::GameGenie || fmt == Format::CodeBreaker) return bad("only GameShark/Action Replay codes work on PS1");
            if (!pair_code(line, 8, 4, &r)) return bad("expected AAAAAAAA VVVV");
            break;
    }
    *out = r;
    return true;
}

/* ---------------------------------------------------------------- N64 */

static bool n64_type_ok(unsigned t) {
    switch (t) {
        case 0x50: case 0x80: case 0x81: case 0x88: case 0x89: case 0xA0: case 0xA1:
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xF0: case 0xF1: return true;
        default: return false;
    }
}

/* Split one stored code line into single codes: databases join several with ';' (or '+', ',', newline). */
static std::vector<std::string> n64_pieces(const std::string& line) {
    std::vector<std::string> r;
    std::string cur;
    for (size_t i = 0; i <= line.size(); i++) {
        if (i == line.size() || line[i] == ';' || line[i] == '+' || line[i] == ',' || line[i] == '\n' || line[i] == '\r') {
            size_t a = cur.find_first_not_of(" \t");
            if (a != std::string::npos) r.push_back(cur.substr(a, cur.find_last_not_of(" \t") - a + 1));
            cur.clear();
        } else cur += line[i];
    }
    return r;
}

/* Types with no effect in an emulator (cartridge/device control: Datel enable/boot/capture/padding). Dropped. */
static bool n64_type_noop(unsigned t) { return t == 0x00 || t == 0xDE || t == 0xEE || t == 0xFF; }
static bool n64_type_cond(unsigned t) { return t == 0x50 || (t >= 0xD0 && t <= 0xD3); }

bool n64_codes(const Cheat& c, std::vector<N64Code>* out, std::string* err) {
    std::vector<N64Code> r;
    bool any_line = false;
    for (const std::string& raw : c.codes) {
        for (const std::string& line : n64_pieces(raw)) {
            any_line = true;
            std::string sq = squeeze(line);
            if (sq.size() >= 12 && sq.find('X') != std::string::npos && is_hex(sq, 0, 8)) {
                if (err) *err = "needs a value typed in (XXXX placeholder): '" + line + "'";
                return false;
            }
            /* "DE000400 EE000000": two device-control words run together. */
            if (sq.size() == 16 && is_hex(sq, 0, 16) && n64_type_noop((unsigned)std::stoul(sq.substr(0, 2), nullptr, 16))) {
                if (!r.empty() && n64_type_cond(r.back().address >> 24)) { if (err) *err = "device-control code after a conditional: '" + line + "'"; return false; }
                continue;
            }
            std::string t;
            if (!normalize_code(Target::N64, c.format, line, &t, err)) return false;
            N64Code nc;
            nc.address = (uint32_t)std::stoul(t.substr(0, 8), nullptr, 16);
            nc.value = (int)std::stoul(t.substr(9), nullptr, 16);
            unsigned type = nc.address >> 24;
            if (n64_type_noop(type)) {
                if (!r.empty() && n64_type_cond(r.back().address >> 24)) { if (err) *err = "device-control code after a conditional: '" + line + "'"; return false; }
                continue;
            }
            if (!n64_type_ok(type)) {
                if (err) { char b[96]; std::snprintf(b, sizeof b, "unsupported GameShark code type %02X in '%s'", type, line.c_str()); *err = b; }
                return false;
            }
            if ((type == 0x88 || type == 0x89) && !r.empty() && r.back().address >> 24 == 0x50) {
                if (err) *err = "repeater over a GameShark-button code is not supported: '" + line + "'";
                return false;
            }
            r.push_back(nc);
        }
    }
    if (!any_line) { if (err) *err = "no codes"; return false; }
    *out = std::move(r);   // may be empty: every line was a device-control code (nothing for an emulator to do)
    return true;
}

bool n64_needs_gs_button(const std::vector<N64Code>& codes) {
    for (auto& c : codes) { unsigned t = c.address >> 24; if (t == 0x88 || t == 0x89) return true; }
    return false;
}

bool n64_is_button_cheat(const Cheat& c) {
    std::vector<N64Code> v;
    return n64_codes(c, &v, nullptr) && n64_needs_gs_button(v);
}

void n64_split_button(const std::vector<N64Code>& codes, std::vector<N64Code>* core, std::vector<N64Code>* once) {
    core->clear();
    once->clear();
    for (const N64Code& c : codes) {
        unsigned t = c.address >> 24;
        if (t == 0x88 || t == 0x89) {
            if (!core->empty() && n64_type_cond(core->back().address >> 24)) {   // its guard goes with it
                once->insert(once->begin() + 0, core->back());
                core->pop_back();
            }
            once->push_back(c);
        } else core->push_back(c);
    }
}

/* ---------------------------------------------------------------- libretro */

bool retro_code_string(Target target, const Cheat& c, std::string* out, std::string* err) {
    std::string joined;
    for (const std::string& line : c.codes) {
        std::string t;
        if (!normalize_code(target, c.format, line, &t, err)) return false;
        if (!joined.empty()) joined += '+';
        joined += t;
    }
    if (joined.empty()) { if (err) *err = "no codes"; return false; }
    *out = joined;
    return true;
}

/* ---------------------------------------------------------------- hash */

uint64_t set_hash(const CheatSet& set, Target target) {
    std::vector<std::string> items;
    for (const Cheat& c : set.cheats) {
        if (!c.enabled) continue;
        std::string item = std::string(format_name(c.format)) + "|";
        for (const std::string& line : c.codes) {
            std::string t;
            item += normalize_code(target, c.format, line, &t, nullptr) ? t : "?" + upper_s(line);
            item += ";";
        }
        items.push_back(item);
    }
    if (items.empty()) return kEmptyHash;
    std::sort(items.begin(), items.end());
    uint64_t h = kEmptyHash;
    for (auto& s : items) {
        for (unsigned char ch : s) h = (h ^ ch) * 0x100000001B3ull;
        h = (h ^ 0xFF) * 0x100000001B3ull;
    }
    return h;
}

/* ---------------------------------------------------------------- file watch */

std::string FileWatch::stamp() const {
    if (path_.empty()) return std::string();
    std::error_code ec;
    fs::path p = fs::u8path(path_);
    auto t = fs::last_write_time(p, ec);
    if (ec) return "missing";
    auto sz = fs::file_size(p, ec);
    return std::to_string((long long)t.time_since_epoch().count()) + ":" + std::to_string(ec ? 0ull : (unsigned long long)sz);
}

bool FileWatch::changed() {
    std::string s = stamp();
    if (s == stamp_) return false;
    stamp_ = s;
    return true;
}

}  // namespace cheats
