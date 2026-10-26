// Console test for offline Wii U (Cemu) support: meta.xml / ZArchive (.wua) reading incl. zstd blocks, archives (.zip) of a .wua,
// updates + DLC, key help, save folders (all accounts, update title), the user's own Cemu settings.xml (backup, fullscreen,
// graphic packs, put back), per-game profiles, the GamePad-screen hotkey/combo, cover-art name matching.
// Scratch dirs only; no Cemu is started.
#include "art_match.h"
#include "emu_formats.h"
#include "emu_saves.h"
#include "emulators.h"
#include "game_settings.h"
#include "library.h"
#include "miniz.h"
#include "wiiu_meta.h"

#include <process.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <xinput.h>

using namespace hub;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

typedef std::vector<uint8_t> Bytes;
static void put_be16(Bytes& b, size_t at, uint32_t v) { b[at] = uint8_t(v >> 8); b[at + 1] = uint8_t(v); }
static void put_be32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (24 - 8 * i)); }
static void put_be64(Bytes& b, size_t at, uint64_t v) { for (int i = 0; i < 8; ++i) b[at + i] = uint8_t(v >> (56 - 8 * i)); }

static bool write_text(const fs::path& p, const std::string& t) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << t;
    return (bool)f;
}
static std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static const char* kBaseMeta =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<menu type=\"complex\" access=\"777\">\n"
    "  <title_id type=\"hexBinary\" length=\"8\">000500001019E500</title_id>\n"
    "  <product_code type=\"string\" length=\"32\">WUP-N-ALZE</product_code>\n"
    "  <longname_ja type=\"string\" length=\"512\">JA NAME</longname_ja>\n"
    "  <longname_en type=\"string\" length=\"512\">The Legend of Zelda:&#10;Twilight Princess HD</longname_en>\n"
    "  <publisher_en type=\"string\" length=\"256\">Nintendo &amp; Co</publisher_en>\n"
    "</menu>\n";
static const char* kUpdMeta = "<menu><title_id type=\"hexBinary\" length=\"8\">0005000E1019E500</title_id><longname_en>UPD</longname_en></menu>";

