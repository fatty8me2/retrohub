#include "runner_ctl.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "json.h"
#include "winutil.h"

namespace fs = std::filesystem;

namespace runctl {

const char* cmd_name(Cmd c) {
    switch (c) {
        case Cmd::SaveState: return "save_state";
        case Cmd::LoadState: return "load_state";
        case Cmd::Screenshot: return "screenshot";
        case Cmd::SwapDisc: return "swap_disc";
        case Cmd::Quit: return "quit";
        default: return "none";
    }
}

Cmd cmd_from_name(const std::string& s) {
    for (Cmd c : {Cmd::SaveState, Cmd::LoadState, Cmd::Screenshot, Cmd::SwapDisc, Cmd::Quit})
        if (s == cmd_name(c)) return c;
    return Cmd::None;
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
    if (out) *out = c;
    return true;
}

std::string make_control(const Command& c) {
    return "{\"seq\":" + std::to_string(c.seq) + ",\"cmd\":\"" + cmd_name(c.cmd) + "\",\"slot\":" + std::to_string(c.slot) + "}";
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

std::string shots_dir(const std::string& vault, const std::string& key) {
    std::string d = vault;
    while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    return d + "/" + key + "/shots";
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
    Command c;
    c.seq = seq; c.cmd = cmd; c.slot = slot;
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
    if (!read_text(path_, &t) || !parse_control(t, &c)) return false;
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
