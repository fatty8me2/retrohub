#include "game_settings.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace hub {
namespace gs {

unsigned sys_bit(System s) { return 1u << (unsigned)s; }

namespace {

const unsigned N64 = 1u << (unsigned)System::N64;
const unsigned GB = (1u << (unsigned)System::GB) | (1u << (unsigned)System::GBC);
const unsigned GBA = 1u << (unsigned)System::GBA;
const unsigned NDS = 1u << (unsigned)System::NDS;
const unsigned GC = 1u << (unsigned)System::GC;
const unsigned N3DS = 1u << (unsigned)System::N3DS;
const unsigned WIIU = 1u << (unsigned)System::WiiU;
const unsigned ALL = N64 | GB | GBA | NDS | GC | N3DS | WIIU;

Option mk(const char* key, const char* label, const char* group, unsigned sys, Kind k, Scope sc, const char* def,
          std::vector<Choice> ch = {}) {
    Option o;
    o.key = key; o.label = label; o.group = group; o.systems = sys; o.kind = k; o.scope = sc; o.def = def;
    o.choices = std::move(ch);
    return o;
}

std::vector<Option> build() {
    using S = Scope; using K = Kind;
    std::vector<Option> v;
    v.push_back(mk("fullscreen", "Fullscreen", "Video", ALL, K::Bool, S::Visual, "false"));

    // ---- N64 (GLideN64 via n64run)
    v.push_back(mk("n64.res", "Internal resolution", "Video", N64, K::Choice, S::Visual, "0",
                   {{"0", "Match window"}, {"1", "1x native"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {"6", "6x"}}));
    v.push_back(mk("n64.aspect", "Aspect ratio", "Video", N64, K::Choice, S::Visual, "4:3",
                   {{"4:3", "4:3"}, {"stretch", "Stretch to window"}, {"16:9", "16:9 (stretched)"},
                    {"widescreen", "Widescreen (adjust to window)"}}));
    v.push_back(mk("n64.aa", "Anti-aliasing (MSAA)", "Video", N64, K::Choice, S::Visual, "0",
                   {{"0", "Off"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}}));
    v.push_back(mk("n64.fxaa", "FXAA", "Video", N64, K::Bool, S::Visual, "false"));
    v.push_back(mk("n64.texfilter", "Texture filtering", "Video", N64, K::Choice, S::Visual, "3point",
                   {{"3point", "3-point (N64 look)"}, {"bilinear", "Bilinear"}}));
    v.push_back(mk("n64.texpack", "Texture pack folder", "Video", N64, K::Text, S::Visual, ""));
    // Framebuffer emulation changes what lands in RDRAM (and the lockstep hash): locked online.
    v.push_back(mk("n64.fb_emulation", "Framebuffer emulation", "Advanced", N64, K::Choice, S::Locked, "default",
                   {{"default", "Default"}, {"on", "On"}, {"off", "Off"}}));
    v.push_back(mk("n64.copy_color", "Copy color buffer to RDRAM", "Advanced", N64, K::Choice, S::Locked, "default",
                   {{"default", "Default"}, {"off", "Off"}, {"sync", "Synchronous"}, {"async", "Asynchronous"}}));
    v.push_back(mk("n64.copy_depth", "Copy depth buffer to RDRAM", "Advanced", N64, K::Choice, S::Locked, "default",
                   {{"default", "Default"}, {"off", "Off"}, {"sw", "Software"}, {"fast", "Fast"}}));

    // ---- Game Boy / GBC / GBA / DS (retrorun)
    v.push_back(mk("scale.filter", "Screen scaling", "Video", GB | GBA | NDS, K::Choice, S::Visual, "auto",
                   {{"auto", "Automatic"}, {"nearest", "Sharp pixels"}, {"linear", "Smooth"}}));
    v.push_back(mk("scale.integer", "Integer scaling", "Video", GB | GBA, K::Bool, S::Visual, "true"));
    v.push_back(mk("gb.color", "Color correction", "Color", GB | GBA, K::Choice, S::Visual, "auto",
                   {{"auto", "Core default"}, {"off", "Off"}, {"on", "On"}}));
    v.push_back(mk("ds.layout", "Screen layout", "Video", NDS, K::Choice, S::Visual, "top-bottom",
                   {{"top-bottom", "Top / bottom"}, {"left-right", "Side by side"},
                    {"largescreen-top", "Large top screen"}, {"hybrid-top", "Hybrid"}}));
    // Results-changing: the host's value goes to guests.
    v.push_back(mk("gb.hardware", "Game Boy hardware", "Emulation", GB, K::Choice, S::Sync, "auto",
                   {{"auto", "Automatic"}, {"dmg", "Game Boy"}, {"cgb", "Game Boy Color"}, {"agb", "Game Boy Advance"}}));
    v.push_back(mk("gba.skip_bios", "Skip BIOS intro", "Emulation", GBA, K::Bool, S::Sync, "false"));

    // ---- GameCube (Dolphin), 3DS (Azahar), Wii U (Cemu)
    v.push_back(mk("gc.res", "Internal resolution", "Video", GC, K::Choice, S::Visual, "2",
                   {{"1", "1x native"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {"5", "5x"}, {"6", "6x"}}));
    v.push_back(mk("gc.widescreen", "Widescreen hack", "Video", GC, K::Bool, S::Visual, "false"));
    v.push_back(mk("3ds.res", "Resolution", "Video", N3DS, K::Choice, S::Visual, "1",
                   {{"1", "1x native"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {"5", "5x"}, {"6", "6x"}}));
    v.push_back(mk("3ds.layout", "Screen layout", "Video", N3DS, K::Choice, S::Visual, "default",
                   {{"default", "Default"}, {"single", "Single screen"}, {"large", "Large screen"},
                    {"side", "Side by side"}, {"hybrid", "Hybrid"}}));
    v.push_back(mk("wiiu.graphic_packs", "Graphic packs", "Video", WIIU, K::Bool, S::Visual, "true"));
    return v;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// ---- tiny JSON: {"a":{"k":"v"},...} strings only
void jesc(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    o += '"';
}

struct P {
    const std::string& s; size_t i = 0;
    explicit P(const std::string& t) : s(t) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++; }
    bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { i++; return true; } return false; }
    bool str(std::string* o) {
        ws();
        if (i >= s.size() || s[i] != '"') return false;
        i++; o->clear();
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\') {
                if (i >= s.size()) return false;
                char e = s[i++];
                if (e == 'n') *o += '\n'; else if (e == 't') *o += '\t';
                else if (e == 'u') {
                    if (i + 4 > s.size()) return false;
                    unsigned cp = (unsigned)std::strtoul(s.substr(i, 4).c_str(), nullptr, 16); i += 4;
                    if (cp < 0x80) *o += (char)cp;
                    else if (cp < 0x800) { *o += (char)(0xC0 | (cp >> 6)); *o += (char)(0x80 | (cp & 63)); }
                    else { *o += (char)(0xE0 | (cp >> 12)); *o += (char)(0x80 | ((cp >> 6) & 63)); *o += (char)(0x80 | (cp & 63)); }
                } else *o += e;
            } else *o += c;
        }
        if (i >= s.size()) return false;
        i++;
        return true;
    }
    // flat object of string values
    bool flat(Values* o) {
        if (!eat('{')) return false;
        if (eat('}')) return true;
        for (;;) {
            std::string k, v;
            if (!str(&k) || !eat(':') || !str(&v)) return false;
            (*o)[k] = v;
            if (eat(',')) continue;
            return eat('}');
        }
    }
    bool nested(std::map<std::string, Values>* o) {
        if (!eat('{')) return false;
        if (eat('}')) return true;
        for (;;) {
            std::string k;
            Values v;
            if (!str(&k) || !eat(':') || !flat(&v)) return false;
            (*o)[k] = v;
            if (eat(',')) continue;
            return eat('}');
        }
    }
};

const char* sys_name(System s) { return system_short(s); }

void put_group(std::string& o, const char* name, const std::map<std::string, Values>& m) {
    o += "  "; jesc(o, name); o += ": {";
    bool first = true;
    for (auto& kv : m) {
        if (kv.second.empty()) continue;
        o += first ? "\n    " : ",\n    "; first = false;
        jesc(o, kv.first); o += ": {";
        bool f2 = true;
        for (auto& e : kv.second) { if (!f2) o += ", "; f2 = false; jesc(o, e.first); o += ": "; jesc(o, e.second); }
        o += "}";
    }
    o += first ? "}" : "\n  }";
}

void keep_valid(Values* v) {
    for (auto it = v->begin(); it != v->end();) {
        const Option* o = find_option(it->first);
        std::string n;
        if (!o || !normalize(*o, it->second, &n)) it = v->erase(it);
        else { it->second = n; ++it; }
    }
}

}  // namespace

const std::vector<Option>& options() { static const std::vector<Option> v = build(); return v; }

const Option* find_option(const std::string& key) {
    for (auto& o : options()) if (o.key == key) return &o;
    return nullptr;
}

std::vector<const Option*> options_for(System s) {
    std::vector<const Option*> r;
    for (auto& o : options()) if (o.systems & sys_bit(s)) r.push_back(&o);
    return r;
}

bool normalize(const Option& o, const std::string& value, std::string* out) {
    std::string v = trim(value);
    switch (o.kind) {
        case Kind::Bool: {
            std::string l;
            for (char c : v) l += (char)std::tolower((unsigned char)c);
            if (l == "true" || l == "1" || l == "on" || l == "yes") { *out = "true"; return true; }
            if (l == "false" || l == "0" || l == "off" || l == "no") { *out = "false"; return true; }
            return false;
        }
        case Kind::Choice:
            for (auto& c : o.choices) if (c.value == v) { *out = v; return true; }
            return false;
        case Kind::Text:
            if (v.size() > 520) return false;
            for (unsigned char c : v) if (c < 0x20) return false;
            *out = v;
            return true;
    }
    return false;
}

Values defaults_for(System s) {
    Values v;
    for (auto& o : options()) if (o.systems & sys_bit(s)) v[o.key] = o.def;
    return v;
}

// ---------------------------------------------------------------- Store

bool Store::set_system(System s, const std::string& key, const std::string& value) {
    const Option* o = find_option(key);
    std::string n;
    if (!o || !(o->systems & sys_bit(s)) || !normalize(*o, value, &n)) return false;
    sys_[sys_name(s)][key] = n;
    return true;
}

bool Store::set_game(const std::string& game_key, System s, const std::string& key, const std::string& value) {
    const Option* o = find_option(key);
    std::string n;
    if (game_key.empty() || !o || !(o->systems & sys_bit(s)) || !normalize(*o, value, &n)) return false;
    game_[game_key][key] = n;
    return true;
}

void Store::clear_system_key(System s, const std::string& key) {
    auto it = sys_.find(sys_name(s));
    if (it != sys_.end()) { it->second.erase(key); if (it->second.empty()) sys_.erase(it); }
}

void Store::clear_game_key(const std::string& g, const std::string& key) {
    auto it = game_.find(g);
    if (it != game_.end()) { it->second.erase(key); if (it->second.empty()) game_.erase(it); }
}

void Store::reset_game(const std::string& g) { game_.erase(g); }
void Store::reset_system(System s) { sys_.erase(sys_name(s)); }
void Store::reset_all() { sys_.clear(); game_.clear(); }
bool Store::has_game_overrides(const std::string& g) const { auto it = game_.find(g); return it != game_.end() && !it->second.empty(); }

Values Store::resolve(System s, const std::string& g) const {
    Values v = defaults_for(s);
    auto layer = [&](const Values& m) {
        for (auto& kv : m) if (v.count(kv.first)) v[kv.first] = kv.second;
    };
    auto si = sys_.find(sys_name(s));
    if (si != sys_.end()) layer(si->second);
    auto gi = game_.find(g);
    if (gi != game_.end()) layer(gi->second);
    return v;
}

int Store::source(System s, const std::string& g, const std::string& key) const {
    auto gi = game_.find(g);
    if (gi != game_.end() && gi->second.count(key)) return 2;
    auto si = sys_.find(sys_name(s));
    if (si != sys_.end() && si->second.count(key)) return 1;
    return 0;
}

std::string Store::to_json() const {
    std::string o = "{\n  \"version\": \"1\",\n";
    put_group(o, "systems", sys_);
    o += ",\n";
    put_group(o, "games", game_);
    o += "\n}\n";
    return o;
}

bool Store::from_json(const std::string& text) {
    P p(text);
    if (!p.eat('{')) return false;
    std::map<std::string, Values> sys, game;
    if (!p.eat('}')) {
        for (;;) {
            std::string k;
            if (!p.str(&k) || !p.eat(':')) return false;
            if (k == "systems") { if (!p.nested(&sys)) return false; }
            else if (k == "games") { if (!p.nested(&game)) return false; }
            else { std::string ignore; if (!p.str(&ignore)) return false; }
            if (p.eat(',')) continue;
            if (!p.eat('}')) return false;
            break;
        }
    }
    for (auto it = sys.begin(); it != sys.end();) { keep_valid(&it->second); it = it->second.empty() ? sys.erase(it) : std::next(it); }
    for (auto it = game.begin(); it != game.end();) { keep_valid(&it->second); it = it->second.empty() ? game.erase(it) : std::next(it); }
    sys_ = std::move(sys);
    game_ = std::move(game);
    return true;
}

void Store::merge_from(const Store& o) {
    for (auto& kv : o.sys_) for (auto& e : kv.second) sys_[kv.first][e.first] = e.second;
    for (auto& kv : o.game_) for (auto& e : kv.second) game_[kv.first][e.first] = e.second;
}

std::string Store::file_path(const std::string& vault_dir) {
    return (fs::u8path(vault_dir) / "_settings" / "game_settings.json").u8string();
}

bool Store::load(const std::string& vault_dir) {
    std::ifstream f(fs::u8path(file_path(vault_dir)), std::ios::binary);
    if (!f) { reset_all(); return true; }
    std::stringstream ss; ss << f.rdbuf();
    return from_json(ss.str());
}

bool Store::save(const std::string& vault_dir) const {
    std::error_code ec;
    fs::path p = fs::u8path(file_path(vault_dir));
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p; tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << to_json();
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(p, ec); ec.clear(); fs::rename(tmp, p, ec); }
    return !ec;
}

