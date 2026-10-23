#include "controls.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace hubctl {

namespace fs = std::filesystem;

static const char* const kButtons[kNumButtons] = {"a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad"};
static const char* const kAxes[kNumAxes] = {"leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"};

int button_index(const std::string& n) { for (int i = 0; i < kNumButtons; i++) if (n == kButtons[i]) return i; return -1; }
int axis_index(const std::string& n) { for (int i = 0; i < kNumAxes; i++) if (n == kAxes[i]) return i; return -1; }
const char* button_name(int i) { return i >= 0 && i < kNumButtons ? kButtons[i] : ""; }
const char* axis_name(int i) { return i >= 0 && i < kNumAxes ? kAxes[i] : ""; }

const std::vector<std::string>& systems() {
    static const std::vector<std::string> v = {"n64", "retro", "gc", "wiiu", "3ds"};
    return v;
}
bool is_system(const std::string& s) { return std::find(systems().begin(), systems().end(), s) != systems().end(); }

std::string system_key(hub::System s) {
    switch (s) {
        case hub::System::N64: return "n64";
        case hub::System::GBA: case hub::System::GB: case hub::System::GBC: case hub::System::NDS: return "retro";
        case hub::System::GC: return "gc";
        case hub::System::WiiU: return "wiiu";
        case hub::System::N3DS: return "3ds";
        default: return "";
    }
}

/* ------------------------------------------------------------------ control lists + defaults */

const std::vector<ControlInfo>& controls_for(const std::string& sys) {
    static const std::map<std::string, std::vector<ControlInfo>> m = {
        {"n64", {{"a", "A", false}, {"b", "B", false}, {"z", "Z trigger", false}, {"start", "Start", false}, {"l", "L", false},
                 {"r", "R", false}, {"c_up", "C up", false}, {"c_down", "C down", false}, {"c_left", "C left", false},
                 {"c_right", "C right", false}, {"dpad_up", "D-pad up", false}, {"dpad_down", "D-pad down", false},
                 {"dpad_left", "D-pad left", false}, {"dpad_right", "D-pad right", false}, {"stick", "Analog stick", true}}},
        {"retro", {{"a", "A", false}, {"b", "B", false}, {"x", "X", false}, {"y", "Y", false}, {"l", "L", false}, {"r", "R", false},
                   {"l2", "L2", false}, {"r2", "R2", false}, {"l3", "L3", false}, {"r3", "R3", false}, {"start", "Start", false},
                   {"select", "Select", false}, {"up", "Up", false}, {"down", "Down", false}, {"left", "Left", false},
                   {"right", "Right", false}}},
        {"gc", {{"a", "A", false}, {"b", "B", false}, {"x", "X", false}, {"y", "Y", false}, {"z", "Z", false}, {"start", "Start", false},
                {"l", "L", false}, {"r", "R", false}, {"l_analog", "L analog", false}, {"r_analog", "R analog", false},
                {"dpad_up", "D-pad up", false}, {"dpad_down", "D-pad down", false}, {"dpad_left", "D-pad left", false},
                {"dpad_right", "D-pad right", false}, {"main_up", "Stick up", false}, {"main_down", "Stick down", false},
                {"main_left", "Stick left", false}, {"main_right", "Stick right", false}, {"c_up", "C-stick up", false},
                {"c_down", "C-stick down", false}, {"c_left", "C-stick left", false}, {"c_right", "C-stick right", false},
                {"main_stick", "Main stick (deadzone)", true}, {"c_stick", "C-stick (deadzone)", true}}},
        {"wiiu", {{"a", "A", false}, {"b", "B", false}, {"x", "X", false}, {"y", "Y", false}, {"l", "L", false}, {"r", "R", false},
                  {"zl", "ZL", false}, {"zr", "ZR", false}, {"plus", "+", false}, {"minus", "-", false}, {"dpup", "D-pad up", false},
                  {"dpdown", "D-pad down", false}, {"dpleft", "D-pad left", false}, {"dpright", "D-pad right", false},
                  {"l3", "Left stick click", false}, {"r3", "Right stick click", false}, {"ls_up", "Left stick up", false},
                  {"ls_down", "Left stick down", false}, {"ls_left", "Left stick left", false}, {"ls_right", "Left stick right", false},
                  {"rs_up", "Right stick up", false}, {"rs_down", "Right stick down", false}, {"rs_left", "Right stick left", false},
                  {"rs_right", "Right stick right", false}, {"left_stick", "Left stick (deadzone)", true},
                  {"right_stick", "Right stick (deadzone)", true}}},
        {"3ds", {{"a", "A", false}, {"b", "B", false}, {"x", "X", false}, {"y", "Y", false}, {"l", "L", false}, {"r", "R", false},
                 {"zl", "ZL", false}, {"zr", "ZR", false}, {"start", "Start", false}, {"select", "Select", false},
                 {"up", "D-pad up", false}, {"down", "D-pad down", false}, {"left", "D-pad left", false},
                 {"right", "D-pad right", false}, {"circle_pad", "Circle Pad", true}, {"c_stick", "C-Stick", true}}},
    };
    static const std::vector<ControlInfo> none;
    auto it = m.find(sys);
    return it == m.end() ? none : it->second;
}

