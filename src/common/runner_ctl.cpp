#include "runner_ctl.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "json.h"
#include "winutil.h"

namespace fs = std::filesystem;

namespace runctl {

static std::string number_text(double d) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", d);
    return b;
}

const char* cmd_name(Cmd c) {
    switch (c) {
        case Cmd::SaveState: return "save_state";
        case Cmd::LoadState: return "load_state";
        case Cmd::Screenshot: return "screenshot";
        case Cmd::SwapDisc: return "swap_disc";
        case Cmd::Quit: return "quit";
        case Cmd::Volume: return "volume";
        case Cmd::Mute: return "mute";
        case Cmd::Ff: return "ff";
        case Cmd::Speed: return "speed";
        case Cmd::Rewind: return "rewind";
        case Cmd::Shader: return "shader";
        case Cmd::Overlay: return "overlay";
        case Cmd::Clip: return "clip";
        case Cmd::OsdMsg: return "osd_msg";
        case Cmd::Input: return "input";
        default: return "none";
    }
}

Cmd cmd_from_name(const std::string& s) {
    for (Cmd c : {Cmd::SaveState, Cmd::LoadState, Cmd::Screenshot, Cmd::SwapDisc, Cmd::Quit, Cmd::Volume, Cmd::Mute, Cmd::Ff, Cmd::Speed,
                  Cmd::Rewind, Cmd::Shader, Cmd::Overlay, Cmd::Clip, Cmd::OsdMsg, Cmd::Input})
        if (s == cmd_name(c)) return c;
    return Cmd::None;
}

bool speed_valid(double s) {
    if (s == 0) return true;
    for (double k : kSpeeds) if (std::fabs(k - s) < 1e-9) return true;
    return false;
}

bool parse_speed_arg(const std::string& in, double* out) {
    std::string s;
    for (char c : in) if (c != ' ') s.push_back((char)std::tolower((unsigned char)c));
    if (s != "max" && !s.empty() && s.back() == 'x') s.pop_back();
    double v;
    if (s == "max") v = 0;
    else {
        char* end = nullptr;
        v = std::strtod(s.c_str(), &end);
        if (s.empty() || !end || *end || v <= 0) return false;
    }
    if (!speed_valid(v)) return false;
    if (out) *out = v;
    return true;
}

std::string speed_label(double s) {
    if (s == 0) return "Speed max";
    std::string n = number_text(s);
    return "Speed " + n + "x";
}

bool offline_only(Cmd c) {
    return c != Cmd::None && c != Cmd::Quit && c != Cmd::Screenshot && c != Cmd::OsdMsg && c != Cmd::Volume && c != Cmd::Mute && c != Cmd::Input && c != Cmd::Clip && c != Cmd::Overlay;   // clips and the stats overlay only read / display: allowed online
}

bool parse_control(const std::string& text, Command* out) {
    json::Value v;
    if (!json::parse(text, &v) || !v.is_object()) return false;
    Command c;
    c.seq = v.i64("seq", 0);
    if (c.seq <= 0) return false;
    c.cmd = cmd_from_name(v.str("cmd"));
    if (c.cmd == Cmd::None) return false;
    c.slot = (int)v.i64("slot", 0);
    if (c.cmd == Cmd::SaveState && (c.slot < 0 || c.slot >= kSlots)) return false;
    if (c.cmd == Cmd::LoadState && (c.slot < kAutoSlot || c.slot >= kSlots)) return false;
    c.value = (int)v.i64("value", 0);
    c.text = v.str("text");
    if (c.text.size() > 120) return false;
    if (const json::Value* sp = v.get("speed")) {
        if (sp->is_string()) { if (!parse_speed_arg(sp->s, &c.speed)) return false; }
        else if (sp->is_number()) c.speed = sp->n;
    }
    switch (c.cmd) {
        case Cmd::Volume: if (c.value < 0 || c.value > 100) return false; break;
        case Cmd::Mute: case Cmd::Input: case Cmd::Ff: case Cmd::Rewind: case Cmd::Overlay: if (c.value != 0 && c.value != 1) return false; break;
        case Cmd::Speed: if (!speed_valid(c.speed)) return false; break;
        case Cmd::Shader: case Cmd::OsdMsg: if (c.text.empty()) return false; break;
        default: break;
    }
    if (out) *out = c;
    return true;
}

std::string make_control(const Command& c) {
    return "{\"seq\":" + std::to_string(c.seq) + ",\"cmd\":\"" + cmd_name(c.cmd) + "\",\"slot\":" + std::to_string(c.slot) +
           ",\"value\":" + std::to_string(c.value) + ",\"speed\":" + number_text(c.speed) + ",\"text\":" + json::quote(c.text) + "}";
}