/* A ZArchive with <base>/meta/meta.xml (block 0 stored raw: exactly 64 KiB) and <update>/meta/meta.xml (block 1, zstd). */
static Bytes make_wua_with_meta(const std::string& base = "000500001019e500_v0") {
    const std::string upd = "0005000e1019e500_v81";
    Bytes data(65536, 0);
    std::string m0 = kBaseMeta, m1 = kUpdMeta;
    std::memcpy(data.data(), m0.data(), m0.size());
    /* the bundled zstd is decode-only: hand-built frame = magic, single-segment descriptor (1-byte content size), one last raw block */
    std::vector<uint8_t> z = {0x28, 0xB5, 0x2F, 0xFD, 0x20, (uint8_t)m1.size(), (uint8_t)(((m1.size() << 3) | 1) & 0xFF), (uint8_t)((m1.size() << 3) >> 8), 0x00};
    z.insert(z.end(), m1.begin(), m1.end());
    size_t zn = z.size();
    data.insert(data.end(), z.begin(), z.end());
    /* names */
    Bytes names;
    auto add = [&](const std::string& s) { size_t at = names.size(); names.push_back((uint8_t)s.size()); names.insert(names.end(), s.begin(), s.end()); return (uint32_t)at; };
    uint32_t n_base = add(base), n_upd = add(upd), n_meta = add("meta"), n_xml = add("meta.xml");
    /* tree: 0 root, 1 base dir, 2 update dir, 3 base/meta, 4 upd/meta, 5 base xml, 6 upd xml */
    Bytes tree(7 * 16, 0);
    auto dir = [&](int i, uint32_t name, uint32_t first, uint32_t count) { put_be32(tree, i * 16, name); put_be32(tree, i * 16 + 4, first); put_be32(tree, i * 16 + 8, count); };
    auto file = [&](int i, uint32_t name, uint64_t off, uint64_t sz) {
        put_be32(tree, i * 16, 0x80000000u | name); put_be32(tree, i * 16 + 4, (uint32_t)off); put_be32(tree, i * 16 + 8, (uint32_t)sz);
        put_be16(tree, i * 16 + 12, (uint32_t)(off >> 32)); put_be16(tree, i * 16 + 14, (uint32_t)(sz >> 32));
    };
    dir(0, 0x7FFFFFFFu & 0, 1, 2); dir(1, n_base, 3, 1); dir(2, n_upd, 4, 1); dir(3, n_meta, 5, 1); dir(4, n_meta, 6, 1);
    file(5, n_xml, 0, m0.size()); file(6, n_xml, 65536, m1.size());
    /* offset record: base 0, block0 = 65536 bytes (stored), block1 = zn bytes */
    Bytes rec(40, 0);
    put_be64(rec, 0, 0);
    put_be16(rec, 8, 65535);
    put_be16(rec, 10, (uint32_t)(zn - 1));
    Bytes f = data;
    size_t rec_off = f.size(); f.insert(f.end(), rec.begin(), rec.end());
    size_t names_off = f.size(); f.insert(f.end(), names.begin(), names.end());
    size_t tree_off = f.size(); f.insert(f.end(), tree.begin(), tree.end());
    size_t foot = f.size();
    f.resize(foot + 144, 0);
    put_be64(f, foot + 0, 0);  put_be64(f, foot + 8, data.size());
    put_be64(f, foot + 16, rec_off); put_be64(f, foot + 24, rec.size());
    put_be64(f, foot + 32, names_off); put_be64(f, foot + 40, names.size());
    put_be64(f, foot + 48, tree_off); put_be64(f, foot + 56, tree.size());
    put_be64(f, foot + 128, f.size());
    put_be32(f, foot + 136, 0x61bf3a01u);
    put_be32(f, foot + 140, 0x169f52d6u);
    return f;
}

static ReadAtFn reader_for(const Bytes& b) {
    return [&b](uint64_t off, void* buf, size_t n) {
        if (off + n > b.size()) return false;
        std::memcpy(buf, b.data() + off, n);
        return true;
    };
}

static void test_meta_and_wua() {
    WiiuMeta m;
    CHECK(parse_wiiu_meta_xml(kBaseMeta, &m));
    CHECK(m.title_id == 0x000500001019e500ull && m.longname == "The Legend of Zelda: Twilight Princess HD");   // English name, newline collapsed
    CHECK(m.product_code == "WUP-N-ALZE" && m.game_id4 == "ALZE" && m.region == 'E' && std::string(wiiu_region_name(m.region)) == "USA");
    CHECK(m.publisher == "Nintendo & Co");
    CHECK(parse_wiiu_meta_xml("<menu><longname_ja>NIHON</longname_ja></menu>", &m) && m.longname == "NIHON");   // no English: first language that has a name
    CHECK(!parse_wiiu_meta_xml("<html></html>", &m));
    CHECK(parse_app_xml_version("<app><title_version type=\"hexBinary\" length=\"2\">0051</title_version></app>") == 81);
    CHECK(parse_app_xml_version("<app/>") == -1);

    Bytes w = make_wua_with_meta();
    std::vector<WuaTitle> ts = wua_titles(reader_for(w), w.size());
    CHECK(ts.size() == 2 && ts[0].kind == WuaTitle::Base && ts[1].kind == WuaTitle::Update && ts[1].version == 81);
    std::string xml;
    CHECK(wua_read_file(reader_for(w), w.size(), "000500001019e500_v0/meta/meta.xml", &xml) && xml == kBaseMeta);          // stored block
    CHECK(wua_read_file(reader_for(w), w.size(), "0005000E1019E500_v81/META/meta.xml", &xml) && xml == kUpdMeta);          // zstd block, case-insensitive
    CHECK(!wua_read_file(reader_for(w), w.size(), "000500001019e500_v0/meta/nothing.xml", &xml));
    CHECK(wua_read_meta(reader_for(w), w.size(), &m) && m.longname == "The Legend of Zelda: Twilight Princess HD");         // the base title's meta wins
    WiiuAddons a;
    wiiu_addons_from_wua(ts, &a);
    CHECK(a.update_in_archive && a.archive_update_version == 81 && !a.dlc_in_archive);
    CHECK(wiiu_addons_text(a).find("Update v81 (inside the .wua)") != std::string::npos);
    /* a repack whose title folder is 0000000000000000_v0 (Vimm's Lair): the id and name come from meta.xml */
    Bytes z = make_wua_with_meta("0000000000000000_v0");
    CHECK(wua_read_meta(reader_for(z), z.size(), &m) && m.title_id == 0x000500001019e500ull && m.longname == "The Legend of Zelda: Twilight Princess HD");
    Bytes bad = w;
    bad[bad.size() - 1] ^= 1;
    CHECK(wua_titles(reader_for(bad), bad.size()).empty());
}

