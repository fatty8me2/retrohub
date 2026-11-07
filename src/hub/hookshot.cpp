#include "hookshot.h"

#include <cstdio>
#include <ctime>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../common/games.h"
#include "../common/json.h"
#include "../common/library.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

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

bool use_for(const hub::GameEntry& e, const Values& v) {
    if (!supports(e)) return false;
    auto it = v.find(kRunnerSetting);
    if (it != v.end() && it->second == "lichii") return false;
    return ready();
}

static bool av1_probe() {
    const HRESULT hs = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hs)) return false;
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_AV1};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_ALL, &in, nullptr, &acts, &n);
    bool found = SUCCEEDED(hr) && n > 0;
    if (acts) {
        for (UINT32 i = 0; i < n; ++i) if (acts[i]) acts[i]->Release();
        CoTaskMemFree(acts);
    }
    MFShutdown();
    return found;
}

bool av1_decoder_present(bool force_recheck) {
    static std::mutex mu;
    static int cached = -1;
    std::lock_guard<std::mutex> lk(mu);
    if (force_recheck || cached < 0) cached = av1_probe() ? 1 : 0;
    return cached == 1;
}

hubemu::SaveSession begin_save_session(const hub::GameEntry& e, const std::string& battery_in) {
    hubemu::SaveSession s;
    s.started_unix = (int64_t)std::time(nullptr);
    std::string battery = battery_in;
    while (!battery.empty() && (battery.back() == '\\' || battery.back() == '/')) battery.pop_back();
    s.battery = battery;
    if (!supports(e) || battery.empty()) return s;
    /* vault layout: system dir / game dir / battery -> _conflicts/<system>/<game>/battery; manifest .sync/emu-<system>-<game>-hookshot.json */
    const fs::path game = P(battery).parent_path(), sysdir = game.parent_path(), vault = sysdir.parent_path();
    s.backup_dir = S(vault / L"_conflicts" / sysdir.filename() / game.filename() / L"battery");
    s.manifest_path = S(vault / L".sync" / (L"emu-" + sysdir.filename().wstring() + L"-" + game.filename().wstring() + L"-hookshot.json"));
    s.emu_root = user_dir();
    s.rels = {kSaveRel};
    char tb[24];
    std::snprintf(tb, sizeof tb, "%016llX", (unsigned long long)kTitleId);   // uppercase, as the Lichii branch writes it
    s.vault_rels = {std::string("save/") + tb};
    std::error_code ec;
    fs::create_directories(P(s.emu_root) / kSaveRel, ec);
    s.active = true;
    hubemu::sync_vault_to_emulator(s);
    return s;
}

}   // namespace hookshot
