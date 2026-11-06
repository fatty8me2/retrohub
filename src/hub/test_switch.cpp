// Console test for Nintendo Switch via Lichii (src/hub/lichii.cpp): qt-config.ini merge (defaults, per-launch, controls), profiles.dat,
// keys / firmware import and discovery, save <-> vault mapping and launch arguments. Fake dirs in temp; no network, no emulator.
#include <windows.h>
#include "controls.h"
#include "emulators.h"
#include "library.h"
#include "lichii.h"
#include "miniz.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace hubemu;

static int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

static bool write_text(const fs::path& p, const std::string& t) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(t.data(), (std::streamsize)t.size());
    return (bool)f;
}
static std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::string U8(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
static bool has(const std::string& t, const std::string& s) { return t.find(s) != std::string::npos; }
static int count_of(const std::string& t, const std::string& s) {
    int n = 0;
    for (size_t p = t.find(s); p != std::string::npos; p = t.find(s, p + 1)) ++n;
    return n;
}

static void test_ini() {
    std::string d = lichii_ini_defaults("");
    CHECK(has(d, "[UI]\n") && has(d, "firstStart\\default=false\nfirstStart=false\n"));
    CHECK(has(d, "confirmStop=2\n") && has(d, "singleWindowMode=true\n") && has(d, "[Renderer]\nfullscreen_mode\\default=false\nfullscreen_mode=0\n"));
    // merge: user's keys and sections stay, existing keys are replaced in place, missing keys land in the right section
    std::string user = "[Core]\nuse_multi_core\\default=true\nuse_multi_core=true\n\n[UI]\nfirstStart\\default=true\nfirstStart=true\nmyKey=keep\n\n[Renderer]\nfoo=1\n";
    std::string m = lichii_ini_defaults(user);
    CHECK(has(m, "use_multi_core=true") && has(m, "myKey=keep") && has(m, "foo=1"));
    CHECK(count_of(m, "firstStart=") == 1 && has(m, "firstStart=false") && !has(m, "firstStart=true"));
    CHECK(count_of(m, "firstStart\\default") == 1 && has(m, "firstStart\\default=false"));
    CHECK(m.find("check_for_updates=false") < m.find("[Renderer]"));   // into [UI], not appended after Renderer
    CHECK(m.find("fullscreen_mode=0") > m.find("[Renderer]") && count_of(m, "[Renderer]") == 1 && count_of(m, "[UI]") == 1);
    CHECK(lichii_ini_defaults(m) == m);   // idempotent

    hub::gs::Values v = hub::gs::defaults_for(hub::System::Switch);
    std::string l = lichii_ini_launch("", v, true);
    CHECK(has(l, "resolution_setup=3\n") && has(l, "frame_gen=false\n") && !has(l, "frame_gen_type"));
    CHECK(has(l, "use_vsync=2\n") && has(l, "[UI]\nfullscreen\\default=false\nfullscreen=true\n") && has(l, "[System]\ncurrent_user\\default=false\ncurrent_user=0\n"));
    v["switch.res"] = "6"; v["switch.framegen"] = "3"; v["switch.vsync"] = "mailbox";
    std::string l2 = lichii_ini_launch(l, v, false);
    CHECK(has(l2, "resolution_setup=6\n") && has(l2, "frame_gen=true\n") && has(l2, "frame_gen_type=1\n") && has(l2, "frame_gen_multiplier=3\n"));
    CHECK(has(l2, "use_vsync=1\n") && has(l2, "fullscreen=false\n") && count_of(l2, "frame_gen=") == 1 && count_of(l2, "fullscreen=") == 1);
    v["switch.vsync"] = "off"; v["switch.framegen"] = "off";
    std::string l3 = lichii_ini_launch(l2, v, true);
    CHECK(has(l3, "use_vsync=0\n") && has(l3, "frame_gen=false\n") && !has(l3, "frame_gen=true"));
    CHECK(has(l3, "frame_gen_type=1\n"));   // left over, harmless while frame_gen=false
    // asynchronous shaders: on by default, off on request, "lichii" keeps whatever the player set inside Lichii
    CHECK(has(l, "use_asynchronous_shaders\\default=false\nuse_asynchronous_shaders=true\n"));
    v["switch.async_shaders"] = "off";
    const std::string a1 = lichii_ini_launch(l3, v, true);
    CHECK(has(a1, "use_asynchronous_shaders=false\n") && count_of(a1, "use_asynchronous_shaders=") == 1);
    v["switch.async_shaders"] = "lichii";
    CHECK(lichii_ini_launch(a1, v, true).find("use_asynchronous_shaders=false\n") != std::string::npos);   // untouched
    CHECK(!has(lichii_ini_launch("", v, true), "use_asynchronous_shaders"));
    // GPU accuracy: Low by default (measured ~30 -> ~40 fps on Ocarina), High on request, "lichii" keeps Lichii's own value
    CHECK(has(l, "gpu_accuracy\\default=false\ngpu_accuracy=0\n") && !has(l, "use_reactive_flushing") && !has(l, "dma_accuracy"));
    v["switch.gpu_accuracy"] = "high";
    const std::string s2 = lichii_ini_launch(l, v, true);
    CHECK(has(s2, "gpu_accuracy=1\n") && count_of(s2, "gpu_accuracy=") == 1);
    v["switch.gpu_accuracy"] = "lichii";
    CHECK(has(lichii_ini_launch(s2, v, true), "gpu_accuracy=1\n") && !has(lichii_ini_launch("", v, true), "gpu_accuracy"));   // untouched
    // frame pacing: untouched by default; 30 / 60 / auto map to Lichii's FramePacingMode; smooth60 = steady 30 + frame generation 2x
    {
        hub::gs::Values p = hub::gs::defaults_for(hub::System::Switch);
        CHECK(!has(lichii_ini_launch("", p, true), "frame_pacing_mode"));
        p["switch.pacing"] = "30";
        const std::string p30 = lichii_ini_launch("", p, true);
        CHECK(has(p30, "frame_pacing_mode\\default=false\nframe_pacing_mode=1\n") && has(p30, "frame_gen=false\n"));
        p["switch.pacing"] = "60";
        CHECK(has(lichii_ini_launch(p30, p, true), "frame_pacing_mode=2\n") && count_of(lichii_ini_launch(p30, p, true), "frame_pacing_mode=") == 1);
        p["switch.pacing"] = "auto";
        CHECK(has(lichii_ini_launch(p30, p, true), "frame_pacing_mode=0\n"));
        p["switch.pacing"] = "smooth60"; p["switch.framegen"] = "off";
        const std::string sm = lichii_ini_launch(p30, p, true);
        // the pacing target counts presented frames (generated ones too): steady 30 real + 30 generated = Target_60
        CHECK(has(sm, "frame_pacing_mode=2\n") && has(sm, "frame_gen=true\n") && has(sm, "frame_gen_type=1\n") && has(sm, "frame_gen_multiplier=2\n"));
        p["switch.pacing"] = "lichii";
        CHECK(has(lichii_ini_launch(sm, p, true), "frame_pacing_mode=2\n") && has(lichii_ini_launch(sm, p, true), "frame_gen=false\n"));   // pacing untouched, framegen back to its own setting
    }
}

static void test_controls() {
    hubctl::Profile p = hubctl::default_profile("switch");
    std::string c = lichii_controls_ini("[Controls]\nmykey=1\n", p, "030000005e040000ea02000000007801", 1);
    const std::string g = "engine:sdl,port:1,guid:030000005e040000ea02000000007801";
    CHECK(has(c, "player_0_button_a=\"" + g + ",button:1\""));   // Switch A = SDL B
    CHECK(has(c, "player_0_button_b=\"" + g + ",button:0\"") && has(c, "player_0_button_x=\"" + g + ",button:3\"") && has(c, "player_0_button_y=\"" + g + ",button:2\""));
    CHECK(has(c, "player_0_button_l=\"" + g + ",button:9\"") && has(c, "player_0_button_r=\"" + g + ",button:10\""));
    CHECK(has(c, "player_0_button_zl=\"" + g + ",axis:4,threshold:0.5,invert:+\"") && has(c, "player_0_button_zr=\"" + g + ",axis:5,threshold:0.5,invert:+\""));
    CHECK(has(c, "player_0_button_plus=\"" + g + ",button:6\"") && has(c, "player_0_button_minus=\"" + g + ",button:4\"") && has(c, "player_0_button_home=\"" + g + ",button:5\""));
    CHECK(has(c, "player_0_button_lstick=\"" + g + ",button:7\"") && has(c, "player_0_button_rstick=\"" + g + ",button:8\""));
    CHECK(has(c, "player_0_button_dup=\"" + g + ",button:11\"") && has(c, "player_0_button_ddown=\"" + g + ",button:12\""));
    CHECK(has(c, "player_0_button_dleft=\"" + g + ",button:13\"") && has(c, "player_0_button_dright=\"" + g + ",button:14\""));
    CHECK(has(c, "player_0_lstick=\"" + g + ",axis_x:0,axis_y:1,offset_x:0,offset_y:0,invert_x:+,invert_y:+,deadzone:0.15,range:1\""));
    CHECK(has(c, "axis_x:2,axis_y:3"));
    CHECK(has(c, "player_0_connected=true") && has(c, "player_0_type=0") && has(c, "player_0_button_a\\default=false"));
    for (int i = 1; i < 8; ++i) CHECK(has(c, "player_" + std::to_string(i) + "_connected=false"));
    CHECK(has(c, "mykey=1") && count_of(c, "[Controls]") == 1);
    CHECK(lichii_controls_ini(c, p, "030000005e040000ea02000000007801", 1) == c);
    p.buttons["a"] = "leftx-";
    p.buttons.erase("minus");
    p.analogs["left_stick"].deadzone = 0.3;
    std::string c2 = lichii_controls_ini("", p, "", 0);
    CHECK(has(c2, "player_0_button_a=\"engine:sdl,port:0,guid:0,axis:0,threshold:0.5,invert:-\""));
    CHECK(has(c2, "player_0_button_minus=\"[empty]\"") && has(c2, "deadzone:0.3,"));
    hubemu::PadDesc a, b; a.guid = b.guid = "g1"; a.key = "g1#1"; b.key = "g1#2";
    std::string guid; int port = -9;
    CHECK(lichii_pad_select({b, a}, {a, b}, &guid, &port) && guid == "g1" && port == 1);
    CHECK(!lichii_pad_select({}, {a}, &guid, &port) && guid.empty() && port == 0);
}

static void test_profile(const fs::path& d) {
    fs::path inst = d / "pinst";
    const fs::path pf = inst / "user" / "nand" / "system" / "save" / "8000000000000010" / "su" / "avators" / "profiles.dat";
    std::string u = lichii_ensure_profile(U8(inst));
    CHECK(u == kLichiiUserUuid && u.size() == 32);
    std::string raw = read_text(pf);
    CHECK(raw.size() == 0x650);
    CHECK((uint8_t)raw[0x10] == 0x62 && (uint8_t)raw[0x17] == 0xC4);   // uuid[0] = low half 0x...? little-endian u64: C41E8B05A97D3F62 -> 62 3F 7D A9 ...
    CHECK(raw.compare(0x10 + 0x28, 6, "Player") == 0);
    CHECK(raw[0x10 + 0x20] == 0 && raw.find_first_not_of('\0', 0x10 + 0xC8) == std::string::npos);   // other slots empty, timestamp 0
    CHECK(lichii_ensure_profile(U8(inst)) == u && read_text(pf) == raw);   // second call changes nothing
    // an existing user list is never altered: its user 0 uuid is returned
    std::string other(0x650, '\0');
    for (int i = 0; i < 8; ++i) other[0x10 + 0 * 0xC8 + i] = (char)(0x11 * (i + 1));          // uuid[0] = 0x8877665544332211
    for (int i = 0; i < 8; ++i) other[0x10 + 8 + i] = (char)(0xA0 + i);                       // uuid[1] = 0xA7A6A5A4A3A2A1A0
    std::memcpy(&other[0x10 + 0x28], "Dad", 3);
    fs::path inst2 = d / "pinst2";
    fs::path pf2 = inst2 / "user" / "nand" / "system" / "save" / "8000000000000010" / "su" / "avators" / "profiles.dat";
    write_text(pf2, other);
    CHECK(lichii_ensure_profile(U8(inst2)) == "A7A6A5A4A3A2A1A08877665544332211");
    CHECK(read_text(pf2) == other);
    write_text(pf2, "");   // empty file counts as missing
    CHECK(lichii_ensure_profile(U8(inst2)) == kLichiiUserUuid && read_text(pf2).size() == 0x650);
    CHECK(lichii_save_rel("A7A6A5A4A3A2A1A08877665544332211", 0x0100abcd12340000ull) ==
          "nand/user/save/0000000000000000/A7A6A5A4A3A2A1A08877665544332211/0100ABCD12340000");
}

static void test_needs_keys() {
    CHECK(!lichii_needs_keys("C:\\g\\exefs\\main") && !lichii_needs_keys("C:\\g\\homebrew.nro") && !lichii_needs_keys("C:\\g\\HOMEBREW.NRO"));
    for (const char* e : {"a.nsp", "a.NSZ", "a.xci", "a.xcz"}) CHECK(lichii_needs_keys(std::string("C:\\g\\") + e));
    CHECK(!lichii_needs_keys("C:\\g\\Zelda [0400486024DEC000].nup") && !lichii_needs_keys("C:\\g\\A.NUP"));   // unencrypted package: no keys
    CHECK(lichii_args("C:\\My Games\\a b.nsp", true) == "-f -u 0 -g \"C:\\My Games\\a b.nsp\"");
    CHECK(lichii_args("C:\\g\\a.nsp", false) == "-u 0 -g \"C:\\g\\a.nsp\"");
}

static const char* kKeys = "; keys\nmaster_key_00 = 00112233\nheader_key = 8745f1\naes_kek_generation_source = aa\n";

static void test_import(const fs::path& d) {
    fs::path inst = d / "iinst", src = d / "src";
    CHECK(!lichii_setup_status(U8(inst)).prod_keys && !lichii_setup_status(U8(inst)).firmware);
    write_text(src / "bad" / "prod.keys", "master_key_00 = 00\n");
    CHECK(!lichii_import_keys(U8(inst), U8(src / "bad")).empty() && has(lichii_import_keys(U8(inst), U8(src / "bad" / "prod.keys")), "header_key"));
    CHECK(!fs::exists(inst / "user" / "keys" / "prod.keys"));
    CHECK(!lichii_import_keys(U8(inst), U8(src / "nothing")).empty() && !lichii_import_keys(U8(inst), U8(src / "bad" / "x.txt")).empty());
    write_text(src / "good" / "prod.keys", kKeys);
    write_text(src / "good" / "title.keys", "0100abcd = 1234\n");
    CHECK(lichii_import_keys(U8(inst), U8(src / "good")).empty());
    CHECK(read_text(inst / "user" / "keys" / "prod.keys") == kKeys && read_text(src / "good" / "prod.keys") == kKeys);   // copied, source stays
    CHECK(has(read_text(inst / "user" / "keys" / "title.keys"), "0100abcd"));
    NxSetup st = lichii_setup_status(U8(inst));
    CHECK(st.prod_keys && st.title_keys && !st.firmware);
    write_text(src / "single" / "my.keys", std::string(kKeys) + "extra = 1\n");   // a single .keys file is accepted as prod.keys
    CHECK(lichii_import_keys(U8(inst), U8(src / "single" / "my.keys")).empty() && has(read_text(inst / "user" / "keys" / "prod.keys"), "extra = 1"));

    // firmware: plain folder, nested; Ryujinx-style x.nca\00 folders
    write_text(src / "fw" / "sub" / "aaaa.nca", "NCA-A");
    write_text(src / "fw" / "bbbb.cnmt.nca", "NCA-B");
    write_text(src / "fw" / "readme.txt", "not firmware");
    CHECK(lichii_import_firmware(U8(inst), U8(src / "fw")).empty());
    fs::path reg = inst / "user" / "nand" / "system" / "Contents" / "registered";
    CHECK(read_text(reg / "aaaa.nca") == "NCA-A" && read_text(reg / "bbbb.cnmt.nca") == "NCA-B" && !fs::exists(reg / "readme.txt"));
    CHECK(fs::exists(src / "fw" / "sub" / "aaaa.nca") && lichii_setup_status(U8(inst)).firmware);
    write_text(src / "ryu" / "cccc.nca" / "00", "NCA-C");
    write_text(src / "ryu" / "dddd.cnmt.nca" / "00", "NCA-D");
    CHECK(lichii_import_firmware(U8(inst), U8(src / "ryu")).empty());
    CHECK(fs::is_regular_file(reg / "cccc.nca") && read_text(reg / "cccc.nca") == "NCA-C" && fs::is_regular_file(reg / "dddd.cnmt.nca"));
    CHECK(!fs::exists(reg / "aaaa.nca"));   // replaced, as Lichii's own Install Firmware does
    CHECK(!fs::exists(inst / "user" / "nand" / "system" / "Contents" / ".retrohub-fw-new"));
    // empty result is rejected and leaves the installed firmware alone
    fs::create_directories(src / "empty");
    write_text(src / "empty" / "x.txt", "x");
    CHECK(!lichii_import_firmware(U8(inst), U8(src / "empty")).empty() && fs::exists(reg / "cccc.nca"));
    CHECK(!lichii_import_firmware(U8(inst), U8(src / "missing")).empty());
    CHECK(!lichii_import_firmware(U8(inst), U8(reg)).empty() && fs::exists(reg / "cccc.nca"));   // never from itself
    // zip
    {
        std::string zp = U8(src / "fw.zip");
        mz_zip_archive za; std::memset(&za, 0, sizeof za);
        CHECK(mz_zip_writer_init_file(&za, zp.c_str(), 0));
        mz_zip_writer_add_mem(&za, "Firmware 19.0.1/eeee.nca", "NCA-E", 5, MZ_BEST_SPEED);
        mz_zip_writer_add_mem(&za, "Firmware 19.0.1/ffff.cnmt.nca", "NCA-F", 5, MZ_BEST_SPEED);
        mz_zip_writer_add_mem(&za, "Firmware 19.0.1/notes.txt", "n", 1, MZ_BEST_SPEED);
        mz_zip_writer_finalize_archive(&za); mz_zip_writer_end(&za);
        CHECK(lichii_import_firmware(U8(inst), zp).empty());
        CHECK(read_text(reg / "eeee.nca") == "NCA-E" && fs::exists(reg / "ffff.cnmt.nca") && !fs::exists(reg / "cccc.nca") && !fs::exists(reg / "notes.txt"));
        CHECK(!fs::exists(inst / "user" / "nand" / "system" / "Contents" / ".retrohub-fw-unzip"));
        std::string bad = U8(src / "evil.zip");
        std::memset(&za, 0, sizeof za);
        CHECK(mz_zip_writer_init_file(&za, bad.c_str(), 0));
        mz_zip_writer_add_mem(&za, "../escape.nca", "X", 1, MZ_BEST_SPEED);
        mz_zip_writer_finalize_archive(&za); mz_zip_writer_end(&za);
        CHECK(!lichii_import_firmware(U8(inst), bad).empty() && !fs::exists(inst / "user" / "nand" / "system" / "escape.nca") && fs::exists(reg / "eeee.nca"));
        std::string emp = U8(src / "emptyzip.zip");
        std::memset(&za, 0, sizeof za);
        CHECK(mz_zip_writer_init_file(&za, emp.c_str(), 0));
        mz_zip_writer_add_mem(&za, "readme.txt", "r", 1, MZ_BEST_SPEED);
        mz_zip_writer_finalize_archive(&za); mz_zip_writer_end(&za);
        CHECK(!lichii_import_firmware(U8(inst), emp).empty());
        CHECK(!lichii_import_firmware(U8(inst), U8(src / "good" / "prod.keys")).empty());   // not a zip
    }

    // discovery
    fs::path ad = d / "appdata";
    write_text(ad / "eden" / "keys" / "prod.keys", kKeys);
    write_text(ad / "yuzu" / "nand" / "system" / "Contents" / "registered" / "1.nca", "n");
    write_text(ad / "Ryujinx" / "system" / "prod.keys", kKeys);
    write_text(ad / "Ryujinx" / "bis" / "system" / "Contents" / "registered" / "2.nca" / "00", "n");
    write_text(ad / "citron" / "keys" / "prod.keys", "");   // empty keys do not count
    write_text(d / "portable" / "user" / "keys" / "prod.keys", kKeys);
    auto srcs = lichii_find_sources(U8(ad), {U8(d / "portable"), U8(d / "nowhere")});
    CHECK(srcs.size() == 4);
    bool eden = false, yuzu = false, ryu = false, port = false;
    for (const auto& s : srcs) {
        if (s.label == "Eden") eden = !s.keys_dir.empty() && s.firmware_dir.empty();
        if (s.label == "yuzu") yuzu = s.keys_dir.empty() && !s.firmware_dir.empty();
        if (s.label == "Ryujinx") ryu = s.keys_dir == U8(ad / "Ryujinx" / "system") && !s.firmware_dir.empty();
        if (has(s.label, "portable") || has(s.label, "Portable")) port = !s.keys_dir.empty();
    }
    CHECK(eden && yuzu && ryu && port);
    fs::path inst3 = d / "iinst3";
    for (const auto& s : srcs) if (s.label == "Ryujinx") {
        CHECK(lichii_import_keys(U8(inst3), s.keys_dir).empty() && lichii_import_firmware(U8(inst3), s.firmware_dir).empty());
        CHECK(fs::is_regular_file(inst3 / "user" / "nand" / "system" / "Contents" / "registered" / "2.nca") && fs::exists(ad / "Ryujinx" / "bis" / "system" / "Contents" / "registered" / "2.nca" / "00"));
    }
}

static void test_prepare(const fs::path& d) {
    fs::path inst = d / "pr";
    hub::gs::Values v = hub::gs::defaults_for(hub::System::Switch);
    hubctl::Profile p = hubctl::default_profile("switch");
    fs::path ini = inst / "user" / "config" / "qt-config.ini", marker = inst / "user" / "config" / "retrohub-defaults.txt";
    write_text(ini, "[UI]\nmyKey=keep\nfirstStart=true\n");
    CHECK(prepare_lichii(U8(inst), p, "guid1", 0, v, true).empty());
    std::string t = read_text(ini);
    CHECK(has(t, "myKey=keep") && has(t, "firstStart=false") && has(t, "player_0_button_a=") && has(t, "fullscreen=true") && fs::exists(marker));
    // defaults written once: a user's later edit of a default survives, per-launch values are rewritten
    std::string edited = read_text(ini);
    size_t at = edited.find("firstStart=false");
    edited.replace(at, 16, "firstStart=true");
    write_text(ini, edited);
    v["fullscreen"] = "false";
    CHECK(prepare_lichii(U8(inst), p, "guid1", 0, v, false).empty());
    t = read_text(ini);
    CHECK(has(t, "firstStart=true") && has(t, "fullscreen=false") && count_of(t, "fullscreen=") == 1);
    fs::remove(marker);
    CHECK(prepare_lichii(U8(inst), p, "guid1", 0, v, false).empty() && has(read_text(ini), "firstStart=false"));
    CHECK(fs::exists(inst / "user" / "nand" / "system" / "save" / "8000000000000010" / "su" / "avators" / "profiles.dat"));
}

static void test_saves(const fs::path& d) {
    // two PCs, one vault: the user uuid differs per PC, the vault layout must not
    const char* tid = "0100abcd12340000";
    hub::GameEntry e; e.system = hub::System::Switch; e.key = std::string("switch:") + tid;
    fs::path vault = d / "vault";
    fs::path bat = vault / "switch" / ("switch_" + std::string(tid)) / "battery";
    fs::path pcA = d / "pcA", pcB = d / "pcB";
    Install ia; ia.exe = U8(pcA / "lichii.exe"); ia.dir = U8(pcA); ia.managed = true;
    Install ib = ia; ib.exe = U8(pcB / "lichii.exe"); ib.dir = U8(pcB);
    std::string uA = lichii_ensure_profile(ia.dir);
    CHECK(uA == kLichiiUserUuid);
    // PC B has a different user
    std::string other(0x650, '\0');
    for (int i = 0; i < 16; ++i) other[0x10 + i] = (char)(i + 1);
    write_text(pcB / "user" / "nand" / "system" / "save" / "8000000000000010" / "su" / "avators" / "profiles.dat", other);
    std::string uB = lichii_ensure_profile(ib.dir);
    CHECK(uB != uA && uB.size() == 32);
    fs::path saveA = pcA / "user" / lichii_save_rel(uA, 0x0100abcd12340000ull);
    fs::path saveB = pcB / "user" / lichii_save_rel(uB, 0x0100abcd12340000ull);

    SaveSession s = begin_session(e, ia, U8(bat), "");
    CHECK(s.active && s.rels.size() == 1 && s.rels[0] == lichii_save_rel(uA, 0x0100abcd12340000ull) && s.vault_rels.size() == 1);
    write_text(saveA / "0" / "game.sav", "PC-A-SAVE");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    end_session(s);
    CHECK(s.error.empty());
    CHECK(read_text(bat / "save" / "0100ABCD12340000" / "0" / "game.sav") == "PC-A-SAVE");
    CHECK(!has(U8(bat), uA) && !fs::exists(bat / "nand"));   // the vault never holds the uuid path
    SaveSession sb = begin_session(e, ib, U8(bat), "");
    CHECK(sb.active && read_text(saveB / "0" / "game.sav") == "PC-A-SAVE");   // restored under B's own uuid
    end_session(sb);
    // account/ layout: used (and mapped under its own vault folder) only when it already exists
    fs::path acct = pcB / "user" / "nand" / "user" / "save" / "account" / uB / "0100ABCD12340000";
    write_text(acct / "x.sav", "ACCT");
    SaveSession sc = begin_session(e, ib, U8(bat), "");
    CHECK(sc.rels.size() == 2 && sc.vault_rels.size() == 2 && sc.vault_rels[1] == "save-account/0100ABCD12340000");
    end_session(sc);
    CHECK(read_text(bat / "save-account" / "0100ABCD12340000" / "x.sav") == "ACCT");
    // no title id (homebrew / loose names): no save sync
    hub::GameEntry hb; hb.system = hub::System::Switch; hb.key = "switch:file-homebrew";
    CHECK(!begin_session(hb, ia, U8(vault / "switch" / "hb" / "battery"), "").active);
}

/* hub_switch_test --download <dir>: downloads the pinned Lichii build into <dir> (network, ~110 MB; NEVER started), checks the exe, the
 * portable user\ folder, that the debug symbols / VC++ installer were dropped, then writes RetroHub's config into it. */
static int test_download(const std::string& root) {
    std::string err;
    int last = -1;
    bool ok = hubemu::install_blocking(hubemu::Kind::Lichii, root, &err, [&](int phase, uint64_t got, uint64_t total) {
        if (phase != last) { std::printf("  Lichii: phase %d (%llu / %llu bytes)\n", phase, (unsigned long long)got, (unsigned long long)total); last = phase; }
    });
    CHECK(ok);
    if (!ok) { std::printf("install failed: %s\n", err.c_str()); return 1; }
    hubemu::Install in = hubemu::find(hubemu::Kind::Lichii, "", "", root);
    std::printf("exe: %s (%s)\n", in.exe.c_str(), in.how.c_str());
    CHECK(!in.exe.empty() && fs::exists(fs::u8path(in.exe)));
    CHECK(fs::is_directory(fs::u8path(in.dir) / "user"));
    CHECK(!fs::exists(fs::u8path(in.dir) / "lichii.pdb") && !fs::exists(fs::u8path(in.dir) / "eden-cli.pdb") && !fs::exists(fs::u8path(in.dir) / "vc_redist.x64.exe"));
    CHECK(fs::exists(fs::u8path(in.dir) / "Qt6Core.dll") && fs::exists(fs::u8path(in.dir) / "platforms" / "qwindows.dll"));
    hub::gs::Values v = hub::gs::defaults_for(hub::System::Switch);
    v["switch.framegen"] = "2";
    CHECK(hubemu::prepare_lichii(in.dir, hubctl::default_profile("switch"), "030000005e0400008e02000000007200", 0, v, true).empty());
    hubemu::NxSetup st = hubemu::lichii_setup_status(in.dir);
    CHECK(!st.prod_keys && !st.firmware);
    std::printf("args: %s\n", hubemu::lichii_args("C:\\Games\\x.nsp", true).c_str());
    std::string again;
    CHECK(hubemu::install_blocking(hubemu::Kind::Lichii, root, &again, nullptr));   // already installed: instant
    return g_fail;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--download") {
        setvbuf(stdout, nullptr, _IONBF, 0);
        int bad = test_download(argv[2]);
        std::printf(bad ? "hub_switch_test --download: %d FAILED\n" : "hub_switch_test --download: all passed\n", bad);
        return bad ? 1 : 0;
    }
    fs::path d = fs::temp_directory_path() / ("hub_switch_test_" + std::to_string((unsigned long long)GetCurrentProcessId()));
    fs::create_directories(d);
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_ini();
    test_controls();
    test_needs_keys();
    test_profile(d);
    test_import(d);
    test_prepare(d);
    test_saves(d);
    std::error_code ec;
    fs::remove_all(d, ec);
    std::printf(g_fail ? "hub_switch_test: %d FAILED\n" : "hub_switch_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