Profile default_profile(const std::string& sys) {
    Profile p;
    p.name = "default-" + sys;
    p.system = sys;
    auto B = [&](std::initializer_list<std::pair<const char*, const char*>> l) { for (auto& kv : l) p.buttons[kv.first] = kv.second; };
    if (sys == "n64") {
        B({{"a", "a"}, {"b", "x|b|y"}, {"start", "start"}, {"z", "lefttrigger@16000"}, {"l", "leftshoulder"},
           {"r", "rightshoulder|righttrigger@16000"}, {"dpad_up", "dpup"}, {"dpad_down", "dpdown"}, {"dpad_left", "dpleft"},
           {"dpad_right", "dpright"}, {"c_right", "rightx+"}, {"c_left", "rightx-"}, {"c_down", "righty+"}, {"c_up", "righty-"}});
        p.analogs["stick"] = {"leftx", "lefty", 0.08, 1.0};
    } else if (sys == "retro") {
        B({{"b", "a"}, {"a", "b"}, {"y", "x"}, {"x", "y"}, {"l", "leftshoulder"}, {"r", "rightshoulder"}, {"start", "start"},
           {"select", "back"}, {"l3", "leftstick"}, {"r3", "rightstick"}, {"up", "dpup|lefty-@16000"}, {"down", "dpdown|lefty+@16000"},
           {"left", "dpleft|leftx-@16000"}, {"right", "dpright|leftx+@16000"}, {"l2", "lefttrigger@16000"}, {"r2", "righttrigger@16000"}});
    } else if (sys == "gc") {
        B({{"a", "a"}, {"b", "x"}, {"x", "b"}, {"y", "y"}, {"z", "rightshoulder"}, {"start", "start"}, {"main_up", "lefty-"},
           {"main_down", "lefty+"}, {"main_left", "leftx-"}, {"main_right", "leftx+"}, {"c_up", "righty-"}, {"c_down", "righty+"},
           {"c_left", "rightx-"}, {"c_right", "rightx+"}, {"l", "lefttrigger"}, {"r", "righttrigger"}, {"l_analog", "lefttrigger"},
           {"r_analog", "righttrigger"}, {"dpad_up", "dpup"}, {"dpad_down", "dpdown"}, {"dpad_left", "dpleft"}, {"dpad_right", "dpright"}});
        p.analogs["main_stick"] = {"", "", 0.0, 1.0};   // 0 = leave Dolphin's own default (nothing is written)
        p.analogs["c_stick"] = {"", "", 0.0, 1.0};
    } else if (sys == "wiiu") {
        B({{"a", "b"}, {"b", "a"}, {"x", "y"}, {"y", "x"}, {"l", "leftshoulder"}, {"r", "rightshoulder"}, {"zl", "lefttrigger"},
           {"zr", "righttrigger"}, {"plus", "start"}, {"minus", "back"}, {"dpup", "dpup"}, {"dpdown", "dpdown"}, {"dpleft", "dpleft"},
           {"dpright", "dpright"}, {"l3", "leftstick"}, {"r3", "rightstick"}, {"ls_up", "lefty-"}, {"ls_down", "lefty+"},
           {"ls_left", "leftx-"}, {"ls_right", "leftx+"}, {"rs_up", "righty-"}, {"rs_down", "righty+"}, {"rs_left", "rightx-"},
           {"rs_right", "rightx+"}});
        p.analogs["left_stick"] = {"", "", 0.25, 1.0};
        p.analogs["right_stick"] = {"", "", 0.25, 1.0};
    } else if (sys == "3ds") {
        B({{"a", "b"}, {"b", "a"}, {"x", "y"}, {"y", "x"}, {"up", "dpup"}, {"down", "dpdown"}, {"left", "dpleft"}, {"right", "dpright"},
           {"l", "leftshoulder"}, {"r", "rightshoulder"}, {"start", "start"}, {"select", "back"}, {"zl", "lefttrigger"},
           {"zr", "righttrigger"}});
        p.analogs["circle_pad"] = {"leftx", "lefty", 0.15, 1.0};
        p.analogs["c_stick"] = {"rightx", "righty", 0.15, 1.0};
    }
    return p;
}

