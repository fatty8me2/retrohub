#include "cheats.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
        case Format::ActionReplayGC: return "ar_gc";
        case Format::Gecko: return "gecko";
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
    else if (s == "ar_gc" || s == "argc") *out = Format::ActionReplayGC;
    else if (s == "gecko") *out = Format::Gecko;
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
    else if (s == "gc" || s == "gamecube") *out = Target::GC;
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

/* Dolphin's encrypted Action Replay form: XXXX-XXXX-XXXXX (letters/digits). */
bool gc_is_encrypted_ar(const std::string& line) {
    std::string s = squeeze(line);
    if (s.size() != 15 || s[4] != '-' || s[9] != '-') return false;
    for (size_t i = 0; i < s.size(); i++) if (i != 4 && i != 9 && !std::isalnum((unsigned char)s[i])) return false;
    return true;
}

bool normalize_code(Target target, Format fmt, const std::string& line, std::string* out, std::string* err) {
    auto bad = [&](const char* why) { if (err) *err = std::string(why) + ": '" + line + "'"; return false; };
    std::string r;
    if (target != Target::GC && (fmt == Format::Gecko || fmt == Format::ActionReplayGC)) return bad("Gecko / GameCube Action Replay codes only work on GameCube");
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
        case Target::GC:
            if (fmt == Format::Gecko) { if (!pair_code(line, 8, 8, &r)) return bad("expected AAAAAAAA VVVVVVVV (Gecko)"); }
            else if (fmt == Format::ActionReplayGC || fmt == Format::ActionReplay) {
                if (gc_is_encrypted_ar(line)) r = squeeze(line);
                else if (!pair_code(line, 8, 8, &r)) return bad("expected AAAAAAAA VVVVVVVV or XXXX-XXXX-XXXXX (Action Replay)");
            } else return bad("only Gecko and Action Replay codes work on GameCube");
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

/* ---------------------------------------------------------------- value cheats */

namespace {

struct Run { size_t code; size_t pos, len; };

bool is_ph(char c) { return c == 'X' || c == 'x' || c == '?'; }
bool is_sep(char c) { return c == ' ' || c == ';' || c == '+' || c == ',' || c == '\n' || c == '\r' || c == '\t' || c == '-' || c == ':'; }
bool is_tokch(char c) { return is_ph(c) || std::isxdigit((unsigned char)c); }

/* Placeholder runs of a cheat. Empty when no code has one, or a code holds anything but hex / placeholders / separators. */
std::vector<Run> find_runs(const Cheat& c) {
    std::vector<Run> runs;
    if (c.format == Format::Gecko || c.format == Format::ActionReplayGC) return runs;
    for (size_t k = 0; k < c.codes.size(); ++k) {
        const std::string& s = c.codes[k];
        for (size_t i = 0; i < s.size();) {
            if (is_ph(s[i])) {
                size_t j = i;
                while (j < s.size() && is_ph(s[j])) ++j;
                runs.push_back({k, i, j - i});
                i = j;
            } else if (std::isxdigit((unsigned char)s[i]) || is_sep(s[i])) ++i;
            else return {};
        }
    }
    return runs;
}

/* The 8-digit address word a run belongs to ("" when the run sits inside an 8-digit word itself, e.g. GB "01XXF8D4"). */
std::string head_word(const std::string& s, size_t pos) {
    size_t b = pos;
    while (b > 0 && is_tokch(s[b - 1])) --b;
    size_t e = pos;
    while (e < s.size() && is_tokch(s[e])) ++e;
    if (e - b >= 8) return std::string();
    size_t p = b;
    while (p > 0 && is_sep(s[p - 1])) --p;
    size_t pb = p;
    while (pb > 0 && is_tokch(s[pb - 1])) --pb;
    if (p - pb != 8) return std::string();
    return s.substr(pb, 8);
}

bool hex_tok(const std::string& s, size_t* p, uint32_t* v, bool* letters) {
    size_t i = *p, st = i;
    uint64_t x = 0;
    bool l = false;
    while (i < s.size() && std::isxdigit((unsigned char)s[i]) && i - st < 8) {
        char ch = s[i];
        if (std::isalpha((unsigned char)ch)) l = true;
        x = x * 16 + (uint64_t)(std::isdigit((unsigned char)ch) ? ch - '0' : (std::tolower((unsigned char)ch) - 'a' + 10));
        ++i;
    }
    if (i == st) return false;
    if (i < s.size() && std::isalnum((unsigned char)s[i])) return false;   // a word, not a number ("Cart", "Fire")
    *v = (uint32_t)x; *letters = l; *p = i;
    return true;
}

/* "A-B" / "A to B" at s[p]; digit-only tokens are hex unless `dec`. */
bool range_at(const std::string& s, size_t p, bool dec, uint32_t* lo, uint32_t* hi, bool* letters) {
    while (p < s.size() && s[p] == ' ') ++p;
    size_t a = p;
    bool l1 = false, l2 = false;
    uint32_t x, y;
    if (!hex_tok(s, &p, &x, &l1)) return false;
    size_t e1 = p;
    size_t q = p;
    while (q < s.size() && s[q] == ' ') ++q;
    if (q < s.size() && s[q] == '-') ++q;
    else if (s.compare(q, 2, "to") == 0 || s.compare(q, 2, "TO") == 0) q += 2;
    else return false;
    while (q < s.size() && s[q] == ' ') ++q;
    size_t b = q;
    if (!hex_tok(s, &q, &y, &l2)) return false;
    if (dec && !l1 && !l2) {
        x = (uint32_t)std::strtoul(s.substr(a, e1 - a).c_str(), nullptr, 10);
        y = (uint32_t)std::strtoul(s.substr(b, q - b).c_str(), nullptr, 10);
    }
    *lo = x; *hi = y; *letters = l1 || l2;
    return true;
}

}  // namespace

bool value_has_placeholder(const Cheat& c) { return !find_runs(c).empty(); }

ValueSpec value_spec(const Cheat& c, Target target) {
    ValueSpec sp;
    std::vector<Run> runs = find_runs(c);
    if (runs.empty()) return sp;
    sp.valid = true;
    for (auto& r : runs) sp.digits = std::max(sp.digits, (int)r.len);
    sp.digits = std::min(sp.digits, 8);
    uint64_t slot = sp.digits >= 8 ? 0xFFFFFFFFull : ((1ull << (4 * sp.digits)) - 1);
    /* 8-bit writes only keep the low byte: N64 GameShark 80/A0/88/D0/D2/F0, PS1 30. */
    bool byte_type = false;
    for (auto& r : runs) {
        std::string head = head_word(c.codes[r.code], r.pos);
        if (head.empty()) continue;
        unsigned t = (unsigned)std::stoul(head.substr(0, 2), nullptr, 16);
        if (target == Target::N64 && (t == 0x80 || t == 0xA0 || t == 0x88 || t == 0xD0 || t == 0xD2 || t == 0xF0)) byte_type = true;
        if (target == Target::PS1 && t == 0x30) byte_type = true;
    }
    if (byte_type) slot = std::min<uint64_t>(slot, 0xFF);
    sp.min = 0;
    sp.max = (uint32_t)slot;

    const std::string& d = c.desc;
    std::string dl = lower_s(d);
    bool word_hex = dl.find("hex") != std::string::npos;
    bool dec = dl.find("dec") != std::string::npos && !word_hex;
    sp.prefer_hex = word_hex;
    /* range: "(00-63)", "(0-63 Hexidecimal)", "enter 0-99", "range 1-5"; the last one wins */
    uint32_t lo = 0, hi = 0;
    bool have = false, letters = false;
    for (size_t i = 0; i < d.size(); ++i) {
        size_t start = std::string::npos;
        if (d[i] == '(') start = i + 1;
        else if (i == 0 || !std::isalpha((unsigned char)dl[i - 1])) {
            static const char* kw[] = {"enter ", "range ", "between "};
            for (auto* k : kw) { size_t n = std::strlen(k); if (dl.compare(i, n, k) == 0) start = i + n; }
        }
        if (start == std::string::npos) continue;
        uint32_t a, b;
        bool l;
        if (range_at(d, start, dec, &a, &b, &l) && a <= b) { lo = a; hi = b; letters = l; have = true; }
    }
    if (have && lo <= slot) {
        sp.hinted = true;
        sp.min = lo;
        sp.max = (uint32_t)std::min<uint64_t>(hi, slot);
        if (letters) sp.prefer_hex = true;
    }
    /* option list: "01=Potion 02=Antidote" */
    struct Hit { size_t tok, lab; uint32_t v; };
    std::vector<Hit> hits;
    for (size_t i = 0; i < d.size(); ++i) {
        if (d[i] != '=') continue;
        size_t e = i;
        while (e > 0 && d[e - 1] == ' ') --e;
        size_t b = e;
        while (b > 0 && std::isxdigit((unsigned char)d[b - 1])) --b;
        if (e == b || e - b > 4) continue;
        if (b > 0 && std::isalnum((unsigned char)d[b - 1])) continue;
        hits.push_back({b, i + 1, (uint32_t)std::strtoul(d.substr(b, e - b).c_str(), nullptr, 16)});
    }
    if (hits.size() >= 2) {
        for (size_t h = 0; h < hits.size(); ++h) {
            size_t end = h + 1 < hits.size() ? hits[h + 1].tok : d.size();
            std::string lab = d.substr(hits[h].lab, end - hits[h].lab);
            while (!lab.empty() && (lab.back() == ' ' || lab.back() == ',' || lab.back() == ';' || lab.back() == '/' || lab.back() == '|' || lab.back() == ')')) lab.pop_back();
            size_t f = lab.find_first_not_of(' ');
            lab = f == std::string::npos ? std::string() : lab.substr(f);
            if (lab.empty() || hits[h].v > slot) continue;
            sp.options.push_back({hits[h].v, lab});
        }
        if (sp.options.size() < 2) sp.options.clear();
        else {
            uint32_t mn = UINT32_MAX, mx = 0;
            for (auto& o : sp.options) { mn = std::min(mn, o.value); mx = std::max(mx, o.value); }
            sp.min = mn; sp.max = mx;
            sp.hinted = true;
            sp.prefer_hex = true;
        }
    }
    return sp;
}

uint32_t value_clamp(const ValueSpec& s, uint32_t v) { return v < s.min ? s.min : v > s.max ? s.max : v; }

bool value_apply(const Cheat& c, uint32_t v, Target target, Cheat* out) {
    std::vector<Run> runs = find_runs(c);
    if (runs.empty()) return false;
    v = value_clamp(value_spec(c, target), v);
    Cheat r = c;
    static const char* hx = "0123456789ABCDEF";
    for (auto& run : runs) {
        std::string& s = r.codes[run.code];
        for (size_t i = 0; i < run.len; ++i) {
            size_t nib = run.len - 1 - i;   // low digit last
            s[run.pos + i] = nib >= 8 ? '0' : hx[(v >> (4 * nib)) & 0xF];
        }
    }
    *out = std::move(r);
    return true;
}

bool value_parse(const std::string& text, bool dec, uint32_t* out) {
    std::string t;
    for (char c : text) if (c != ' ') t += c;
    if (t.empty()) return false;
    bool hex = !dec;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { t = t.substr(2); hex = true; }
    else if (t.size() > 1 && (t.back() == 'h' || t.back() == 'H')) { t.pop_back(); hex = true; }
    if (t.empty() || t.size() > 10) return false;
    uint64_t v = 0;
    for (char c : t) {
        int d = std::isdigit((unsigned char)c) ? c - '0' : hex && std::isxdigit((unsigned char)c) ? std::tolower((unsigned char)c) - 'a' + 10 : -1;
        if (d < 0) return false;
        v = v * (hex ? 16 : 10) + (uint64_t)d;
        if (v > 0xFFFFFFFFull) return false;
    }
    *out = (uint32_t)v;
    return true;
}

std::string value_format(uint32_t v, bool dec, int digits) {
    char b[24];
    if (dec) std::snprintf(b, sizeof b, "%u", v);
    else std::snprintf(b, sizeof b, "%0*X", std::max(1, std::min(digits, 8)), v);
    return b;
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