// ---------------------------------------------------------------- online

Values sync_subset(const Values& r) {
    Values o;
    for (auto& kv : r) { const Option* op = find_option(kv.first); if (op && op->scope == Scope::Sync) o[kv.first] = kv.second; }
    return o;
}

std::string sync_fragment(const Values& resolved) {
    Values s = sync_subset(resolved);
    if (s.empty()) return "";
    std::string o = ",\"gs\":{";
    bool first = true;
    for (auto& kv : s) { if (!first) o += ","; first = false; jesc(o, kv.first); o += ":"; jesc(o, kv.second); }
    return o + "}";
}

Values extract_sync(const std::string& json) {
    Values v;
    size_t k = json.find("\"gs\"");
    size_t start = 0;
    if (k != std::string::npos) {
        start = json.find('{', k);
        if (start == std::string::npos) return {};
    } else {
        start = json.find_first_not_of(" \t\r\n");
        if (start == std::string::npos || json[start] != '{' || json.find("\"gs\"") != std::string::npos) return {};
        // bare object only accepted when it has no nested braces
        if (json.find('{', start + 1) != std::string::npos) return {};
    }
    P p(json);
    p.i = start;
    if (!p.flat(&v)) return {};
    for (auto it = v.begin(); it != v.end();) {
        const Option* o = find_option(it->first);
        std::string n;
        if (!o || o->scope != Scope::Sync || !normalize(*o, it->second, &n)) it = v.erase(it);
        else { it->second = n; ++it; }
    }
    return v;
}