bool Profile::same_mapping(const Profile& o) const {
    return system == o.system && buttons == o.buttons && analogs == o.analogs && axis_threshold == o.axis_threshold;
}

static std::string fmt_num(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

uint64_t mapping_hash(const Profile& p) {
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](const std::string& s) { for (unsigned char c : s) h = (h ^ c) * 0x100000001B3ull; h = (h ^ 0xFF) * 0x100000001B3ull; };
    mix(p.system);
    mix(std::to_string(p.axis_threshold));
    for (auto& kv : p.buttons) { mix(kv.first); mix(kv.second); }
    for (auto& kv : p.analogs) { mix(kv.first); mix(kv.second.x); mix(kv.second.y); mix(fmt_num(kv.second.deadzone)); mix(fmt_num(kv.second.sensitivity)); }
    return h;
}
std::string hash_hex(uint64_t h) { char b[20]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h); return b; }

/* ------------------------------------------------------------------ expressions */

bool parse_expr(const std::string& expr, int def_thr, std::vector<Term>* out, std::string* err) {
    out->clear();
    size_t p = 0;
    auto fail = [&](const std::string& m) { if (err) *err = m; out->clear(); return false; };
    while (p <= expr.size()) {
        size_t q = expr.find('|', p);
        if (q == std::string::npos) q = expr.size();
        std::string t = expr.substr(p, q - p);
        p = q + 1;
        size_t tb = t.find_first_not_of(" 	"), te = t.find_last_not_of(" 	");
        t = tb == std::string::npos ? std::string() : t.substr(tb, te - tb + 1);
        if (t.empty()) return fail("empty term");
        Term m;
        int thr = def_thr;
        size_t at = t.find('@');
        if (at != std::string::npos) {
            char* e = nullptr;
            long v = std::strtol(t.c_str() + at + 1, &e, 10);
            if (e == t.c_str() + at + 1 || *e || v < 1 || v > 32766) return fail("bad threshold in '" + t + "'");
            thr = (int)v;
            t.resize(at);
        }
        int sign = 0;
        if (!t.empty() && (t.back() == '+' || t.back() == '-')) { sign = t.back() == '+' ? 1 : -1; t.pop_back(); }
        int bi = button_index(t), ai = axis_index(t);
        if (bi >= 0 && !sign && at == std::string::npos) { m.axis = false; m.idx = bi; }
        else if (ai >= 0) { m.axis = true; m.idx = ai; m.sign = sign; m.threshold = thr; }
        else return fail("unknown source '" + t + "'");
        out->push_back(m);
        if (q >= expr.size()) break;
    }
    if (out->empty()) return fail("empty expression");
    return true;
}
bool valid_expr(const std::string& e) { std::vector<Term> t; return parse_expr(e, kDefaultAxisThreshold, &t); }
std::string term_name(const Term& t) {
    if (!t.axis) return kButtons[t.idx];
    return std::string(kAxes[t.idx]) + (t.sign > 0 ? "+" : t.sign < 0 ? "-" : "");
}