static void test_addons_keys_saves(const fs::path& dir) {
    fs::path mlc = dir / "mlc01";
    write_text(mlc / "usr" / "title" / "0005000e" / "1019e500" / "code" / "app.xml", "<app><title_version type=\"hexBinary\" length=\"2\">0051</title_version></app>");
    fs::create_directories(mlc / "usr" / "title" / "0005000c" / "1019e500");
    FsProbe fp;
    fp.dir_exists = [](const std::string& p) { std::error_code ec; return fs::is_directory(fs::u8path(p), ec); };
    fp.file_exists = [](const std::string& p) { std::error_code ec; return fs::is_regular_file(fs::u8path(p), ec); };
    fp.read_text = [](const std::string& p) { return read_text(fs::u8path(p)); };
    WiiuAddons a = wiiu_installed_addons(0x000500001019e500ull, mlc.string(), fp);
    CHECK(a.update && a.update_version == 81 && a.dlc && a.dlc_version == -1);
    CHECK(wiiu_addons_text(a) == "Update v81 (installed in Cemu), DLC (installed in Cemu)");
    CHECK(wiiu_addons_text(wiiu_installed_addons(0x000500001019e501ull, mlc.string(), fp)) == "No update or DLC found");

    /* keys.txt */
    fs::path ud = dir / "ud";
    fs::create_directories(ud);
    WiiuKeys k = wiiu_keys_check(ud.string(), (dir / "exe").string(), fp);
    CHECK(k.found_path.empty() && k.expected_path == (ud / "keys.txt").string());
    std::string msg = wiiu_keys_message(k, "wux");
    CHECK(msg.find("keys.txt") != std::string::npos && msg.find(k.expected_path) != std::string::npos && msg.find("32-digit") != std::string::npos);
    CHECK(wiiu_keys_message(k, "wua").empty());
    write_text(ud / "keys.txt", "# comment\nnot a key\n");
    k = wiiu_keys_check(ud.string(), "", fp);
    CHECK(!k.found_path.empty() && k.key_lines == 0 && wiiu_keys_message(k, "wud").find("no 32-digit key lines") != std::string::npos);
    write_text(ud / "keys.txt", "00112233445566778899aabbccddeeff # Some Game\n");
    k = wiiu_keys_check(ud.string(), "", fp);
    CHECK(k.key_lines == 1 && wiiu_keys_message(k, "wud").empty() && msg.find("00112233") == std::string::npos);

    /* save folders and accounts */
    auto rels = wiiu_save_rels(0x000500001019e500ull);
    CHECK(rels.size() == 2 && rels[0] == "usr/save/00050000/1019e500" && rels[1] == "usr/save/0005000e/1019e500");
    write_text(mlc / "usr" / "save" / "00050000" / "1019e500" / "user" / "80000001" / "a.sav", "A");
    write_text(mlc / "usr" / "save" / "00050000" / "1019e500" / "user" / "80000002" / "b.sav", "B");
    write_text(mlc / "usr" / "save" / "00050000" / "1019e500" / "user" / "common" / "c.sav", "C");
    auto acc = wiiu_save_accounts((mlc / "usr" / "save" / "00050000" / "1019e500").string());
    CHECK(acc.size() == 2 && acc[0] == "80000001" && acc[1] == "80000002");
}