std::string make_ack(const Ack& a) {
    return "{\"seq\":" + std::to_string(a.seq) + ",\"ok\":" + (a.ok ? "true" : "false") + ",\"msg\":" + json::quote(a.msg) + "}";
}

bool parse_ack(const std::string& text, Ack* out) {
    json::Value v;
    if (!json::parse(text, &v) || !v.is_object() || !v.get("seq")) return false;
    Ack a;
    a.seq = v.i64("seq", 0);
    a.ok = v.boolean("ok", false);
    a.msg = v.str("msg");
    if (out) *out = a;
    return true;
}

std::string ack_path_for(const std::string& ctl) {
    fs::path p = fs::u8path(ctl);
    return (p.parent_path() / "control_ack.json").u8string();
}

static std::string slot_base(const std::string& dir, int slot) {
    std::string d = dir;
    while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    return d + "/" + (slot < 0 ? std::string("auto") : "slot" + std::to_string(slot));
}
std::string state_file(const std::string& d, int s) { return slot_base(d, s) + ".state"; }
std::string thumb_file(const std::string& d, int s) { return slot_base(d, s) + ".png"; }
std::string sidecar_file(const std::string& d, int s) { return slot_base(d, s) + ".json"; }

std::string shots_dir(const std::string& game_dir) {
    std::string d = game_dir;
    while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    return d + "/shots";
}

std::string shot_name(std::time_t t) {
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[40];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S.png", &tm);
    return b;
}

std::string make_sidecar(const Sidecar& s) {
    return "{\"game_key\":" + json::quote(s.game_key) + ",\"time\":" + std::to_string(s.time) + ",\"playtime\":" + std::to_string(s.playtime) +
           ",\"disc\":" + std::to_string(s.disc) + "}";
}

bool parse_sidecar(const std::string& text, Sidecar* out) {
    json::Value v;
    if (!json::parse(text, &v) || !v.is_object()) return false;
    Sidecar s;
    s.game_key = v.str("game_key");
    s.time = v.i64("time", 0);
    s.playtime = v.i64("playtime", 0);
    s.disc = (int)v.i64("disc", 0);
    if (out) *out = s;
    return true;
}

bool write_text_atomic(const std::string& path, const std::string& text) {
    fs::path p = fs::u8path(path);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {   // Windows: rename over an open/existing file can fail; fall back to overwrite
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
        if (ec) { fs::remove(tmp, ec); return false; }
    }
    return true;
}

bool read_text(const std::string& path, std::string* out) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

long long send_command(const std::string& ctl, Cmd cmd, int slot) {
    Command c;
    c.cmd = cmd; c.slot = slot;
    return send_command_ex(ctl, c);
}

long long send_command_ex(const std::string& ctl, Command c) {
    std::string cur;
    Command prev;
    long long seq = 1;
    if (read_text(ctl, &cur)) {
        json::Value v;
        if (json::parse(cur, &v) && v.is_object()) seq = v.i64("seq", 0) + 1;
        else seq = (long long)std::time(nullptr);   // corrupt file: a clock-based seq stays above any earlier one
    }
    (void)prev;
    if (seq < 1) seq = 1;
    c.seq = seq;
    return write_text_atomic(ctl, make_control(c)) ? seq : 0;
}

Poller::Poller(std::string p) : path_(std::move(p)) {
    stamp_ = stamp();
    std::string t;
    Command c;
    if (!path_.empty() && read_text(path_, &t) && parse_control(t, &c)) last_ = c.seq;   // already there at start-up: handled
}

std::string Poller::stamp() const {
    if (path_.empty()) return std::string();
    std::error_code ec;
    fs::path p = fs::u8path(path_);
    auto t = fs::last_write_time(p, ec);
    if (ec) return "missing";
    auto sz = fs::file_size(p, ec);
    return std::to_string((long long)t.time_since_epoch().count()) + ":" + std::to_string(ec ? 0ull : (unsigned long long)sz);
}

bool Poller::poll(uint64_t now_ms, Command* out, uint64_t interval_ms) {
    if (path_.empty() || now_ms < next_) return false;
    next_ = now_ms + interval_ms;
    std::string s = stamp();
    if (s == stamp_) return false;
    stamp_ = s;
    std::string t;
    Command c;
    if (!read_text(path_, &t) || !parse_control(t, &c)) { stamp_.clear(); return false; }   // mid-write / locked: look again next poll, don't drop the command
    if (c.seq <= last_) return false;
    last_ = pending_ = c.seq;
    if (out) *out = c;
    return true;
}

void Poller::ack(bool ok, const std::string& msg) const {
    if (path_.empty() || pending_ <= 0) return;
    Ack a;
    a.seq = pending_; a.ok = ok; a.msg = msg;
    write_text_atomic(ack_path_for(path_), make_ack(a));
}

}  // namespace runctl
