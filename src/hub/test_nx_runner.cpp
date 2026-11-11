// Fritchii runner (nx_runner.h): runner resolution, environment, settings pin, key + shader seed, and the save comparison across Lichii / Fritchii / the vault.
// Temp folders only; nothing is launched, nothing touches the real data dir or %LOCALAPPDATA%\RetroHub (the backup root is a temp folder too).
#include "nx_runner.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "lichii.h"

namespace fs = std::filesystem;
using namespace hubemu;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        } else {                                                        \
            ++g_pass;                                                   \
        }                                                               \
    } while (0)

static std::string U8(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
static std::string rd(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void wr(const fs::path& p, const std::string& t, int age_minutes = 0) {   // age_minutes: how long ago it was last written
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(t.data(), (std::streamsize)t.size());
    }
    fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::minutes(age_minutes), ec);
}
static bool has(const std::vector<std::pair<std::string, std::string>>& env, const char* k, const char* v = nullptr) {
    for (const auto& kv : env) if (kv.first == k && (!v || kv.second == v)) return true;
    return false;
}
static int count_files(const fs::path& root) {
    int n = 0;
    std::error_code ec;
    if (!fs::exists(root, ec)) return 0;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) if (it->is_regular_file(ec)) ++n;
    return n;
}
// all files below root whose content is `text`
static int files_with(const fs::path& root, const std::string& text) {
    int n = 0;
    std::error_code ec;
    if (!fs::exists(root, ec)) return 0;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) if (it->is_regular_file(ec) && rd(it->path()) == text) ++n;
    return n;
}

static const uint64_t kTid = 0x0400486024DEC000ull;
static const char* kTidHex = "0400486024DEC000";

static fs::path save_dir(const fs::path& install) {   // <install>\user\nand\user\save\0000000000000000\<uuid>\<TID>
    return install / "user" / "nand" / "user" / "save" / "0000000000000000" / kLichiiUserUuid / kTidHex;
}
static fs::path vault_save(const fs::path& battery) { return battery / "save" / kTidHex; }

struct World {
    fs::path root, lichii, fritchii, battery, backups;
    explicit World(const fs::path& r) : root(r), lichii(r / "lichii"), fritchii(r / "fritchii"), battery(r / "vault" / "switch" / "switch_0400486024dec000" / "battery"), backups(r / "save-backups") {
        std::error_code ec;
        fs::create_directories(r, ec);
    }
    NxSaveSync req(Kind k) const {
        NxSaveSync s;
        s.launching = k;
        s.title_id = kTid;
        s.lichii_dir = U8(lichii);
        s.fritchii_dir = U8(fritchii);
        s.battery = U8(battery);
        s.game_label = "switch_0400486024dec000";
        s.backup_root = U8(backups);
        return s;
    }
    void install_dirs() {   // both emulators "installed": a user\ folder each
        std::error_code ec;
        fs::create_directories(lichii / "user", ec);
        fs::create_directories(fritchii / "user", ec);
    }
};