Values online_effective(System s, const Values& local, const Values& host_sync) {
    Values r = local;
    for (auto& kv : r) {
        const Option* o = find_option(kv.first);
        if (!o) continue;
        if (o->scope == Scope::Locked) kv.second = o->def;
        else if (o->scope == Scope::Sync) {
            auto h = host_sync.find(kv.first);
            kv.second = h != host_sync.end() ? h->second : o->def;
        }
    }
    (void)s;
    return r;
}

// ---------------------------------------------------------------- emitters

bool n64_key_locked_online(const std::string& k) {
    return k == "EnableFBEmulation" || k == "EnableCopyColorToRDRAM" || k == "EnableCopyDepthToRDRAM";
}

bool is_fullscreen(const Values& v) { auto i = v.find("fullscreen"); return i != v.end() && i->second == "true"; }

static std::string get(const Values& v, const char* k) {
    auto i = v.find(k);
    if (i != v.end()) return i->second;
    const Option* o = find_option(k);
    return o ? o->def : "";
}

std::vector<std::pair<std::string, std::string>> n64_gliden_params(const Values& v) {
    std::vector<std::pair<std::string, std::string>> r;
    auto add = [&](const char* k, const std::string& t) { r.push_back({k, t}); };
    std::string res = get(v, "n64.res");
    if (res != "0") add("UseNativeResolutionFactor", "i:" + res);
    std::string a = get(v, "n64.aspect");
    if (a != "4:3") add("AspectRatio", std::string("i:") + (a == "stretch" ? "0" : a == "16:9" ? "2" : "3"));
    else add("AspectRatio", "i:1");
    if (get(v, "n64.aa") != "0") add("MultiSampling", "i:" + get(v, "n64.aa"));
    if (get(v, "n64.fxaa") == "true") add("EnableFXAA", "b:1");
    if (get(v, "n64.texfilter") == "bilinear") add("bilinearMode", "i:1");
    std::string tp = get(v, "n64.texpack");
    if (!tp.empty()) { add("txPath", "s:" + tp); add("txHiresEnable", "b:1"); }
    std::string fb = get(v, "n64.fb_emulation");
    if (fb != "default") add("EnableFBEmulation", fb == "on" ? "b:1" : "b:0");
    std::string cc = get(v, "n64.copy_color");
    if (cc != "default") add("EnableCopyColorToRDRAM", std::string("i:") + (cc == "off" ? "0" : cc == "sync" ? "1" : "2"));
    std::string cd = get(v, "n64.copy_depth");
    if (cd != "default") add("EnableCopyDepthToRDRAM", std::string("i:") + (cd == "off" ? "0" : cd == "sw" ? "1" : "2"));
    return r;
}