static void test_session_all_accounts(const fs::path& dir) {
    fs::path inst = dir / "cemu", vault = dir / "vault";
    fs::path save = inst / "portable" / "mlc01" / "usr" / "save";
    write_text(save / "00050000" / "1019e500" / "user" / "80000001" / "a.sav", "A1");
    write_text(save / "00050000" / "1019e500" / "user" / "80000002" / "b.sav", "B1");
    write_text(save / "00050000" / "1019e500" / "user" / "common" / "c.sav", "C1");
    write_text(save / "0005000e" / "1019e500" / "user" / "80000001" / "u.sav", "U1");
    hubemu::Install ci;
    ci.dir = inst.string();
    ci.exe = (inst / "Cemu.exe").string();
    ci.managed = true;
    GameEntry e;
    e.system = System::WiiU;
    e.key = "wiiu:file-super-mario-3d-world";   // an archive entry: the real title id is read from the unpacked .wua
    fs::path vb = vault / "wiiu" / "wiiu-file-super-mario-3d-world" / "battery";
    hubemu::SaveSession s = hubemu::begin_session(e, ci, vb.string(), vault.string(), 0x000500001019e500ull);
    CHECK(s.active && !s.wiiu_unknown_title && s.rels.size() == 2);
    hubemu::end_session(s);
    CHECK(read_text(vb / "usr" / "save" / "00050000" / "1019e500" / "user" / "80000001" / "a.sav") == "A1");
    CHECK(read_text(vb / "usr" / "save" / "00050000" / "1019e500" / "user" / "80000002" / "b.sav") == "B1");
    CHECK(read_text(vb / "usr" / "save" / "00050000" / "1019e500" / "user" / "common" / "c.sav") == "C1");
    CHECK(read_text(vb / "usr" / "save" / "0005000e" / "1019e500" / "user" / "80000001" / "u.sav") == "U1");
    /* another PC: empty emulator, everything comes back */
    fs::remove_all(inst / "portable");
    s = hubemu::begin_session(e, ci, vb.string(), vault.string(), 0x000500001019e500ull);
    CHECK(read_text(save / "00050000" / "1019e500" / "user" / "80000002" / "b.sav") == "B1");
    CHECK(read_text(save / "0005000e" / "1019e500" / "user" / "80000001" / "u.sav") == "U1");
    hubemu::end_session(s);
}

