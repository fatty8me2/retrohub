#include "hookshot.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../common/games.h"
#include "../common/json.h"
#include "../common/library.h"

namespace fs = std::filesystem;

namespace hookshot {

static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::string S(const fs::path& p) { return p.u8string(); }

bool supports(const hub::GameEntry& e) {
    if (e.system != hub::System::Switch) return false;
    const std::string want = "switch:0400486024dec000";
    if (e.key.size() < want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i) {
        char c = e.key[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != want[i]) return false;
    }
    return e.key.size() == want.size() || e.key[want.size()] == '-' || e.key[want.size()] == '_';
}

std::string game_dir() { return S(P(hub::data_dir()) / "emulators" / "hookshot"); }
std::string exe_path() { return S(P(game_dir()) / "hookshot.exe"); }
std::string user_dir() { return S(P(game_dir()) / "user"); }

static const char* kStampName = "retrohub_hookshot.json";

bool read_stamp(const std::string& dir, Stamp* out) {
    std::ifstream f(P(dir) / kStampName, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    json::Value v;
    if (!json::parse(ss.str(), &v) || !v.is_object()) return false;
    Stamp s;
    s.commit = v.str("commit");
    s.game_build = v.str("game_build");
    s.source = v.str("source");
    s.built = v.i64("built");
    s.size = (uint64_t)v.i64("size");
    if (s.commit.empty()) return false;
    if (out) *out = s;
    return true;
}

bool write_stamp(const std::string& dir, const Stamp& s) {
    std::string text = std::string("{\"commit\":") + json::quote(s.commit) + ",\"game_build\":" + json::quote(s.game_build) +
                       ",\"built\":" + std::to_string(s.built) + ",\"source\":" + json::quote(s.source) +
                       ",\"size\":" + std::to_string(s.size) + "}\n";
    const fs::path tmp = P(dir) / (std::string(kStampName) + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, P(dir) / kStampName, ec);
    return !ec;
}

bool ready(std::string* why, const std::string& dir_in) {
    const fs::path d = P(dir_in.empty() ? game_dir() : dir_in);
    auto miss = [&](const char* what) { if (why) *why = what; return false; };
    std::error_code ec;
    if (!fs::is_regular_file(d / "hookshot.exe", ec)) return miss("not built yet");
    if (!fs::is_regular_file(d / "hookshot_vk.dll", ec)) return miss("the build is incomplete (hookshot_vk.dll is missing)");
    if (!fs::is_regular_file(d / "SDL2.dll", ec)) return miss("the build is incomplete (SDL2.dll is missing)");
    if (!fs::is_directory(d / "romfs", ec)) return miss("the build is incomplete (the game's romfs folder is missing)");
    Stamp s;
    if (!read_stamp(S(d), &s)) return miss("the build was not made by RetroHub (no build stamp)");
    if (s.commit != kSourceCommit) return miss("the build is from another Hookshot version: rebuild it");
    if (why) why->clear();
    return true;
}

}   // namespace hookshot
