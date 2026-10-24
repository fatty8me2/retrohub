/* Kanto Together mGBA gamepad config writer: exact text, merge, write-once. Scratch dirs only. */
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "kanto_pad.h"
#include "winutil.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static const char* kBody =
    "keyA=0\nkeyB=1\nkeySelect=4\nkeyStart=6\nkeyRight=14\nkeyLeft=13\nkeyUp=11\nkeyDown=12\nkeyR=10\nkeyL=9\n"
    "axisRightValue=16000\naxisRightAxis=+0\naxisLeftValue=-16000\naxisLeftAxis=-0\n"
    "axisUpValue=-16000\naxisUpAxis=-1\naxisDownValue=16000\naxisDownAxis=+1\n";

static std::string sec(const std::string& name) { return "[" + name + "]\n" + kBody; }

int main() {
    namespace fs = std::filesystem;
    hubctl::Profile def = hubctl::default_profile("retro");

    /* exact text from an empty config */
    std::string all = sec("gba.input.SDLC");
    for (int i = 1; i <= 4; ++i) all += "\n" + sec("gba.input-profile.XInput Controller #" + std::to_string(i));
    all += "\n";
    CHECK(kanto::mgba_pad_config_patch("", def) == all);

    /* merge: keyboard + tilt + user's own profile stay, our section lands after existing keys; idempotent */
    std::string user = "[gba.input.QT_K]\nkeyA=88\n\n[gba.input.SDLC]\nkeyA=7\ntiltAxisX=2\n\n[ports.qt]\nmute=1\n";
    std::string m = kanto::mgba_pad_config_patch(user, def);
    CHECK(m.find("[gba.input.QT_K]\nkeyA=88\n\n[gba.input.SDLC]\ntiltAxisX=2\nkeyA=0\nkeyB=1\n") != std::string::npos);
    CHECK(m.find("keyA=7") == std::string::npos);
    CHECK(m.find("\n[ports.qt]\nmute=1\n") != std::string::npos);
    CHECK(kanto::mgba_pad_config_patch(m, def) == m);
    CHECK(kanto::has_pad_section(m) && !kanto::has_pad_section(user.substr(0, 10)));
    std::string crlf = kanto::mgba_pad_config_patch("[a]\r\nx=1\r\n", def);
    CHECK(crlf.find("\r\nkeyA=0\r\n") != std::string::npos && crlf.find("keyA=0\n[") == std::string::npos);

    /* a remap on the Controllers page carries over: GBA A = Xbox X, L = left trigger-free */
    hubctl::Profile custom = def;
    custom.buttons["a"] = "x";
    CHECK(kanto::pad_bindings(custom)[0] == std::make_pair(std::string("keyA"), std::string("2")));
    CHECK(kanto::pad_bindings(def)[1] == std::make_pair(std::string("keyB"), std::string("1")));

    /* write once */
    fs::path tmp = fs::temp_directory_path() / ("kt_pad_" + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp / "runtime" / "mgba");
    winutil::write_file((tmp / "runtime" / "mgba" / "config.ini").string(), "[gba.input.QT_K]\nkeyA=88\n");
    CHECK(kanto::prepare_pad(tmp.string(), def));
    CHECK(!kanto::prepare_pad(tmp.string(), def));
    std::string ini; winutil::read_file((tmp / "runtime" / "mgba" / "config.ini").string(), &ini);
    ini += "[gba.input.SDLC]\nkeyA=3\n";   // not appended twice; simulate a user edit below instead
    std::string edited = ini;
    size_t p = edited.find("keyA=0");
    edited.replace(p, 6, "keyA=3");
    winutil::write_file((tmp / "runtime" / "mgba" / "config.ini").string(), edited);
    CHECK(!kanto::prepare_pad(tmp.string(), def));   // user's edit survives
    winutil::read_file((tmp / "runtime" / "mgba" / "config.ini").string(), &ini);
    CHECK(ini.find("keyA=3") != std::string::npos);
    CHECK(kanto::prepare_pad(tmp.string(), custom));   // profile changed -> rewritten
    winutil::read_file((tmp / "runtime" / "mgba" / "config.ini").string(), &ini);
    CHECK(ini.find("keyA=2") != std::string::npos && ini.find("keyA=88") != std::string::npos);
    fs::remove_all(tmp);

    std::printf(fails ? "kanto_pad_test: %d FAILED\n" : "kanto_pad_test: all passed\n", fails);
    return fails ? 1 : 0;
}