static void test_own_settings(const fs::path& dir) {
    const std::string orig =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<content>\n    <fullscreen>true</fullscreen>\n    <GraphicPack>\n        <Entry filename=\"graphicPacks/a/rules.txt\">\n"
        "            <Preset><category>x</category><preset>y</preset></Preset>\n        </Entry>\n    </GraphicPack>\n    <check_update>true</check_update>\n</content>\n";
    CHECK(hubemu::cemu_fullscreen_value(orig) == "true");
    CHECK(hubemu::cemu_fullscreen_value(hubemu::cemu_fullscreen_apply(orig, false)) == "false");
    CHECK(hubemu::cemu_fullscreen_value(hubemu::cemu_fullscreen_apply("<content>\n</content>\n", true)) == "true");   // tag added when missing
    CHECK(hubemu::cemu_fullscreen_apply("not xml", true) == "not xml");

    /* pure apply / revert */
    hub::gs::Values v;
    v["fullscreen"] = "false";
    v["wiiu.graphic_packs"] = "false";
    hubemu::CemuOwnState st;
    std::string applied = hubemu::cemu_own_settings_apply(orig, v, &st);
    CHECK(hubemu::cemu_fullscreen_value(applied) == "false" && applied.find("<GraphicPack/>") != std::string::npos && applied.find("<Entry") == std::string::npos);
    CHECK(applied.find("<check_update>true</check_update>") != std::string::npos);   // nothing else touched
    CHECK(hubemu::cemu_own_settings_revert(applied, st) == orig);
    hubemu::CemuOwnState rt;
    CHECK(hubemu::cemu_own_state_parse(hubemu::cemu_own_state_text(st), &rt) && rt.gp_block == st.gp_block && rt.fs_orig == "true" && rt.fs_written == "false" && rt.gp_stripped);
    /* the user toggled fullscreen inside the session: theirs stays; packs enabled inside the session stay */
    std::string toggled = hubemu::cemu_fullscreen_apply(applied, true);
    CHECK(hubemu::cemu_fullscreen_value(hubemu::cemu_own_settings_revert(toggled, st)) == "true");
    std::string newpacks = applied;
    newpacks.replace(newpacks.find("<GraphicPack/>"), 14, "<GraphicPack><Entry filename=\"graphicPacks/z/rules.txt\"/></GraphicPack>");
    CHECK(hubemu::cemu_own_settings_revert(newpacks, st).find("graphicPacks/z/rules.txt") != std::string::npos);

    /* files: backup once, crash recovery, put back */
    fs::path ud = dir / "appdata" / "Cemu";
    write_text(ud / "settings.xml", orig);
    CHECK(hubemu::prepare_cemu_own_settings(ud.string(), v));
    CHECK(read_text(ud / "settings.xml.retrohub-backup") == orig && fs::exists(ud / "retrohub-cemu-state.txt"));
    CHECK(hubemu::cemu_fullscreen_value(read_text(ud / "settings.xml")) == "false");
    CHECK(hubemu::prepare_cemu_own_settings(ud.string(), v));   // a second launch after a crash: leftovers undone first, the backup stays the original
    CHECK(read_text(ud / "settings.xml.retrohub-backup") == orig);
    CHECK(hubemu::revert_cemu_own_settings(ud.string()));
    CHECK(read_text(ud / "settings.xml") == orig && !fs::exists(ud / "retrohub-cemu-state.txt"));
    CHECK(!hubemu::revert_cemu_own_settings(ud.string()));
    v["fullscreen"] = "true";
    v["wiiu.graphic_packs"] = "true";
    CHECK(hubemu::prepare_cemu_own_settings(ud.string(), v));
    CHECK(read_text(ud / "settings.xml") == orig);   // already consistent: unchanged
    hubemu::revert_cemu_own_settings(ud.string());
    CHECK(!hubemu::prepare_cemu_own_settings((dir / "nothing").string(), v));   // Cemu never ran: no settings.xml, nothing written
    CHECK(!fs::exists(dir / "nothing"));

    /* -f and the setting agree */
    CHECK(hubemu::cemu_args("g.wua", true).find(" -f") != std::string::npos && hubemu::cemu_args("g.wua", false).find(" -f") == std::string::npos);
    /* RetroHub's own portable download: fullscreen follows the setting too */
    fs::path inst = dir / "managed";
    write_text(inst / "portable" / "settings.xml", hubemu::cemu_default_settings_xml());
    v["fullscreen"] = "true";
    hubemu::apply_game_settings(hubemu::Kind::Cemu, inst.string(), v);
    CHECK(hubemu::cemu_fullscreen_value(read_text(inst / "portable" / "settings.xml")) == "true");
}