std::string describe_term(const Term& t) {
    static const char* bn[kNumButtons] = {"A", "B", "X", "Y", "Back", "Guide", "Start", "L3 (stick click)", "R3 (stick click)", "LB", "RB",
                                          "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Share", "Paddle 1", "Paddle 2",
                                          "Paddle 3", "Paddle 4", "Touchpad"};
    if (!t.axis) return t.idx >= 0 && t.idx < kNumButtons ? bn[t.idx] : "?";
    static const char* axn[4] = {"Left stick", "Left stick", "Right stick", "Right stick"};
    if (t.idx == 4) return "LT";
    if (t.idx == 5) return "RT";
    if (t.idx < 0 || t.idx > 3) return "?";
    bool horiz = (t.idx % 2) == 0;
    const char* dir = horiz ? (t.sign < 0 ? "left" : "right") : (t.sign < 0 ? "up" : "down");
    return std::string(axn[t.idx]) + " " + dir;
}
std::string describe_expr(const std::string& expr) {
    std::vector<Term> terms;
    if (!parse_expr(expr, kDefaultAxisThreshold, &terms) || terms.empty()) return "Unbound";
    std::string out;
    for (size_t i = 0; i < terms.size(); ++i) out += (i ? " / " : "") + describe_term(terms[i]);
    return out;
}

/* ------------------------------------------------------------------ Mapper */

Mapper::Mapper(const Profile& p) {
    auto add = [&](const std::string& name, Ctl c) { ids_[name] = (int)ctl_.size(); ctl_.push_back(std::move(c)); };
    for (auto& kv : p.buttons) {
        Ctl c;
        if (!parse_expr(kv.second, p.axis_threshold, &c.terms)) continue;   // an invalid expression = unbound
        add(kv.first, std::move(c));
    }
    for (auto& kv : p.analogs) {
        Ctl c;
        c.ax = axis_index(kv.second.x);
        c.ay = axis_index(kv.second.y);
        c.dead = (float)std::max(0.0, std::min(0.95, kv.second.deadzone));
        c.sens = (float)kv.second.sensitivity;
        add(kv.first, std::move(c));
    }
}
int Mapper::id(const std::string& c) const { auto it = ids_.find(c); return it == ids_.end() ? -1 : it->second; }

bool Mapper::down(int id, const PadState& s) const {
    if (id < 0 || id >= (int)ctl_.size()) return false;
    for (const Term& t : ctl_[id].terms) {
        if (!t.axis) { if (s.b[t.idx]) return true; continue; }
        int v = s.a[t.idx];
        if (t.sign < 0 ? v < -t.threshold : v > t.threshold) return true;
    }
    return false;
}

void Mapper::stick(int id, const PadState& s, float* ox, float* oy) const {
    *ox = *oy = 0;
    if (id < 0 || id >= (int)ctl_.size()) return;
    const Ctl& c = ctl_[id];
    if (c.ax < 0 || c.ay < 0) return;
    float x = std::max(-1.0f, std::min(1.0f, s.a[c.ax] / 32767.0f));
    float y = std::max(-1.0f, std::min(1.0f, s.a[c.ay] / 32767.0f));
    float mag = std::sqrt(x * x + y * y);
    if (mag <= c.dead) return;
    float scaled = std::min(1.0f, (mag - c.dead) / (1.0f - c.dead));
    float k = scaled / mag;
    *ox = x * k * c.sens;
    *oy = y * k * c.sens;
}

/* ------------------------------------------------------------------ Store operations */

Profile resolve(const Store& s, const std::string& sys, const std::string& game) {
    const std::string* name = nullptr;
    auto g = s.game_override.find(game);
    if (!game.empty() && g != s.game_override.end()) name = &g->second;
    else { auto d = s.system_default.find(sys); if (d != s.system_default.end()) name = &d->second; }
    Profile p;
    if (name) { auto it = s.profiles.find(*name); if (it != s.profiles.end() && it->second.system == sys) p = it->second; else name = nullptr; }
    if (!name) p = default_profile(sys);
    p.port_order = s.port_order;
    return p;
}

Profile& edit_profile(Store& s, const std::string& sys, const std::string& game) {
    std::string name = game.empty() ? sys + "-custom" : "game:" + game;
    auto it = s.profiles.find(name);
    if (it == s.profiles.end()) {
        Profile base = resolve(s, sys, game.empty() ? "" : game);
        base.name = name;
        base.port_order.clear();
        it = s.profiles.emplace(name, base).first;
    }
    if (game.empty()) s.system_default[sys] = name; else s.game_override[game] = name;
    s.stamp = (int64_t)std::time(nullptr);
    return it->second;
}

