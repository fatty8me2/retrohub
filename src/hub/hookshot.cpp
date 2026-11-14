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
#include "../common/winutil.h"

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

/* Longshot v1.0.0 builds to the same flat layout as Hookshot (its program is still hookshot.exe; the Longshot.exe launcher is a copy of its
 * builder set-up program, which RetroHub never has, so make_game.py never makes the bin\ layout). */
static const FlavorInfo kInfo[kFlavorCount] = {
    {Flavor::Hookshot, "hookshot", "Hookshot", "hookshot.exe", "user", kSourceCommit, kSourceVersion, kPreviousAuditedCommit,
     "hookshot-src-8ef8461.zip", "05ab70bf02640f35ef22fd670200d3f3eb0187eef31efe1f84778a0a12d283d5", 100, "-hookshot"},
    {Flavor::Longshot, "longshot", "Longshot", "hookshot.exe", "user", "5c1f00dd444e735bf27b73b2f91c0617f930ddac", "v1.0.0", "",
     "longshot-src-5c1f00d.zip", "ec0aaa4590c1e28de932d429074d8171c340447ef227a9686aceb0e6ccc072ee", 101, "-longshot"},
};
const FlavorInfo& info(Flavor f) { return kInfo[(int)f == 1 ? 1 : 0]; }

bool flavor_from_id(const std::string& id, Flavor* out) {
    for (const FlavorInfo& i : kInfo)
        if (id == i.id) { if (out) *out = i.flavor; return true; }
    return false;
}

std::string game_dir(Flavor f) { return S(P(hub::data_dir()) / "emulators" / info(f).id); }
std::string exe_path(Flavor f) { return S(P(game_dir(f)) / P(info(f).exe_rel)); }
std::string user_dir(Flavor f) { return S(P(game_dir(f)) / P(info(f).user_rel)); }

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

bool ready(std::string* why, const std::string& dir_in, Flavor f) {
    const FlavorInfo& fi = info(f);
    const fs::path d = P(dir_in.empty() ? game_dir(f) : dir_in);
    auto miss = [&](const std::string& what) { if (why) *why = what; return false; };
    std::error_code ec;
    const fs::path exe = d / P(fi.exe_rel), bin = exe.parent_path();
    if (!fs::is_regular_file(exe, ec)) return miss("not built yet");
    if (!fs::is_regular_file(bin / "hookshot_vk.dll", ec)) return miss("the build is incomplete (hookshot_vk.dll is missing)");
    if (!fs::is_regular_file(bin / "SDL2.dll", ec)) return miss("the build is incomplete (SDL2.dll is missing)");
    if (!fs::is_directory(bin / "romfs", ec) && !fs::is_directory(d / "romfs", ec)) return miss("the build is incomplete (the game's romfs folder is missing)");
    Stamp s;
    if (!read_stamp(S(d), &s)) return miss("the build was not made by RetroHub (no build stamp)");
    if (s.commit != fi.source_commit && !(fi.previous_commit[0] && s.commit == fi.previous_commit)) return miss(std::string("the build is from another ") + fi.name + " version: rebuild it");
    if (why) why->clear();
    return true;
}

bool is_older_audited(const Stamp& s, Flavor f) { const FlavorInfo& fi = info(f); return fi.previous_commit[0] && s.commit == fi.previous_commit; }

Decision decide(const hub::GameEntry& e, const Values& v) {
    Decision d;
    if (!supports(e)) return d;
    auto it = v.find(kRunnerSetting);
    if (it == v.end() || !flavor_from_id(it->second, &d.flavor)) return d;   // unset, "lichii", "fritchii" and the old v1.37.0 "auto": an emulator
    d.choice = ready(&d.why, std::string(), d.flavor) ? Choice::Native : Choice::NativeNotBuilt;   // chosen but missing is its own answer: never quietly an emulator
    return d;
}

bool use_for(const hub::GameEntry& e, const Values& v) {
    const Decision d = decide(e, v);
    return d.choice == Choice::Native && d.flavor == Flavor::Hookshot;
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

hubemu::SaveSession begin_save_session(const hub::GameEntry& e, const std::string& battery_in, Flavor f) {
    hubemu::SaveSession s;
    s.started_unix = (int64_t)std::time(nullptr);
    std::string battery = battery_in;
    while (!battery.empty() && (battery.back() == '\\' || battery.back() == '/')) battery.pop_back();
    s.battery = battery;
    if (!supports(e) || battery.empty()) return s;
    /* vault layout: system dir / game dir / battery -> _conflicts/<system>/<game>/battery; manifest .sync/emu-<system>-<game>-hookshot.json */
    const fs::path game = P(battery).parent_path(), sysdir = game.parent_path(), vault = sysdir.parent_path();
    s.backup_dir = S(vault / L"_conflicts" / sysdir.filename() / game.filename() / L"battery");
    s.manifest_path = S(vault / L".sync" / (L"emu-" + sysdir.filename().wstring() + L"-" + game.filename().wstring() + winutil::to_wide(info(f).manifest_suffix) + L".json"));
    s.emu_root = user_dir(f);
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
