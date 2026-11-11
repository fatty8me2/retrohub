// Console test for tile text helpers (tiles_logic.h): file-size wording and cover-less tile initials. Pure strings.
#include "tiles_logic.h"
#include <cstdio>
#include <string>
static int g_fail = 0;
#define CHECK_EQ(a, b) do { std::string _a = (a), _b = (b); if (_a != _b) { std::printf("FAIL %s:%d: %s -> '%s' expected '%s'\n", __FILE__, __LINE__, #a, _a.c_str(), _b.c_str()); ++g_fail; } } while (0)
using namespace hubtiles;

int main() {
    {   // size_text
        const uint64_t KB = 1024, MB = 1024 * KB, GB = 1024 * MB, TB = 1024 * GB;
        CHECK_EQ(size_text(0), "0 B");
        CHECK_EQ(size_text(1), "1 B");
        CHECK_EQ(size_text(1023), "1023 B");
        CHECK_EQ(size_text(1024), "1.0 KB");
        CHECK_EQ(size_text(1536), "1.5 KB");
        CHECK_EQ(size_text(MB - 1), "1024.0 KB");   // rounds up to the unit boundary (existing wording)
        CHECK_EQ(size_text(MB), "1.0 MB");
        CHECK_EQ(size_text(MB + MB / 2), "1.5 MB");
        CHECK_EQ(size_text(512 * MB), "512.0 MB");
        CHECK_EQ(size_text(GB), "1024.0 MB");        // there is no GB unit: big files read in MB
        CHECK_EQ(size_text(4 * GB), "4096.0 MB");
        CHECK_EQ(size_text(5 * GB + GB / 2), "5632.0 MB");
        CHECK_EQ(size_text(TB), "1048576.0 MB");
        CHECK_EQ(size_text(~0ull), "17592186044416.0 MB");   // fits the buffer, no overflow
    }
    {   // initials
        CHECK_EQ(initials(""), "");
        CHECK_EQ(initials("Zelda"), "Z");
        CHECK_EQ(initials("mario"), "M");
        CHECK_EQ(initials("Super Mario 64"), "SM6");
        CHECK_EQ(initials("Super Smash Bros Melee"), "SSB");          // at most 3
        CHECK_EQ(initials("The Legend of Zelda"), "TLO");             // articles are not skipped
        CHECK_EQ(initials("A B C D"), "ABC");
        CHECK_EQ(initials("F-Zero X"), "FZX");
        CHECK_EQ(initials("Pac-Man"), "PM");
        CHECK_EQ(initials("Dr. Mario"), "DM");
        CHECK_EQ(initials("Zelda: Majora's Mask"), "ZMM");           // apostrophe is not a separator
        CHECK_EQ(initials("007: Nightfire"), "0N");
        CHECK_EQ(initials("1080 Snowboarding"), "1S");
        CHECK_EQ(initials("Mario_Kart"), "M");                        // underscore does not split
        CHECK_EQ(initials("  padded   name  "), "PN");
        CHECK_EQ(initials("...Hi"), "H");
        CHECK_EQ(initials("-"), "");
        CHECK_EQ(initials("   "), "");
        CHECK_EQ(initials("(Disc 1)"), "1");                          // a bracket starts no word
        CHECK_EQ(initials("Game (Disc 1)"), "G1");
        CHECK_EQ(initials("!!! ???"), "");
        // non-ASCII (UTF-8) first letters are skipped by the ASCII-only test: that word contributes nothing, others still do
        CHECK_EQ(initials("\xC3\x9C" "ber Game"), "G");               // U-umlaut
        CHECK_EQ(initials("\xC3\x89" "clair"), "");
        CHECK_EQ(initials("\xE3\x82\xBC\xE3\x83\xAB\xE3\x83\x80 Zelda"), "Z");   // Japanese then Latin
        CHECK_EQ(initials("\xE3\x82\xBC\xE3\x83\xAB\xE3\x83\x80"), "");
        CHECK_EQ(initials("Caf\xC3\xA9 Noir"), "CN");
        // bytes >= 0x80 are safe for the ctype calls (no crash on any single byte)
        for (int c = 0; c < 256; ++c) { std::string s(1, (char)c); std::string r = initials(s); (void)r; }
        // never more than 3 characters, always upper-case ASCII alnum
        std::string long_name;
        for (int i = 0; i < 50; ++i) long_name += "word ";
        CHECK_EQ(initials(long_name), "WWW");
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hub_tiles_logic_test: all passed\n");
    return 0;
}