bool set_mapping(Store& s, const std::string& sys, const std::string& game, const std::string& control, const std::string& expr, std::string* err) {
    if (!is_system(sys)) { if (err) *err = "unknown system"; return false; }
    bool known = false;
    for (auto& c : controls_for(sys)) if (c.name == control && !c.analog) known = true;
    if (!known) { if (err) *err = "unknown control"; return false; }
    if (!valid_expr(expr)) { if (err) *err = "invalid source expression"; return false; }
    edit_profile(s, sys, game).buttons[control] = expr;
    return true;
}

bool set_analog(Store& s, const std::string& sys, const std::string& game, const std::string& control, double dz, double sens, std::string* err) {
    if (!is_system(sys)) { if (err) *err = "unknown system"; return false; }
    bool known = false;
    for (auto& c : controls_for(sys)) if (c.name == control && c.analog) known = true;
    if (!known) { if (err) *err = "unknown analog control"; return false; }
    if (dz < 0 || dz > 0.95 || sens < 0.1 || sens > 4) { if (err) *err = "deadzone 0..0.95, sensitivity 0.1..4"; return false; }
    Profile& p = edit_profile(s, sys, game);
    auto& a = p.analogs[control];
    a.deadzone = dz;
    a.sensitivity = sens;
    return true;
}

void reset(Store& s, const std::string& sys, const std::string& game) {
    if (game.empty()) {
        auto d = s.system_default.find(sys);
        if (d != s.system_default.end()) { s.profiles.erase(d->second); s.system_default.erase(d); }
    } else {
        auto g = s.game_override.find(game);
        if (g != s.game_override.end()) { s.profiles.erase(g->second); s.game_override.erase(g); }
    }
    s.stamp = (int64_t)std::time(nullptr);
}

void set_port_order(Store& s, const std::vector<std::string>& keys) { s.port_order = keys; s.stamp = (int64_t)std::time(nullptr); }

std::vector<int> order_pads(const std::vector<std::string>& order, const std::vector<std::string>& keys) {
    std::vector<int> out;
    std::vector<bool> used(keys.size(), false);
    auto guid = [](const std::string& k) { return k.substr(0, k.find('#')); };
    for (const std::string& want : order) {
        int hit = -1;
        for (size_t i = 0; i < keys.size() && hit < 0; i++) if (!used[i] && keys[i] == want) hit = (int)i;
        for (size_t i = 0; i < keys.size() && hit < 0; i++) if (!used[i] && guid(keys[i]) == guid(want)) hit = (int)i;
        if (hit >= 0) { used[hit] = true; out.push_back(hit); }
    }
    for (size_t i = 0; i < keys.size(); i++) if (!used[i]) out.push_back((int)i);
    return out;
}

/* ------------------------------------------------------------------ JSON (small, enough for our own files) */

namespace {

struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    const J* get(const std::string& k) const { for (auto& kv : o) if (kv.first == k) return &kv.second; return nullptr; }
};

struct Parser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    bool ok = true;
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++; }
    bool lit(const char* w) { size_t n = std::char_traits<char>::length(w); if (s.compare(i, n, w) == 0) { i += n; return true; } return false; }
    std::string str() {
        std::string r;
        i++;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char c = s[++i];
                if (c == 'n') r += '\n'; else if (c == 't') r += '\t'; else if (c == 'r') r += '\r';
                else if (c == 'u' && i + 4 < s.size()) { r += (char)std::strtol(s.substr(i + 1, 4).c_str(), nullptr, 16); i += 4; }
                else r += c;
                i++;
            } else r += s[i++];
        }
        if (i >= s.size()) ok = false; else i++;
        return r;
    }
    J val() {
        J v;
        ws();
        if (i >= s.size() || ++depth > 32) { ok = false; return v; }
        char c = s[i];
        if (c == '{') {
            v.t = J::Obj; i++; ws();
            if (i < s.size() && s[i] == '}') i++;
            else for (;;) {
                ws();
                if (i >= s.size() || s[i] != '"') { ok = false; break; }
                std::string k = str();
                ws();
                if (i >= s.size() || s[i] != ':') { ok = false; break; }
                i++;
                v.o.emplace_back(k, val());
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; break; }
                ok = false; break;
            }
        } else if (c == '[') {
            v.t = J::Arr; i++; ws();
            if (i < s.size() && s[i] == ']') i++;
            else for (;;) {
                v.a.push_back(val());
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; break; }
                ok = false; break;
            }
        } else if (c == '"') { v.t = J::Str; v.s = str(); }
        else if (lit("true")) { v.t = J::Bool; v.b = true; }
        else if (lit("false")) { v.t = J::Bool; }
        else if (lit("null")) {}
        else {
            char* e = nullptr;
            v.n = std::strtod(s.c_str() + i, &e);
            if (e == s.c_str() + i) ok = false; else { v.t = J::Num; i = (size_t)(e - s.c_str()); }
        }
        depth--;
        return v;
    }
};

