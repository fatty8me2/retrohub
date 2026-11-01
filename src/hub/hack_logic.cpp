#include "hack_logic.h"

#include "json.h"

#include <algorithm>
#include <initializer_list>

/* hack.json <-> HackDef (Plan 4 H2). Pure. */
namespace hubui {
namespace hacks {

namespace {
std::string clean_title(const std::string& t) {
    std::string o;
    for (unsigned char c : t) if (c >= 32 && c != 127) o.push_back((char)c);
    while (!o.empty() && o.back() == ' ') o.pop_back();
    size_t b = 0;
    while (b < o.size() && o[b] == ' ') ++b;
    o = o.substr(b);
    if (o.size() > 100) {   // cut on a character boundary
        size_t n = 100;
        while (n > 0 && ((unsigned char)o[n] & 0xC0) == 0x80) --n;
        o.resize(n);
    }
    return o;
}
bool one_of(const std::string& v, std::initializer_list<const char*> l) {
    for (const char* s : l) if (v == s) return true;
    return false;
}
}  // namespace

bool valid_patch_file_name(const std::string& n) {
    if (n.empty() || n.size() > 120 || n == "." || n == "..") return false;
    for (unsigned char c : n) if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    return n.find("..") == std::string::npos;
}

std::string encode(const HackDef& d) {
    json::Value o = json::make_object();
    auto put = [&](const char* k, json::Value v) { o.o.emplace_back(k, std::move(v)); };
    put("version", json::make_int(1));
    put("key", json::make_string(d.key));
    put("base_key", json::make_string(d.base_key));
    put("system", json::make_string(d.system));
    put("title", json::make_string(d.title));
    put("kind", json::make_string(d.kind));
    put("lang", json::make_string(d.lang));
    put("patch_file", json::make_string(d.patch_file));
    put("format", json::make_string(d.format));
    put("prep", json::make_string(d.prep));
    put("patch_crc", json::make_string(hex8(d.patch_crc)));
    put("out_crc", json::make_string(hex8(d.out_crc)));
    put("out_size", json::make_int((int64_t)d.out_size));
    put("added", json::make_int(d.added));
    return json::stringify(o);
}

bool decode(const std::string& text, HackDef* out) {
    json::Value v;
    if (!json::parse(text, &v) || !v.is_object()) return false;
    if (v.i64("version", 0) != 1) return false;
    HackDef d;
    d.key = v.str("key");
    d.base_key = v.str("base_key");
    d.system = v.str("system");
    d.title = clean_title(v.str("title"));
    d.kind = v.str("kind", "hack");
    d.lang = v.str("lang");
    d.patch_file = v.str("patch_file");
    d.format = v.str("format");
    d.prep = v.str("prep");
    auto hex = [](const std::string& s, uint32_t* o) {
        if (s.size() != 8) return false;
        for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        *o = (uint32_t)std::strtoul(s.c_str(), nullptr, 16);
        return true;
    };
    if (!hex(v.str("patch_crc"), &d.patch_crc) || !hex(v.str("out_crc"), &d.out_crc)) return false;
    d.out_size = (uint64_t)std::max<int64_t>(0, v.i64("out_size", 0));
    d.added = v.i64("added", 0);
    std::string base;
    uint32_t crc = 0;
    if (!parse_key(d.key, &base, &crc) || base != d.base_key || crc != d.out_crc) return false;
    if (d.base_key.empty() || d.system.empty() || d.title.empty()) return false;
    if (!valid_patch_file_name(d.patch_file)) return false;
    if (!one_of(d.format, {"ips", "ups", "bps", "vcdiff"})) return false;
    if (!one_of(d.prep, {"", "z64", "nohdr"})) return false;
    if (!one_of(d.kind, {"hack", "translation"})) return false;
    if (d.lang.size() > 12) return false;
    if (out) *out = d;
    return true;
}

}  // namespace hacks
}  // namespace hubui