static void test_profiles(const fs::path& dir) {
    CHECK(wiiu_profile_file_name(0x000500001019e500ull) == "000500001019e500.ini");
    const std::string def = "# Super Mario 3D World (JPN)\n\n[Graphics]\naccurateShaderMul = false\n\n[CPU]\ncpuMode = Singlecore-Recompiler\n";
    CHECK(ini_get(def, "CPU", "cpuMode") == "Singlecore-Recompiler" && ini_get(def, "cpu", "CPUMODE") == "Singlecore-Recompiler");
    CHECK(ini_get(def, "Graphics", "accurateShaderMul") == "false" && ini_get(def, "Graphics", "nothing").empty());
    CHECK(wiiu_profile_value(def, "[CPU]\ncpuMode = Dualcore-Recompiler\n", "CPU", "cpuMode") == "Dualcore-Recompiler");   // the user's file wins
    std::string put = ini_put(def, "CPU", "cpuMode", "Dualcore-Recompiler");
    CHECK(ini_get(put, "CPU", "cpuMode") == "Dualcore-Recompiler" && ini_get(put, "Graphics", "accurateShaderMul") == "false");
    CHECK(ini_get(ini_put("", "CPU", "cpuMode", "X"), "CPU", "cpuMode") == "X");
    CHECK(ini_get(ini_put("[Graphics]\na = 1\n", "CPU", "cpuMode", "X"), "Graphics", "a") == "1");
    CHECK(std::string(wiiu_cpu_mode_ini("dual-rec")) == "Dualcore-Recompiler" && std::string(wiiu_cpu_mode_ini("game")).empty());
    CHECK(hub::gs::find_option("wiiu.cpu_mode") && hub::gs::find_option("wiiu.cpu_mode")->def == "game");

    hub::gs::Values v;
    v["wiiu.cpu_mode"] = "game";
    CHECK(hubemu::cemu_profile_apply("", v).empty());
    v["wiiu.cpu_mode"] = "single-interp";
    CHECK(ini_get(hubemu::cemu_profile_apply("", v), "CPU", "cpuMode") == "Singlecore-Interpreter");

    fs::path ud = dir / "ud";
    const uint64_t tid = 0x000500001019e500ull;
    fs::path f = ud / "gameProfiles" / "000500001019e500.ini";
    CHECK(hubemu::apply_cemu_game_profile(ud.string(), tid, v) && ini_get(read_text(f), "CPU", "cpuMode") == "Singlecore-Interpreter");   // none before: created
    CHECK(hubemu::revert_cemu_game_profile(ud.string(), tid) && !fs::exists(f) && !fs::exists(f.string() + ".retrohub-new"));
    write_text(f, "[Graphics]\naccurateShaderMul = true\n");   // the user's own profile: kept and restored
    CHECK(hubemu::apply_cemu_game_profile(ud.string(), tid, v));
    CHECK(ini_get(read_text(f), "Graphics", "accurateShaderMul") == "true" && ini_get(read_text(f), "CPU", "cpuMode") == "Singlecore-Interpreter");
    CHECK(hubemu::revert_cemu_game_profile(ud.string(), tid) && read_text(f) == "[Graphics]\naccurateShaderMul = true\n");
    v["wiiu.cpu_mode"] = "game";
    CHECK(!hubemu::apply_cemu_game_profile(ud.string(), tid, v) && read_text(f) == "[Graphics]\naccurateShaderMul = true\n");   // nothing to set: untouched
    /* summary reads Cemu's shipped default + the user's override */
    fs::path exe = dir / "exe";
    write_text(exe / "gameProfiles" / "default" / "000500001019E500.ini", "[CPU]\ncpuMode = Singlecore-Recompiler\n[Graphics]\nstreamoutBufferCacheSize = 48\n");
    std::string sum = hubemu::cemu_profile_summary(exe.string(), (dir / "ud2").string(), tid);
    CHECK(sum.find("Singlecore-Recompiler") != std::string::npos && sum.find("stream-out cache 48") != std::string::npos && sum.find("default game profile") != std::string::npos);
}