std::string esc(const std::string& in) {
    std::string r = "\"";
    for (unsigned char c : in) {
        if (c == '"' || c == '\\') { r += '\\'; r += (char)c; }
        else if (c == '\n') r += "\\n";
        else if (c < 0x20) r += ' ';
        else r += (char)c;
    }
    return r + "\"";
}

void write_profile(std::ostringstream& o, const Profile& p, const char* ind) {
    o << ind << "{\"system\":" << esc(p.system) << ",\"axis_threshold\":" << p.axis_threshold << ",\n" << ind << " \"buttons\":{";
    bool first = true;
    for (auto& kv : p.buttons) { o << (first ? "" : ",") << esc(kv.first) << ":" << esc(kv.second); first = false; }
    o << "},\n" << ind << " \"analogs\":{";
    first = true;
    for (auto& kv : p.analogs) {
        o << (first ? "" : ",") << esc(kv.first) << ":{\"x\":" << esc(kv.second.x) << ",\"y\":" << esc(kv.second.y)
          << ",\"deadzone\":" << fmt_num(kv.second.deadzone) << ",\"sensitivity\":" << fmt_num(kv.second.sensitivity) << "}";
        first = false;
    }
    o << "}}";
}

bool read_profile(const J& j, const std::string& name, Profile* p) {
    if (j.t != J::Obj) return false;
    const J* sys = j.get("system");
    if (!sys || sys->t != J::Str || !is_system(sys->s)) return false;
    *p = default_profile(sys->s);   // missing entries fall back to the default (forward compatible)
    p->name = name;
    if (const J* t = j.get("axis_threshold")) if (t->t == J::Num && t->n >= 1 && t->n <= 32766) p->axis_threshold = (int)t->n;
    if (const J* b = j.get("buttons")) if (b->t == J::Obj)
        for (auto& kv : b->o) if (kv.second.t == J::Str && valid_expr(kv.second.s)) p->buttons[kv.first] = kv.second.s;
    if (const J* a = j.get("analogs")) if (a->t == J::Obj)
        for (auto& kv : a->o) {
            if (kv.second.t != J::Obj) continue;
            Analog an = p->analogs.count(kv.first) ? p->analogs[kv.first] : Analog{};
            if (auto* x = kv.second.get("x")) if (x->t == J::Str && (x->s.empty() || axis_index(x->s) >= 0)) an.x = x->s;
            if (auto* y = kv.second.get("y")) if (y->t == J::Str && (y->s.empty() || axis_index(y->s) >= 0)) an.y = y->s;
            if (auto* d = kv.second.get("deadzone")) if (d->t == J::Num && d->n >= 0 && d->n <= 0.95) an.deadzone = d->n;
            if (auto* sn = kv.second.get("sensitivity")) if (sn->t == J::Num && sn->n >= 0.1 && sn->n <= 4) an.sensitivity = sn->n;
            p->analogs[kv.first] = an;
        }
    return true;
}

void read_strs(const J* a, std::vector<std::string>* out) {
    if (a && a->t == J::Arr) for (auto& e : a->a) if (e.t == J::Str) out->push_back(e.s);
}

}  // namespace

