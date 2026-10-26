#include "state_slots.h"

#include "json.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace hubui {

namespace {
std::string base_name(int slot) { return slot < 0 ? std::string("auto") : "slot" + std::to_string(slot); }
std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char c = dir.back();
    return (c == '\\' || c == '/') ? dir + name : dir + "\\" + name;
}
bool read_all(const std::string& p, std::string* out) {
    std::ifstream f(fs::u8path(p), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}
uint64_t mtime_unix(const fs::path& p) {
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec) return 0;
    auto sys = std::chrono::time_point_cast<std::chrono::seconds>(ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    long long s = sys.time_since_epoch().count();
    return s > 0 ? (uint64_t)s : 0;
}
}  // namespace

std::vector<StateSlot> list_states(const std::string& dir) {
    std::vector<StateSlot> out;
    for (int slot = -1; slot < kStateSlots; ++slot) {
        StateSlot s;
        s.slot = slot;
        std::string b = base_name(slot);
        s.state_path = join(dir, b + ".state");
        fs::path sp = fs::u8path(s.state_path);
        std::error_code ec;
        s.present = fs::is_regular_file(sp, ec);
        if (s.present) {
            s.bytes = (uint64_t)fs::file_size(sp, ec);
            s.time = mtime_unix(sp);
            std::string tp = join(dir, b + ".png");
            if (fs::is_regular_file(fs::u8path(tp), ec)) s.thumb_path = tp;
            std::string txt;
            json::Value j;
            if (read_all(join(dir, b + ".json"), &txt) && json::parse(txt, &j) && j.is_object()) {
                long long t = j.i64("time", 0);
                if (t > 0) s.time = (uint64_t)t;
                long long pt = j.i64("playtime", 0);
                s.playtime = pt > 0 ? (uint64_t)pt : 0;
                s.disc = (int)j.i64("disc", 0);
            }
        }
        out.push_back(s);
    }
    return out;
}

int count_states(const std::vector<StateSlot>& v, bool include_auto) {
    int n = 0;
    for (auto& s : v) if (s.present && (include_auto || s.slot >= 0)) ++n;
    return n;
}

bool delete_state(const std::string& dir, int slot) {
    std::string b = base_name(slot);
    std::error_code ec;
    fs::remove(fs::u8path(join(dir, b + ".png")), ec);
    fs::remove(fs::u8path(join(dir, b + ".json")), ec);
    fs::remove(fs::u8path(join(dir, b + ".state")), ec);
    return !fs::exists(fs::u8path(join(dir, b + ".state")), ec);
}

std::string state_slot_label(int slot) { return slot < 0 ? std::string("Auto") : "Slot " + std::to_string(slot); }

bool system_has_states(const std::string& s, bool native) {
    if (native) return false;
    return s == "n64" || s == "gb" || s == "gbc" || s == "gba" || s == "nds" || s == "ps1" || s == "nes" || s == "snes";   // retrorun systems; n64run too (mupen64plus states)
}

std::string no_states_reason(const std::string& s, bool native) {
    if (system_has_states(s, native)) return "";
    if (native) return "Smash Classic keeps its own saves; it has no save states.";
    const char* emu = s == "gc" ? "Dolphin" : s == "wiiu" ? "Cemu" : s == "3ds" ? "Azahar" : s == "ps2" ? "PCSX2" : nullptr;
    if (emu) return std::string("Save states in ") + emu + " are made inside the emulator, so RetroHub can't list them here.";
    return "Save states aren't available for this system yet.";
}

std::string ago_text(uint64_t now, uint64_t t) {
    if (!t) return "never";
    if (t >= now || now - t < 60) return "just now";
    uint64_t d = now - t;
    if (d < 3600) return std::to_string(d / 60) + " min ago";
    if (d < 86400) return std::to_string(d / 3600) + "h ago";
    uint64_t days = d / 86400;
    if (days == 1) return "yesterday";
    if (days < 14) return std::to_string(days) + " days ago";
    return std::to_string(days / 7) + " weeks ago";
}

std::string resume_line(const std::string& title, uint64_t now, uint64_t last_played, const std::string& sep) {
    if (!last_played) return "Play " + title;
    return "Resume " + title + sep + "last played " + ago_text(now, last_played);
}

bool can_resume(bool supp, bool auto_present) { return supp && auto_present; }

std::string playtime_text(uint64_t s) {
    if (!s) return "";
    char b[32];
    if (s >= 3600) std::snprintf(b, sizeof b, "%lluh %02llum", (unsigned long long)(s / 3600), (unsigned long long)((s % 3600) / 60));
    else std::snprintf(b, sizeof b, "%llu:%02llu", (unsigned long long)(s / 60), (unsigned long long)(s % 60));
    return b;
}

const char* sort_name(SortMode m) {
    switch (m) {
        case SortMode::LastPlayed: return "Last played";
        case SortMode::MostPlayed: return "Most played";
        case SortMode::RecentlyAdded: return "Recently added";
        default: return "Title";
    }
}
const char* sort_key(SortMode m) {
    switch (m) {
        case SortMode::LastPlayed: return "last_played";
        case SortMode::MostPlayed: return "most_played";
        case SortMode::RecentlyAdded: return "recently_added";
        default: return "title";
    }
}
SortMode sort_from_key(const std::string& k) {
    for (int i = 0; i < kSortModes; ++i) if (k == sort_key((SortMode)i)) return (SortMode)i;
    return SortMode::Title;
}
SortMode next_sort(SortMode m, int dir) { return (SortMode)(((int)m + (dir < 0 ? kSortModes - 1 : 1)) % kSortModes); }

}  // namespace hubui