static void test_gamepad_toggle() {
    auto keys = hubemu::cemu_toggle_gamepad_keys();
    CHECK(keys.size() == 2 && keys[0] == VK_CONTROL && keys[1] == VK_TAB);   // Cemu 2.6: Ctrl+Tab = toggle pad screen
    CHECK(hubemu::cemu_toggle_combo(XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_RIGHT_SHOULDER));
    CHECK(!hubemu::cemu_toggle_combo(XINPUT_GAMEPAD_BACK) && !hubemu::cemu_toggle_combo(XINPUT_GAMEPAD_RIGHT_SHOULDER));
    CHECK(!hubemu::cemu_toggle_combo(XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START));   // the exit combo is a different one
    CHECK(!hubemu::cemu_toggle_combo(XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_Y));       // and so is the cheats / game menu one
}

static void test_archive_and_library(const fs::path& dir) {
    fs::path games = dir / "games";
    fs::create_directories(games);
    Bytes wua = make_wua_with_meta();
    {
        fs::path z = games / "Super Mario 3D World (USA) (En,Fr,Es) (Rev 2).zip";
        mz_zip_archive za;
        std::memset(&za, 0, sizeof za);
        CHECK(mz_zip_writer_init_file(&za, z.string().c_str(), 0));
        CHECK(mz_zip_writer_add_mem(&za, "Super Mario 3D World (USA) (En,Fr,Es) (Rev 2).wua", wua.data(), wua.size(), MZ_DEFAULT_COMPRESSION));
        const char* txt = "downloaded from somewhere";
        CHECK(mz_zip_writer_add_mem(&za, "Vimm's Lair.txt", txt, std::strlen(txt), MZ_DEFAULT_COMPRESSION));
        CHECK(mz_zip_writer_finalize_archive(&za));
        mz_zip_writer_end(&za);
    }
    {   // a plain .wua gets its name from meta.xml, not from the file name
        std::ofstream f(games / "x1.wua", std::ios::binary);
        f.write((const char*)wua.data(), (std::streamsize)wua.size());
    }
    library_set_data_dir((dir / "data").string());
    auto lib = scan_library({games.string()}, nullptr);
    const GameEntry* arch = nullptr;
    const GameEntry* plain = nullptr;
    for (auto& e : lib) {
        if (e.issue == "extract") arch = &e;
        else if (e.system == System::WiiU) plain = &e;
    }
    CHECK(plain && plain->title == "The Legend of Zelda: Twilight Princess HD" && plain->key == "wiiu:000500001019e500");
    CHECK(arch && arch->system == System::WiiU && arch->title == "Super Mario 3D World" && arch->key == "wiiu:0005000010145c00");
    CHECK(lib.size() == 2);   // the readme inside the archive is not a game
    if (arch) {
        CHECK(!fs::exists(dir / "data" / "rom-cache"));   // listed only
        CHECK(extracted_3ds_path(*arch).empty() && arch->unpacked == wua.size());
        std::string out, err;
        CHECK(extract_3ds_zip(*arch, [](uint64_t, uint64_t) { return true; }, &out, &err));
        CHECK(!out.empty() && fs::path(fs::u8path(out)).filename() == "disc.wua" && extracted_3ds_path(*arch) == out);
        CHECK(out.find("rom-cache") != std::string::npos && out.find("wiiu") != std::string::npos);
        CHECK(hubemu::wiiu_title_id_of_file(out) == 0x000500001019e500ull);   // the save mapping reads the real id from the unpacked file
        CHECK(hubemu::wiiu_title_id_of_file(out + ".missing") == 0);
        {   // same archive layout as the real download: the only folder is 0000000000000000_v0
            fs::path zz = dir / "zero.wua";
            Bytes zb = make_wua_with_meta("0000000000000000_v0");
            std::ofstream f(zz, std::ios::binary);
            f.write((const char*)zb.data(), (std::streamsize)zb.size());
            f.close();
            CHECK(hubemu::wiiu_title_id_of_file(zz.string()) == 0x000500001019e500ull);
        }
    }
    library_set_data_dir("");
}

