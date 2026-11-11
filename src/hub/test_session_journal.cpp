// Emulator process / save-session lane: XML entity decoding of Cemu's mlc_path, environment save/restore around a spawn,
// suspended-spawn helper, and the pending-session journal. Temp folders only; no emulator is ever started.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "emu_saves.h"

namespace fs = std::filesystem;

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

static void test_mlc_entities() {
    using hub::cemu_settings_mlc_path;
    CHECK(cemu_settings_mlc_path("<content><mlc_path>D:\\Wii U &amp; Games\\mlc</mlc_path></content>") == "D:\\Wii U & Games\\mlc");
    CHECK(cemu_settings_mlc_path("<mlc_path>  C:\\a&lt;b&gt;&quot;c&quot;&apos;d&apos;\r\n</mlc_path>") == "C:\\a<b>\"c\"'d'");
    CHECK(cemu_settings_mlc_path("<mlc_path>C:\\x&amp;lt;y</mlc_path>") == "C:\\x&lt;y");   // one pass only: no double decoding
    CHECK(cemu_settings_mlc_path("<mlc_path>C:\\a &bogus; b &amp</mlc_path>") == "C:\\a &bogus; b &amp");   // unknown entities and a lone & stay as written
    CHECK(cemu_settings_mlc_path("<mlc_path>D:\\plain\\mlc</mlc_path>") == "D:\\plain\\mlc");
    CHECK(cemu_settings_mlc_path("<mlc_path></mlc_path>").empty() && cemu_settings_mlc_path("<mlc_path/>").empty());
    CHECK(hub::xml_decode_entities("a&amp;&amp;b") == "a&&b");
}

int main() {
    test_mlc_entities();
    std::printf("session journal / emu process tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