int main() {
    const fs::path tmp = fs::temp_directory_path() / ("hub_nx_runner_test_" + std::to_string((unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    // ---- runner resolution
    {
        hub::GameEntry sw;
        sw.system = hub::System::Switch;
        sw.key = "switch:0400486024dec000";
        hub::GameEntry gc;
        gc.system = hub::System::GC;
        hub::gs::Values v;
        CHECK(nx_runner_value(v) == NxRunner::Lichii);                  // nothing stored = Lichii
        CHECK(!fritchii_selected(sw, v));
        v["switch.runner"] = "fritchii";
        CHECK(nx_runner_value(v) == NxRunner::Fritchii);
        CHECK(fritchii_selected(sw, v));
        CHECK(!fritchii_selected(gc, v));                               // only Switch games
        v["switch.runner"] = "hookshot";
        CHECK(nx_runner_value(v) == NxRunner::Hookshot);
        CHECK(!fritchii_selected(sw, v));
        v["switch.runner"] = "auto";                                    // the old v1.37.0 value
        CHECK(nx_runner_value(v) == NxRunner::Lichii);
        v["switch.runner"] = "FRITCHII";                                // exact match only
        CHECK(nx_runner_value(v) == NxRunner::Lichii);
        CHECK(is_nx(Kind::Lichii) && is_nx(Kind::Fritchii) && !is_nx(Kind::Dolphin));
        CHECK(std::string(name(Kind::Fritchii)) == "Fritchii");
        CHECK(std::string(nx_manifest_suffix(Kind::Lichii)).empty());
        CHECK(std::string(nx_manifest_suffix(Kind::Fritchii)) == "-fritchii");
    }

    // ---- default runner: Ocarina of Time plays with Fritchii unless somebody chose
    {
        hub::GameEntry oot, other, oot_v2, gc;
        oot.system = other.system = oot_v2.system = hub::System::Switch;
        oot.key = "switch:0400486024dec000";
        oot_v2.key = "switch:0400486024DEC000-v2";
        other.key = "switch:0100abcd12340000";
        gc.system = hub::System::GC;
        gc.key = "gc:0400486024dec000";
        hub::gs::Values lich, fri;
        lich["switch.runner"] = "lichii";
        fri["switch.runner"] = "fritchii";
        const bool on = true;
        CHECK(kOotTitleId == 0x0400486024DEC000ull);
        CHECK(nx_emulator_for(oot, lich, false, 0, on) == Kind::Fritchii);                    // default: OoT -> Fritchii
        CHECK(nx_emulator_for(oot_v2, lich, false, 0, on) == Kind::Fritchii);
        CHECK(nx_emulator_for(other, lich, false, 0, on) == Kind::Lichii);                    // other Switch games stay on Lichii
        CHECK(nx_emulator_for(gc, lich, false, 0, on) == Kind::Lichii);
        CHECK(nx_emulator_for(oot, lich, false, 2, on) == Kind::Lichii);                      // the player picked Lichii for this game
        CHECK(nx_emulator_for(oot, lich, false, 1, on) == Kind::Lichii);                      // ...or for all of Switch
        CHECK(nx_emulator_for(oot, fri, false, 2, on) == Kind::Fritchii && nx_emulator_for(other, fri, false, 1, on) == Kind::Fritchii);
        CHECK(nx_emulator_for(oot, lich, true, 0, on) == Kind::Lichii);                       // online and rooms: always Lichii
        CHECK(nx_emulator_for(oot, fri, true, 2, on) == Kind::Lichii);
        CHECK(nx_emulator_for(oot, lich, false, 0, false) == Kind::Lichii);                   // Fritchii neither installed nor downloadable: the default does not block Play
        CHECK(nx_emulator_for(oot, fri, false, 2, false) == Kind::Fritchii);                  // ...but an explicit choice still gets the install prompt
        // the shared decision (launch, option rows, "Plays with"): obtainable = installed OR the release is pinned
        CHECK(nx_fritchii_obtainable(false, false) && nx_fritchii_obtainable(true, true) && nx_fritchii_obtainable(false, true) && !nx_fritchii_obtainable(true, false));
        const bool pinned_not_installed = nx_fritchii_obtainable(false, false), unpinned_not_installed = nx_fritchii_obtainable(true, false), installed = nx_fritchii_obtainable(true, true);
        CHECK(nx_emulator_for(oot, lich, false, 0, installed) == Kind::Fritchii);              // default + Fritchii installed
        CHECK(nx_emulator_for(oot, lich, false, 0, pinned_not_installed) == Kind::Fritchii);   // default + not installed but downloadable: Play offers the install, then Fritchii
        CHECK(nx_emulator_for(oot, lich, false, 0, unpinned_not_installed) == Kind::Lichii);   // default + not installed and not downloadable
        CHECK(nx_emulator_for(oot, lich, false, 2, unpinned_not_installed) == Kind::Lichii);   // explicit lichii
        CHECK(nx_emulator_for(oot, fri, false, 2, unpinned_not_installed) == Kind::Fritchii);  // explicit fritchii
        CHECK(nx_emulator_for(oot, lich, true, 0, installed) == Kind::Lichii && nx_emulator_for(oot, fri, true, 2, installed) == Kind::Lichii);   // online / room
        CHECK(nx_emulator_for(other, lich, false, 0, installed) == Kind::Lichii);              // another Switch game
        hub::gs::Values hs;
        hs["switch.runner"] = "hookshot";
        CHECK(nx_emulator_for(oot, hs, false, 2, on) == Kind::Lichii);                        // Hookshot is decided before this (launch.cpp)
        // through the real settings store
        hub::gs::Store st;
        const std::string k = oot.key;
        auto pick = [&] { return nx_emulator_for(oot, st.resolve(hub::System::Switch, k), false, st.source(hub::System::Switch, k, kRunnerKey), on); };
        CHECK(pick() == Kind::Fritchii);
        CHECK(st.set_system(hub::System::Switch, kRunnerKey, "lichii") && pick() == Kind::Lichii);
        st.clear_system_key(hub::System::Switch, kRunnerKey);
        CHECK(pick() == Kind::Fritchii);
        CHECK(st.set_game(k, hub::System::Switch, kRunnerKey, "lichii") && pick() == Kind::Lichii);
        st.clear_game_key(k, kRunnerKey);
        CHECK(pick() == Kind::Fritchii);
    }

    // ---- environment
    {
        const uint64_t GiB = 1ull << 30;
        hub::gs::Values v;
        v["switch.preload_shaders"] = "auto";
        // Lichii: exactly lichii_env
        CHECK(nx_env(Kind::Lichii, v, 32 * GiB) == lichii_env(v, 32 * GiB));
        CHECK(has(nx_env(Kind::Lichii, v, 64 * GiB), "EDEN_NX2_PRELOAD_ALL_PIPELINES", "1"));
        CHECK(!has(nx_env(Kind::Lichii, v, 32 * GiB), "EDEN_NX2_PRELOAD_ALL_PIPELINES"));   // auto: a "32 GB" PC builds only recent shaders at start
        CHECK(nx_env(Kind::Lichii, v, 16 * GiB).size() == 1 && has(nx_env(Kind::Lichii, v, 16 * GiB), "EDEN_NX2_NO_REACTIVE", "1"));   // only the fast-reads var
        CHECK(has(nx_env(Kind::Fritchii, v, 16 * GiB), "EDEN_NX2_NO_REACTIVE", "1"));
        {
            hub::gs::Values off = v;
            off["switch.fast_reads"] = "off";
            CHECK(nx_env(Kind::Lichii, off, 16 * GiB).empty());
            CHECK(!has(nx_env(Kind::Fritchii, off, 16 * GiB), "EDEN_NX2_NO_REACTIVE") && has(nx_env(Kind::Lichii, off, 64 * GiB), "EDEN_NX2_PRELOAD_ALL_PIPELINES", "1"));
        }
        CHECK(!has(nx_env(Kind::Lichii, v, 32 * GiB), "EDEN_UPDATE_URL"));
        // Fritchii: "auto" never preloads (29k pipelines = ~13 GB), "all" does, "recent" does not; the updater URL is always pinned
        CHECK(!has(nx_env(Kind::Fritchii, v, 64 * GiB), "EDEN_NX2_PRELOAD_ALL_PIPELINES"));
        CHECK(has(nx_env(Kind::Fritchii, v, 64 * GiB), "EDEN_UPDATE_URL", "http://127.0.0.1:9/x"));
        v["switch.preload_shaders"] = "all";
        CHECK(!has(nx_env(Kind::Fritchii, v, 8 * GiB), "EDEN_NX2_PRELOAD_ALL_PIPELINES") && !has(nx_env(Kind::Fritchii, v, 64 * GiB), "EDEN_NX2_PRELOAD_ALL_PIPELINES"));   // v5 prepares shaders itself
        CHECK(has(nx_env(Kind::Lichii, v, 8 * GiB), "EDEN_NX2_PRELOAD_ALL_PIPELINES", "1"));   // Lichii keeps its behaviour
        v["switch.preload_shaders"] = "recent";
        CHECK(!has(nx_env(Kind::Fritchii, v, 64 * GiB), "EDEN_NX2_PRELOAD_ALL_PIPELINES"));
        CHECK(has(nx_env(Kind::Fritchii, v, 64 * GiB), "EDEN_UPDATE_URL"));
        CHECK(nx_env(Kind::Fritchii, hub::gs::Values{}, 0).size() == 2);   // no setting at all: the updater pin and fast reads (missing counts as on)
        // never these
        for (const char* mode : {"auto", "all", "recent"}) {
            v["switch.preload_shaders"] = mode;
            for (Kind k : {Kind::Lichii, Kind::Fritchii}) {
                auto e = nx_env(k, v, 64 * GiB);
                CHECK(!has(e, "EDEN_NX2_DEBUG_SERVER") && !has(e, "EDEN_NX2_CRASH_DUMP"));
                CHECK(!has(e, "FRITCHII_CLASSIC_UI") && !has(e, "EDEN_NX2_PRECOMPILE") && !has(e, "EDEN_NX2_PRECOMPILE_RECIPE"));   // would skip / break v5's shader preparation
            }
            hub::gs::Values off = v;
            off["switch.fast_reads"] = "off";
            auto e2 = nx_env(Kind::Fritchii, off, 64 * GiB);
            CHECK(!has(e2, "FRITCHII_CLASSIC_UI") && !has(e2, "EDEN_NX2_PRECOMPILE") && !has(e2, "EDEN_NX2_PRELOAD_ALL_PIPELINES"));
        }
        CHECK(nx_skip_keys(Kind::Lichii).empty());
    }

    // ---- qt-config pin and frizti.ini
    {
        const std::string q = fritchii_ini_pin("[Renderer]\nresolution_setup\\default=false\nresolution_setup=3\n");
        CHECK(q.find("frizti_settings_revision\\default=false") != std::string::npos);
        CHECK(q.find("frizti_settings_revision=999") != std::string::npos);
        CHECK(q.find("oot_fps_asked\\default=false") != std::string::npos);
        CHECK(q.find("oot_fps_asked=true") != std::string::npos);
        CHECK(q.find("resolution_setup=3") != std::string::npos);        // RetroHub's other keys stay
        CHECK(fritchii_ini_pin(q) == q);                                  // idempotent
        {   // every launch writes check_for_updates=false (even if the player switched it on); the pin marks the pinned build's What's new as seen
            const std::string l = lichii_ini_launch("[UI]\ncheck_for_updates\\default=false\ncheck_for_updates=true\n", hub::gs::Values{}, false, &nx_skip_keys(Kind::Fritchii));
            CHECK(l.find("check_for_updates=false") != std::string::npos && l.find("check_for_updates=true") == std::string::npos);
            CHECK(q.find("fritchii_whats_new_seen=v5.7") != std::string::npos && fritchii_ini_pin(q) == q);
        }
        {   // the recipe's dynamic state: EDS2 and dynamic vertex input, whatever the player set in Fritchii's own menus
            const std::string d = fritchii_ini_pin("[Renderer]\ndyna_state\\default=false\ndyna_state=0\nvertex_input_dynamic_state\\default=false\nvertex_input_dynamic_state=false\n");
            CHECK(d.find("dyna_state=2") != std::string::npos && d.find("dyna_state=0") == std::string::npos);
            CHECK(d.find("vertex_input_dynamic_state=true") != std::string::npos && d.find("vertex_input_dynamic_state=false") == std::string::npos);
            CHECK(q.find("dyna_state=2") != std::string::npos && q.find("vertex_input_dynamic_state\\default=false") != std::string::npos);
        }
        const std::string old = "revision=3\nglobal.gpu_accuracy=1\nglobal.use_vsync=2\ngame.0400486024DEC000.resolution_setup=2\n# note\n";
        const std::string n = frizti_ini_pin(old);
        CHECK(n.find("revision=999") != std::string::npos);
        CHECK(n.find("revision=3") == std::string::npos);
        CHECK(n.find("global.") == std::string::npos);                    // every global line dropped
        CHECK(n.find("game.0400486024DEC000.resolution_setup=2") != std::string::npos);   // game lines kept
        CHECK(frizti_ini_pin(n) == n);
        const std::string fresh = frizti_ini_pin("");                      // missing file
        CHECK(fresh.find("revision=999") != std::string::npos && fresh.find("global.") == std::string::npos);
        CHECK(frizti_ini_pin("revision = 7\r\nglobal.x=1\r\n").find("revision=999") != std::string::npos);
        CHECK(frizti_ini_pin("revision = 7\r\nglobal.x=1\r\n").find("revision = 7") == std::string::npos);

        const fs::path inst = tmp / "pin" / "fritchii";                    // prepare_fritchii: files on disk
        CHECK(prepare_fritchii(U8(inst)).empty());
        CHECK(fs::exists(inst / "user" / "config" / "frizti.ini"));        // created although it was missing
        CHECK(rd(inst / "user" / "config" / "frizti.ini").find("revision=999") != std::string::npos);
        CHECK(rd(inst / "user" / "config" / "qt-config.ini").find("frizti_settings_revision=999") != std::string::npos);
        wr(inst / "user" / "config" / "frizti.ini", "revision=3\nglobal.use_vsync=2\ngame.x.y=1\n");
        CHECK(prepare_fritchii(U8(inst)).empty());
        const std::string after = rd(inst / "user" / "config" / "frizti.ini");
        CHECK(after.find("global.") == std::string::npos && after.find("game.x.y=1") != std::string::npos && after.find("revision=999") != std::string::npos);
        CHECK(!fs::exists(inst / "nx2_switches.txt") && !fs::exists(inst / "user" / "nx2_switches.txt"));   // RetroHub never writes one
        // the skip list: a key named there is not written by lichii_ini_launch; the others are
        hub::gs::Values v;
        std::vector<std::string> skip = {"Renderer.use_vsync"};
        const std::string t = lichii_ini_launch("", v, true, &skip);
        CHECK(t.find("use_vsync") == std::string::npos && t.find("resolution_setup") != std::string::npos);
        CHECK(lichii_ini_launch("", v, true).find("use_vsync") != std::string::npos);   // no list (Lichii) = all keys
    }

    // ---- Ocarina of Time options (switch.oot_high_refresh / oot_refresh_cap / oot_aa)
    {
        const std::vector<std::string> none;
        const std::string base = lichii_ini_launch("", hub::gs::Values{}, false, &none);
        hub::gs::Values d;   // the defaults the settings table gives
        for (const char* k : {"switch.oot_high_refresh", "switch.oot_refresh_cap", "switch.oot_aa"}) {
            const hub::gs::Option* o = hub::gs::find_option(k);
            CHECK(o && o->def == "default");
            d[k] = "default";
        }
        CHECK(fritchii_oot_keys(d).empty() && fritchii_oot_keys(hub::gs::Values{}).empty());
        CHECK(lichii_ini_launch("", d, false, &none) == base);   // defaults: the ini is exactly what it was
        CHECK(base.find("oot_") == std::string::npos);
        hub::gs::Values v;
        v["switch.oot_high_refresh"] = "on";
        const std::string on = lichii_ini_launch("", v, false, &none);
        CHECK(on.find("oot_high_refresh\\default=false\noot_high_refresh=true\n") != std::string::npos);
        v["switch.oot_high_refresh"] = "off";
        CHECK(lichii_ini_launch(on, v, false, &none).find("oot_high_refresh=false") != std::string::npos);
        CHECK(lichii_ini_launch(on, v, false, &none).find("oot_high_refresh=true") == std::string::npos);
        const char* caps[] = {"display", "90", "120", "144", "165"};
        for (int i = 0; i < 5; ++i) {
            hub::gs::Values c;
            c["switch.oot_refresh_cap"] = caps[i];
            CHECK(lichii_ini_launch("", c, false, &none).find("oot_high_refresh_cap=" + std::to_string(i) + "\n") != std::string::npos);
        }
        const char* aas[] = {"game", "nosharp", "fsr3", "fsr4"};
        for (int i = 0; i < 4; ++i) {
            hub::gs::Values c;
            c["switch.oot_aa"] = aas[i];
            CHECK(lichii_ini_launch("", c, false, &none).find("oot_anti_aliasing=" + std::to_string(i) + "\n") != std::string::npos);
        }
        hub::gs::Values bad;
        bad["switch.oot_aa"] = "bogus";
        bad["switch.oot_refresh_cap"] = "240";
        bad["switch.oot_high_refresh"] = "maybe";
        CHECK(lichii_ini_launch("", bad, false, &none) == base);
        v["switch.oot_refresh_cap"] = "144";
        v["switch.oot_aa"] = "fsr3";
        v["switch.oot_high_refresh"] = "on";
        const std::string all = lichii_ini_launch("", v, false, &none);
        CHECK(lichii_ini_launch(all, v, false, &none) == all);                      // idempotent
        CHECK(fritchii_ini_pin(all) == fritchii_ini_pin(fritchii_ini_pin(all)));
        CHECK(fritchii_ini_pin(all).find("oot_high_refresh_cap=3") != std::string::npos);   // the pin leaves them alone
        CHECK(lichii_ini_launch("", v, false).find("oot_") == std::string::npos);      // Lichii (no skip list) never gets them
        // back on "Fritchii's setting": a key an earlier launch wrote is flipped to default=true (Fritchii then ignores the value line); a key that is not there stays absent
        {
            const std::string back = lichii_ini_launch(all, d, false, &none);
            CHECK(back.find("oot_high_refresh\\default=true\n") != std::string::npos && back.find("oot_high_refresh\\default=false") == std::string::npos);
            CHECK(back.find("oot_high_refresh_cap\\default=true\n") != std::string::npos && back.find("oot_anti_aliasing\\default=true\n") != std::string::npos);
            CHECK(back.find("oot_anti_aliasing\\default=false") == std::string::npos);
            CHECK(lichii_ini_launch(back, d, false, &none) == back);                       // idempotent
            CHECK(lichii_ini_launch(back, hub::gs::Values{}, false, &none) == back);       // an absent setting reads as default too
            hub::gs::Values part = v;
            part["switch.oot_aa"] = "default";                                             // one option back to default, the others stay chosen
            const std::string mix = lichii_ini_launch(all, part, false, &none);
            CHECK(mix.find("oot_anti_aliasing\\default=true\n") != std::string::npos);
            CHECK(mix.find("oot_high_refresh_cap\\default=false\noot_high_refresh_cap=3\n") != std::string::npos);
            CHECK(lichii_ini_launch(back, v, false, &none).find("oot_anti_aliasing\\default=false\noot_anti_aliasing=2\n") != std::string::npos);   // choosing a value again
            CHECK(lichii_ini_launch(base, d, false, &none) == base);                       // an ini that never had the keys stays byte for byte
            const std::string other = "[Renderer]\nfoo\\default=false\nfoo=1\n";
            CHECK(lichii_ini_launch(other, d, false, &none).find("oot_") == std::string::npos);
            std::vector<std::string> sk = {"Renderer.oot_anti_aliasing"};                  // a skipped key is not touched
            CHECK(lichii_ini_launch(all, d, false, &sk).find("oot_anti_aliasing\\default=false\noot_anti_aliasing=2\n") != std::string::npos);
        }
        std::vector<std::string> skip = {"Renderer.oot_anti_aliasing"};
        CHECK(lichii_ini_launch("", v, false, &skip).find("oot_anti_aliasing") == std::string::npos);
    }

    // ---- keys + firmware copied from Lichii
    {
        World w(tmp / "keys");
        wr(w.lichii / "user" / "keys" / "prod.keys", "header_key = 00112233445566778899aabbccddeeff\n");
        wr(w.lichii / "user" / "keys" / "title.keys", "0123 = 4567\n");
        wr(w.lichii / "user" / "nand" / "system" / "Contents" / "registered" / "aa.nca", "firmware-a");
        wr(w.lichii / "user" / "nand" / "system" / "Contents" / "registered" / "bb.cnmt.nca", "firmware-b");
        wr(w.lichii / "nx2_switches.txt", "EVIL=1\n");   // Lichii's own file: must NOT be carried over
        fs::create_directories(w.fritchii / "user");
        CHECK(nx_seed_setup_needed(U8(w.lichii), U8(w.fritchii)));
        bool did = false;
        CHECK(nx_seed_setup(U8(w.lichii), U8(w.fritchii), &did).empty());
        CHECK(did);
        CHECK(rd(w.fritchii / "user" / "keys" / "prod.keys") == rd(w.lichii / "user" / "keys" / "prod.keys"));
        CHECK(fs::exists(w.fritchii / "user" / "keys" / "title.keys"));
        CHECK(rd(w.fritchii / "user" / "nand" / "system" / "Contents" / "registered" / "aa.nca") == "firmware-a");
        CHECK(fs::exists(w.lichii / "user" / "keys" / "prod.keys") && fs::exists(w.lichii / "user" / "nand" / "system" / "Contents" / "registered" / "aa.nca"));   // the source is untouched
        CHECK(!fs::exists(w.fritchii / "nx2_switches.txt") && !fs::exists(w.fritchii / "user" / "nx2_switches.txt"));
        CHECK(!nx_seed_setup_needed(U8(w.lichii), U8(w.fritchii)));
        wr(w.fritchii / "user" / "keys" / "prod.keys", "header_key = ffeeddccbbaa99887766554433221100\n");   // the target's own keys are never replaced
        CHECK(nx_seed_setup(U8(w.lichii), U8(w.fritchii), &did).empty() && !did);
        CHECK(rd(w.fritchii / "user" / "keys" / "prod.keys").find("ffeedd") != std::string::npos);
        CHECK(nx_seed_setup(U8(tmp / "none"), U8(w.fritchii), &did).empty() && !did);   // no Lichii: nothing to do
    }

    // ---- shader cache seed
    {
        World w(tmp / "shader");
        const std::string big(300000, 'x');
        auto src = [&](const char* f) { return w.lichii / "user" / "cache" / "shader" / "0400486024dec000" / f; };
        auto dst = [&](const char* f) { return w.fritchii / "user" / "cache" / "shader" / "0400486024dec000" / f; };
        wr(src("vulkan.fork-v53.bin"), big + "53", 120);
        wr(src("vulkan.fork-v54.bin"), big + "54", 100);
        wr(src("vulkan.fork-v52.bin"), "v52-not-seeded", 100);
        auto plan = shader_seed_plan(U8(w.lichii), U8(w.fritchii), kTid);
        CHECK(plan.size() == 2);
        CHECK(shader_seed_plan(U8(w.lichii), U8(w.fritchii), 0x1234).empty());          // another title: nothing
        CHECK(shader_seed_plan(U8(tmp / "nope"), U8(w.fritchii), kTid).empty());          // no Lichii install
        uint64_t last_done = 0, last_total = 0;
        CHECK(shader_seed_run(plan, [&](uint64_t d, uint64_t t) { last_done = d; last_total = t; }).empty());
        CHECK(last_total == plan[0].size + plan[1].size && last_done == last_total);
        CHECK(rd(dst("vulkan.fork-v53.bin")) == big + "53" && rd(dst("vulkan.fork-v54.bin")) == big + "54");
        CHECK(!fs::exists(dst("vulkan.fork-v52.bin")));                                    // only v53 / v54
        CHECK(fs::last_write_time(dst("vulkan.fork-v53.bin")) == fs::last_write_time(src("vulkan.fork-v53.bin")));   // same time: not "older" next time
        CHECK(fs::hard_link_count(dst("vulkan.fork-v53.bin")) == 1 && fs::hard_link_count(src("vulkan.fork-v53.bin")) == 1);   // a copy, not a hardlink
        CHECK(count_files(w.fritchii / "user" / "cache") == 2);                             // no .retrohub-part left over
        CHECK(shader_seed_plan(U8(w.lichii), U8(w.fritchii), kTid).empty());                // up to date
        wr(dst("vulkan.fork-v53.bin"), "own newer cache", 0);                                // destination newer: left alone
        CHECK(shader_seed_plan(U8(w.lichii), U8(w.fritchii), kTid).empty());
        wr(dst("vulkan.fork-v53.bin"), "own old cache", 500);                                // destination older: replaced
        plan = shader_seed_plan(U8(w.lichii), U8(w.fritchii), kTid);
        CHECK(plan.size() == 1);
        CHECK(shader_seed_run(plan, nullptr).empty());
        CHECK(rd(dst("vulkan.fork-v53.bin")) == big + "53");
        CHECK(rd(src("vulkan.fork-v53.bin")) == big + "53");                                 // the source never changes
        // cancel: nothing is left behind
        World c(tmp / "shader2");
        wr(c.lichii / "user" / "cache" / "shader" / "0400486024DEC000" / "vulkan.fork-v53.bin", big, 10);   // upper-case folder name
        auto p2 = shader_seed_plan(U8(c.lichii), U8(c.fritchii), kTid);
        CHECK(p2.size() == 1);
        std::atomic<bool> cancel{true};
        CHECK(shader_seed_run(p2, nullptr, &cancel) == "cancelled");
        CHECK(count_files(c.fritchii) == 0);
        CHECK(shader_seed_run(std::vector<ShaderSeedItem>{}, nullptr).empty());
        // a copy that comes out short (the source changed or vanished while it was read) is never put in place
        World t(tmp / "shader3");
        wr(t.lichii / "user" / "cache" / "shader" / "0400486024dec000" / "vulkan.fork-v53.bin", std::string(1000, 'a'), 10);
        auto p3 = shader_seed_plan(U8(t.lichii), U8(t.fritchii), kTid);
        CHECK(p3.size() == 1);
        p3[0].size += 500;                                                              // the planned size no longer matches what was read
        const std::string t_dst = p3[0].dst;
        CHECK(shader_seed_run(p3, nullptr).find("changed while") != std::string::npos);
        CHECK(!fs::exists(fs::u8path(t_dst)) && !fs::exists(fs::u8path(t_dst + ".retrohub-part")));
        // ...and a failing replace leaves the old cache exactly as it was (no remove before the rename)
        auto p4 = shader_seed_plan(U8(t.lichii), U8(t.fritchii), kTid);
        CHECK(p4.size() == 1);
        wr(fs::u8path(t_dst), "my own older cache", 600);
        p4 = shader_seed_plan(U8(t.lichii), U8(t.fritchii), kTid);
        CHECK(p4.size() == 1);
        HANDLE h = CreateFileW(fs::u8path(t_dst).wstring().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);   // held open: the rename cannot replace it
        CHECK(h != INVALID_HANDLE_VALUE);
        const std::string e4 = shader_seed_run(p4, nullptr);
        CHECK(!e4.empty());
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        CHECK(rd(fs::u8path(t_dst)) == "my own older cache" && !fs::exists(fs::u8path(t_dst + ".retrohub-part")));
        CHECK(shader_seed_run(p4, nullptr).empty() && rd(fs::u8path(t_dst)) == std::string(1000, 'a'));   // released: the same plan now replaces it
    }

    // ---- saves
    {
        // a. Lichii holds the newest save, Fritchii an older one, the vault is empty: launching Fritchii brings it over, the replaced one is backed up
        World w(tmp / "saves_a");
        w.install_dirs();
        wr(save_dir(w.lichii) / "slot.sav", "lichii-NEW", 10);
        wr(save_dir(w.fritchii) / "slot.sav", "fritchii-old", 600);
        wr(w.backups / "old-game" / "keep.me", "pre-existing backup");
        NxSaveSyncReport r = nx_sync_saves_before_launch(w.req(Kind::Fritchii));
        CHECK(r.ok && r.ran);
        CHECK(rd(save_dir(w.fritchii) / "slot.sav") == "lichii-NEW");
        CHECK(rd(vault_save(w.battery) / "slot.sav") == "lichii-NEW");        // the vault follows
        CHECK(rd(save_dir(w.lichii) / "slot.sav") == "lichii-NEW");           // the other emulator is never changed by a launch of this one
        CHECK(r.backups >= 1 && files_with(w.backups, "fritchii-old") == 1);  // the version that was replaced
        CHECK(rd(w.backups / "old-game" / "keep.me") == "pre-existing backup");   // existing backups stay
        CHECK(r.backup_dirs.size() == (size_t)r.backups);
        // already in step: nothing copied, no new backup
        const int files_before = count_files(w.backups);
        NxSaveSyncReport r2 = nx_sync_saves_before_launch(w.req(Kind::Fritchii));
        CHECK(r2.ok && r2.copied == 0 && r2.backups == 0 && count_files(w.backups) == files_before);

        // b. the other direction: Fritchii played (session over: its save went to the vault), Lichii is launched and gets it
        wr(save_dir(w.fritchii) / "slot.sav", "fritchii-NEWER", 100);
        wr(save_dir(w.fritchii) / "slot2.sav", "second slot", 100);
        NxSaveSyncReport r3 = nx_sync_saves_before_launch(w.req(Kind::Lichii));
        CHECK(r3.ok && r3.ran);
        CHECK(rd(save_dir(w.lichii) / "slot.sav") == "fritchii-NEWER" && rd(save_dir(w.lichii) / "slot2.sav") == "second slot");
        CHECK(rd(vault_save(w.battery) / "slot.sav") == "fritchii-NEWER");
        CHECK(files_with(w.backups, "lichii-NEW") >= 1);                       // what Lichii had is kept
        // the manifests are separate per emulator
        CHECK(fs::exists(w.root / "vault" / ".sync" / "emu-switch-switch_0400486024dec000.json"));
        CHECK(fs::exists(w.root / "vault" / ".sync" / "emu-switch-switch_0400486024dec000-fritchii.json"));

        // c. both changed since they last agreed: the newest wins and the other one is backed up
        wr(save_dir(w.lichii) / "slot.sav", "lichii-edit", 30);
        wr(save_dir(w.fritchii) / "slot.sav", "fritchii-edit-LATEST", 2);
        NxSaveSyncReport r4 = nx_sync_saves_before_launch(w.req(Kind::Fritchii));
        CHECK(r4.ok);
        CHECK(rd(save_dir(w.fritchii) / "slot.sav") == "fritchii-edit-LATEST");   // the newest stays in the launching emulator
        CHECK(files_with(w.backups, "lichii-edit") >= 1);                          // ...and the older edit is preserved under save-backups
        CHECK(rd(save_dir(w.lichii) / "slot.sav") == "lichii-edit");               // (still intact in its own folder)
        // and the same with the other one newest, launching Lichii
        wr(save_dir(w.fritchii) / "slot.sav", "fritchii-edit-2", 40);
        wr(save_dir(w.lichii) / "slot.sav", "lichii-edit-LATEST", 2);
        NxSaveSyncReport r5 = nx_sync_saves_before_launch(w.req(Kind::Lichii));
        CHECK(r5.ok && rd(save_dir(w.lichii) / "slot.sav") == "lichii-edit-LATEST");
        CHECK(files_with(w.backups, "fritchii-edit-2") >= 1);

        // d. the vault holds the newest (another PC synced it): both emulators get it, each backing up its own changes
        wr(vault_save(w.battery) / "slot.sav", "vault-from-other-pc", 0);
        NxSaveSyncReport r6 = nx_sync_saves_before_launch(w.req(Kind::Fritchii));
        CHECK(r6.ok && rd(save_dir(w.fritchii) / "slot.sav") == "vault-from-other-pc");
        CHECK(rd(vault_save(w.battery) / "slot.sav") == "vault-from-other-pc");
        // nothing under save-backups was ever removed
        CHECK(rd(w.backups / "old-game" / "keep.me") == "pre-existing backup");
        CHECK(files_with(w.backups, "fritchii-old") == 1);
    }
    {
        // e. missing folders on either side
        World w(tmp / "saves_e");
        // no other emulator at all: a no-op (the Lichii-only PC behaves as before)
        fs::create_directories(w.lichii / "user");
        wr(save_dir(w.lichii) / "slot.sav", "only-lichii", 5);
        NxSaveSyncReport r = nx_sync_saves_before_launch(w.req(Kind::Lichii));
        CHECK(r.ok && !r.ran && r.copied == 0 && !fs::exists(w.backups) && !fs::exists(w.battery));
        CHECK(!fs::exists(w.root / "vault" / ".sync"));                        // no manifest of either emulator, in particular no emu-*-fritchii.json
        {   // a "user" folder in the WORKING directory must never be mistaken for a Fritchii install (an empty dir makes the path relative)
            const fs::path cwd = fs::current_path();
            const fs::path wd = w.root / "cwd";
            fs::create_directories(wd / "user" / "nand" / "user" / "save" / "0000000000000000" / kLichiiUserUuid / kTidHex);
            wr(wd / "user" / "nand" / "user" / "save" / "0000000000000000" / kLichiiUserUuid / kTidHex / "slot.sav", "cwd-save", 1);
            fs::current_path(wd);
            NxSaveSync req = w.req(Kind::Lichii);
            req.fritchii_dir.clear();
            NxSaveSyncReport rc = nx_sync_saves_before_launch(req);
            NxSaveSync req2 = w.req(Kind::Fritchii);
            req2.lichii_dir.clear();
            NxSaveSyncReport rc2 = nx_sync_saves_before_launch(req2);
            fs::current_path(cwd);
            CHECK(rc.ok && !rc.ran && rc.copied == 0 && rc.backups == 0);
            CHECK(rc2.ok && !rc2.ran && rc2.copied == 0 && rc2.backups == 0);
            CHECK(!fs::exists(w.battery) && !fs::exists(w.backups) && !fs::exists(w.root / "vault" / ".sync") && rd(wd / "user" / "nand" / "user" / "save" / "0000000000000000" / kLichiiUserUuid / kTidHex / "slot.sav") == "cwd-save");
        }
        // Fritchii installed but with no save yet, vault empty: the Lichii save comes over, no backup (nothing was replaced)
        fs::create_directories(w.fritchii / "user");
        r = nx_sync_saves_before_launch(w.req(Kind::Fritchii));
        CHECK(r.ok && r.ran && r.backups == 0);
        CHECK(rd(save_dir(w.fritchii) / "slot.sav") == "only-lichii" && rd(vault_save(w.battery) / "slot.sav") == "only-lichii");
        CHECK(!fs::exists(w.backups) || count_files(w.backups) == 0);
        // nobody has a save (and no vault folder): nothing happens, nothing fails
        World n(tmp / "saves_n");
        n.install_dirs();
        r = nx_sync_saves_before_launch(n.req(Kind::Fritchii));
        CHECK(r.ok && r.ran && r.copied == 0 && r.backups == 0 && count_files(n.fritchii / "user" / "nand" / "user") == 0);
        // only the vault has a save, neither emulator does
        World v(tmp / "saves_v");
        v.install_dirs();
        wr(vault_save(v.battery) / "slot.sav", "vault-only", 5);
        r = nx_sync_saves_before_launch(v.req(Kind::Lichii));
        CHECK(r.ok && rd(save_dir(v.lichii) / "slot.sav") == "vault-only" && r.backups == 0);
        CHECK(!fs::exists(save_dir(v.fritchii) / "slot.sav"));                 // the other emulator is not touched by this launch
        // no title id / no battery: nothing to do
        NxSaveSync bad = v.req(Kind::Fritchii);
        bad.title_id = 0;
        CHECK(!nx_sync_saves_before_launch(bad).ran);
        // a backup that cannot be written stops the step: no backup, no overwrite
        World f(tmp / "saves_f");
        f.install_dirs();
        wr(save_dir(f.lichii) / "slot.sav", "lichii-new", 5);
        wr(save_dir(f.fritchii) / "slot.sav", "fritchii-old", 500);
        wr(f.root / "blocked", "a file where the backup folder should be");
        NxSaveSync blocked = f.req(Kind::Fritchii);
        blocked.backup_root = U8(f.root / "blocked");
        r = nx_sync_saves_before_launch(blocked);
        CHECK(!r.ok);
        CHECK(rd(save_dir(f.fritchii) / "slot.sav") == "fritchii-old");        // not overwritten without a backup
        CHECK(!fs::exists(vault_save(f.battery) / "slot.sav"));                // ...and the vault was not written either (every snapshot comes before the first write)
        CHECK(!fs::exists(f.root / "vault" / ".sync" / "emu-switch-switch_0400486024dec000-fritchii.json"));
        CHECK(r.copied == 0 && r.error.size() > 0);
        // no backup root at all (LOCALAPPDATA unset and none given): the same, nothing written
        NxSaveSync noroot = f.req(Kind::Fritchii);
        noroot.backup_root.clear();
        _putenv_s("LOCALAPPDATA", "");
        r = nx_sync_saves_before_launch(noroot);
        CHECK(!r.ok && !fs::exists(vault_save(f.battery) / "slot.sav") && rd(save_dir(f.fritchii) / "slot.sav") == "fritchii-old");
    }
    {
        // the vault's own pre-launch copy survives two snapshot points in one launch: step 1 replaces the vault with the other emulator's newer save, and the
        // launching emulator then holds something newer still (the vault's older version is replaced again at the session's end). Both used to land in one folder.
        World w(tmp / "saves_snap");
        w.install_dirs();
        wr(vault_save(w.battery) / "slot.sav", "vault-ORIGINAL", 50);
        wr(save_dir(w.lichii) / "slot.sav", "lichii-newer", 30);
        wr(save_dir(w.fritchii) / "slot.sav", "fritchii-newest", 5);
        NxSaveSyncReport r = nx_sync_saves_before_launch(w.req(Kind::Fritchii));
        CHECK(r.ok);
        CHECK(files_with(w.backups, "vault-ORIGINAL") == 1);                      // the version the vault held BEFORE this launch is kept
        CHECK(files_with(w.backups, "lichii-newer") >= 1);                        // so is the other emulator's unsynced save
        CHECK(rd(save_dir(w.fritchii) / "slot.sav") == "fritchii-newest");
        bool vault_dir_named = false;
        for (const auto& d : r.backup_dirs) if (d.find("vault-before-sync") != std::string::npos) vault_dir_named = true;
        CHECK(vault_dir_named);
        for (const auto& d : r.backup_dirs) CHECK(d.find("-before-sync") != std::string::npos);
        // a second launch in the same second never reuses or touches the first backup folder
        const int before = files_with(w.backups, "vault-ORIGINAL");
        wr(vault_save(w.battery) / "slot.sav", "vault-v2", 20);
        wr(save_dir(w.fritchii) / "slot.sav", "fritchii-v2", 1);
        CHECK(nx_sync_saves_before_launch(w.req(Kind::Fritchii)).ok);
        CHECK(files_with(w.backups, "vault-ORIGINAL") == before);
    }
    {
        // the vault's _conflicts keeps its usual last 10 copies per file (it syncs to every PC and the cloud), as for every other emulator; the LOCAL snapshots are never pruned
        World w(tmp / "saves_prune");
        w.install_dirs();
        wr(vault_save(w.battery) / "slot.sav", "vault-old", 50);
        wr(save_dir(w.lichii) / "slot.sav", "lichii-wins", 5);
        const fs::path conf = w.root / "vault" / "_conflicts" / "switch" / "switch_0400486024dec000" / "battery" / "save" / kTidHex;
        for (int i = 0; i < 12; ++i) wr(conf / ("slot.sav.local.2020010" + std::string(i < 10 ? "1-00000" : "1-0001") + std::to_string(i % 10)), "older backup " + std::to_string(i));
        const int n0 = count_files(conf);
        CHECK(n0 == 12);
        CHECK(nx_sync_saves_before_launch(w.req(Kind::Fritchii)).ok);
        CHECK(count_files(conf) == 10);                              // 12 old + 1 new, pruned to the last 10
        CHECK(files_with(conf, "older backup 0") == 0 && files_with(conf, "lichii-wins") == 0);   // (the oldest went; the new copy holds the replaced vault file)
        CHECK(files_with(conf, "vault-old") == 1);
        // the local snapshots: 15 earlier ones under save-backups stay after more launches that make new snapshots
        for (int i = 0; i < 15; ++i) wr(w.backups / "switch_0400486024dec000" / ("2020010" + std::to_string(i % 10) + "-0" + std::to_string(i) + "-before-x-launch") / "vault-before-sync" / "slot.sav", "kept forever " + std::to_string(i));
        for (int k = 0; k < 3; ++k) {
            wr(save_dir(w.lichii) / "slot.sav", "lichii-round-" + std::to_string(k), 4 - k);
            wr(save_dir(w.fritchii) / "slot.sav", "fritchii-round-" + std::to_string(k), 20);
            CHECK(nx_sync_saves_before_launch(w.req(Kind::Fritchii)).ok);
        }
        for (int i = 0; i < 15; ++i) CHECK(files_with(w.backups, "kept forever " + std::to_string(i)) == 1);
    }
    {
        // Fritchii only runs offline: an online or room launch is always Lichii
        hub::GameEntry e;
        e.system = hub::System::Switch;
        e.key = "switch:0400486024dec000";
        hub::gs::Values v;
        v["switch.runner"] = "fritchii";
        CHECK(nx_emulator_for(e, v, false, 2, true) == Kind::Fritchii);
        CHECK(nx_emulator_for(e, v, true, 2, true) == Kind::Lichii);
        v["switch.runner"] = "lichii";
        CHECK(nx_emulator_for(e, v, false, 2, true) == Kind::Lichii);
        hub::GameEntry gc;
        gc.system = hub::System::GC;
        v["switch.runner"] = "fritchii";
        CHECK(nx_emulator_for(gc, v, false, 2, true) == Kind::Lichii);
    }

    // ---- update leftovers of Fritchii's own updater
    {
        const fs::path d = tmp / "left";
        fs::create_directories(d / "user");
        CHECK(update_leftovers(Kind::Fritchii, U8(d)).empty());
        fs::create_directories(d / "update-staging");
        CHECK(update_leftovers(Kind::Fritchii, U8(d)) == "update-staging");
        CHECK(update_leftovers(Kind::Lichii, U8(d)).empty());                   // Lichii is never checked
        fs::remove_all(d / "update-staging");
        wr(d / ".update-lock", "x");
        CHECK(!update_leftovers(Kind::Fritchii, U8(d)).empty());
    }

    fs::remove_all(tmp, ec);
    std::printf("nx runner test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