static void test_art_names() {
    const char* tree =
        "{\"sha\":\"x\",\"tree\":[{\"path\":\".gitignore\"},{\"path\":\"Named_Boxarts\"},"
        "{\"path\":\"Named_Boxarts/Legend of Zelda, The - Twilight Princess HD (Europe, Australia) (En,Fr,De,Es,It) (Rev 2).png\"},"
        "{\"path\":\"Named_Boxarts/Legend of Zelda, The - Twilight Princess HD (USA) (En,Fr,Es) (Rev 2).png\"},"
        "{\"path\":\"Named_Boxarts/Super Mario 3D World (USA) (En,Fr,Es) (Rev 2).png\"},"
        "{\"path\":\"Named_Snaps/Super Mario 3D World (USA) (En,Fr,Es) (Rev 2).png\"}]}";
    auto recs = hubart_match::parse_wiiu_tree(tree);
    CHECK(recs.size() == 3);
    hubart_match::Query q;
    q.system = "wiiu";
    q.title = "The Legend of Zelda: Twilight Princess HD";
    std::string how;
    CHECK(hubart_match::match_name(recs, q, &how) == "Legend of Zelda, The - Twilight Princess HD (USA) (En,Fr,Es) (Rev 2)" && how == "title");
    q.region = "Europe";
    CHECK(hubart_match::match_name(recs, q, &how).find("(Europe, Australia)") != std::string::npos);
    q.region.clear();
    q.title = "Super Mario 3D World";
    CHECK(hubart_match::match_name(recs, q, &how) == "Super Mario 3D World (USA) (En,Fr,Es) (Rev 2)");
    q.title = "Nonexistent Game";
    CHECK(hubart_match::match_name(recs, q, &how).empty());
    CHECK(hubart_match::unshift_article("Legend of Zelda, The - X (USA)") == "The Legend of Zelda - X (USA)");
    CHECK(hubart_match::unshift_article("Mario Kart 8 (USA)") == "Mario Kart 8 (USA)");
    CHECK(hubart_match::dat_url("wiiu").find("api.github.com/repos/libretro-thumbnails/Nintendo_-_Wii_U") != std::string::npos);
    CHECK(hubart_match::thumb_url("wiiu", 0, "Super Mario 3D World (USA) (En,Fr,Es) (Rev 2)").find("/Nintendo_-_Wii_U/master/Named_Boxarts/") != std::string::npos);
    /* GameTDB fallback: front cover only, one image per game */
    std::string gn = hubart_match::gametdb_name("ALZE", 'E');
    CHECK(gn == "gametdb:ALZE01:US" && hubart_match::gametdb_name("ALZ", 'E').empty());
    auto u = hubart_match::thumb_urls("wiiu", 0, gn);
    CHECK(u.size() == 1 && u[0] == "https://art.gametdb.com/wiiu/cover/US/ALZE01.jpg");
    CHECK(hubart_match::thumb_urls("wiiu", 1, gn).empty());
    CHECK(hubart_match::thumb_urls("wiiu", 0, hubart_match::gametdb_name("ALZP", 'P'))[0].find("/EN/ALZP01.jpg") != std::string::npos);
}

int main() {
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / ("hub_wiiu_test_" + std::to_string(_getpid()));
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    test_meta_and_wua();
    test_addons_keys_saves(dir / "a");
    test_session_all_accounts(dir / "s");
    test_own_settings(dir / "o");
    test_profiles(dir / "p");
    test_gamepad_toggle();
    test_archive_and_library(dir / "l");
    test_art_names();
    fs::remove_all(dir, ec);
    if (g_fail == 0) { std::printf("hub_wiiu_test: all checks passed\n"); return 0; }
    std::printf("hub_wiiu_test: %d check(s) failed\n", g_fail);
    return 1;
}