std::string profile_to_json(const Profile& p) {
    std::ostringstream o;
    o << "{\"version\":1,\"name\":" << esc(p.name) << ",\"port_order\":[";
    for (size_t i = 0; i < p.port_order.size(); i++) o << (i ? "," : "") << esc(p.port_order[i]);
    o << "],\n\"profile\":\n";
    write_profile(o, p, "");
    o << "}\n";
    return o.str();
}

bool profile_from_json(const std::string& json, Profile* out) {
    Parser ps{json};
    J r = ps.val();
    if (!ps.ok || r.t != J::Obj) return false;
    const J* pj = r.get("profile");
    std::string name = r.get("name") && r.get("name")->t == J::Str ? r.get("name")->s : "";
    Profile p;
    if (!pj || !read_profile(*pj, name, &p)) return false;
    read_strs(r.get("port_order"), &p.port_order);
    *out = p;
    return true;
}

std::string to_json(const Store& s) {
    std::ostringstream o;
    o << "{\"version\":1,\"stamp\":" << s.stamp << ",\n\"port_order\":[";
    for (size_t i = 0; i < s.port_order.size(); i++) o << (i ? "," : "") << esc(s.port_order[i]);
    o << "],\n\"system_default\":{";
    bool f = true;
    for (auto& kv : s.system_default) { o << (f ? "" : ",") << esc(kv.first) << ":" << esc(kv.second); f = false; }
    o << "},\n\"game_override\":{";
    f = true;
    for (auto& kv : s.game_override) { o << (f ? "" : ",") << esc(kv.first) << ":" << esc(kv.second); f = false; }
    o << "},\n\"profiles\":{";
    f = true;
    for (auto& kv : s.profiles) { o << (f ? "\n" : ",\n") << esc(kv.first) << ":\n"; write_profile(o, kv.second, " "); f = false; }
    o << "}}\n";
    return o.str();
}

bool from_json(const std::string& json, Store* out) {
    Parser ps{json};
    J r = ps.val();
    if (!ps.ok || r.t != J::Obj) return false;
    Store s;
    if (auto* t = r.get("stamp")) if (t->t == J::Num) s.stamp = (int64_t)t->n;
    read_strs(r.get("port_order"), &s.port_order);
    if (auto* sd = r.get("system_default")) if (sd->t == J::Obj) for (auto& kv : sd->o) if (kv.second.t == J::Str && is_system(kv.first)) s.system_default[kv.first] = kv.second.s;
    if (auto* go = r.get("game_override")) if (go->t == J::Obj) for (auto& kv : go->o) if (kv.second.t == J::Str) s.game_override[kv.first] = kv.second.s;
    if (auto* pr = r.get("profiles")) if (pr->t == J::Obj)
        for (auto& kv : pr->o) { Profile p; if (read_profile(kv.second, kv.first, &p)) s.profiles[kv.first] = p; }
    *out = s;
    return true;
}

/* ------------------------------------------------------------------ files */

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static bool spit(const fs::path& p, const std::string& t) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) return false; f << t; if (!f) return false; }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(p, ec); fs::rename(tmp, p, ec); }
    return !ec;
}
static fs::path store_path(const std::string& dir) { return fs::u8path(dir) / "controls.json"; }

bool load_store(const std::string& userdata, const std::string& vault, Store* out) {
    Store a, b;
    bool ha = !userdata.empty() && from_json(slurp(store_path(userdata)), &a);
    bool hb = !vault.empty() && from_json(slurp(store_path(vault)), &b);
    if (ha && hb) *out = b.stamp > a.stamp ? b : a;
    else if (ha) *out = a;
    else if (hb) *out = b;
    else { *out = Store{}; return false; }
    return true;
}

bool save_store(const Store& s, const std::string& userdata, const std::string& vault) {
    std::string j = to_json(s);
    bool ok = true;
    if (!userdata.empty()) ok = spit(store_path(userdata), j) && ok;
    if (!vault.empty()) ok = spit(store_path(vault), j) && ok;
    return ok;
}

bool write_profile_file(const std::string& path, const Profile& p) { return spit(fs::u8path(path), profile_to_json(p)); }
bool read_profile_file(const std::string& path, Profile* out) { return profile_from_json(slurp(fs::u8path(path)), out); }

}  // namespace hubctl