std::vector<std::string> n64_args(const Values& v) {
    std::vector<std::string> r;
    for (auto& p : n64_gliden_params(v)) { r.push_back("--gfx"); r.push_back(p.first + "=" + p.second); }
    return r;
}

bool parse_gfx_arg(const std::string& arg, GfxParam* out) {
    size_t eq = arg.find('=');
    if (eq == std::string::npos || eq == 0 || arg.size() < eq + 3 || arg[eq + 2] != ':') return false;
    char t = arg[eq + 1];
    std::string val = arg.substr(eq + 3);
    GfxParam p;
    p.key = arg.substr(0, eq); p.type = t; p.sval = val;
    if (t == 'i' || t == 'b') {
        char* e = nullptr;
        long n = std::strtol(val.c_str(), &e, 10);
        if (val.empty() || *e) return false;
        p.ival = (int)n;
        if (t == 'b' && n != 0 && n != 1) return false;
    } else if (t != 's') return false;
    *out = p;
    return true;
}

std::vector<std::string> retrorun_args(System s, const Values& v) {
    std::vector<std::string> r;
    auto opt = [&](const std::string& k, const std::string& val) { r.push_back("--opt"); r.push_back(k + "=" + val); };
    std::string f = get(v, "scale.filter");
    if (f != "auto") { r.push_back("--filter"); r.push_back(f); }
    if (s != System::NDS && get(v, "scale.integer") == "false") { r.push_back("--integer-scale"); r.push_back("0"); }
    std::string cc = get(v, "gb.color");
    if (cc != "auto" && (s == System::GB || s == System::GBC)) {
        bool on = cc == "on";
        opt("gambatte_gbc_color_correction", on ? "always" : "disabled");
        opt("sameboy_color_correction_mode", on ? "emulate hardware" : "off");
    } else if (cc != "auto" && s == System::GBA) {
        opt("mgba_color_correction", cc == "on" ? "Auto" : "OFF");
    }
    if (s == System::GB || s == System::GBC) {
        std::string hw = get(v, "gb.hardware");
        if (hw != "auto") {
            opt("gambatte_gb_hwmode", hw == "dmg" ? "GB" : hw == "cgb" ? "GBC" : "GBA");
            opt("sameboy_model", hw == "dmg" ? "Game Boy" : hw == "cgb" ? "Game Boy Color" : "Game Boy Advance");
        }
    }
    if (s == System::GBA && get(v, "gba.skip_bios") == "true") opt("mgba_skip_bios", "ON");
    if (s == System::NDS) opt("melonds_screen_layout1", get(v, "ds.layout"));
    return r;
}

}  // namespace gs
}  // namespace hub
