// Console test for tile text helpers (tiles_logic.h): file-size wording and cover-less tile initials. Pure strings.
#include "tiles_logic.h"
#include <cstdio>
#include <string>
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
#define CHECK_EQ(a, b) do { std::string _a = (a), _b = (b); if (_a != _b) { std::printf("FAIL %s:%d: %s -> '%s' expected '%s'\n", __FILE__, __LINE__, #a, _a.c_str(), _b.c_str()); ++g_fail; } } while (0)
using namespace hubtiles;

int main() {
    {   // size_text
        const uint64_t KB = 1024, MB = 1024 * KB, GB = 1024 * MB, TB = 1024 * GB;
        CHECK_EQ(size_text(0), "0 bytes");
        CHECK_EQ(size_text(1), "1 byte");
        CHECK_EQ(size_text(2), "2 bytes");
        CHECK_EQ(size_text(1023), "1023 bytes");
        CHECK_EQ(size_text(1024), "1 KB");
        CHECK_EQ(size_text(1536), "1.5 KB");
        CHECK_EQ(size_text(310 * KB), "310 KB");
        CHECK_EQ(size_text(MB - 1), "1 MB");          // rounds up to the unit boundary and then reads in the next unit
        CHECK_EQ(size_text(MB), "1 MB");
        CHECK_EQ(size_text(MB + MB / 2), "1.5 MB");
        CHECK_EQ(size_text(2516582), "2.4 MB");
        CHECK_EQ(size_text(99 * MB + MB / 2), "99.5 MB");
        CHECK_EQ(size_text(100 * MB), "100 MB");
        CHECK_EQ(size_text(812 * MB), "812 MB");
        CHECK_EQ(size_text(1023 * MB), "1023 MB");
        CHECK_EQ(size_text(GB), "1 GB");
        CHECK_EQ(size_text(4 * GB), "4 GB");
        CHECK_EQ(size_text(5 * GB + GB / 2), "5.5 GB");
        CHECK_EQ(size_text(uint64_t(23.3 * GB)), "23.3 GB");
        CHECK_EQ(size_text(150 * GB), "150 GB");
        CHECK_EQ(size_text(TB), "1 TB");
        CHECK_EQ(size_text(uint64_t(1.2 * TB)), "1.2 TB");
        CHECK_EQ(size_text(~0ull), "16777216 TB");   // fits the buffer, no overflow
        // never ".0", never a unit that shows 1024 or more
        for (uint64_t v : {1024ull, 1025ull, 1048575ull, 1048576ull, GB - 1, GB + 1, TB - 1, TB + 1}) {
            std::string t = size_text(v);
            CHECK(t.find(".0") == std::string::npos);
            CHECK(t.compare(0, 4, "1024") != 0);
        }
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
        // non-ASCII (UTF-8) first letters count. Latin-1 letters are upper-cased. Glyphs the tile font lacks (it draws U+0020..U+00FF) read as "?"
        CHECK_EQ(initials("\xC3\x9C" "ber Game"), "\xC3\x9C" "G");     // U-umlaut
        CHECK_EQ(initials("\xC3\xBC" "ber game"), "\xC3\x9C" "G");     // lower-case u-umlaut is upper-cased
        CHECK_EQ(initials("\xC3\x89" "clair"), "\xC3\x89");
        CHECK_EQ(initials("\xC3\xA9" "clair"), "\xC3\x89");
        CHECK_EQ(initials("\xC3\x97 Game"), "G");                     // multiplication sign is not a letter
        CHECK_EQ(initials("\xC3\xB7 Game"), "G");
        CHECK_EQ(initials("\xC3\x9F" "tein"), "\xC3\x9F");             // sharp s has no simple upper case: left as it is
        CHECK_EQ(initials("\xE3\x82\xBC\xE3\x83\xAB\xE3\x83\x80 Zelda"), "?Z");   // Japanese (no glyph) then Latin
        CHECK_EQ(initials("\xE3\x82\xBC\xE3\x83\xAB\xE3\x83\x80"), "?");
        CHECK_EQ(initials("\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0 \xD0\x9C"), "??");   // Cyrillic: no glyph in the tile font
        CHECK_EQ(initials("\xF0\x9F\x8E\xAE"), "?");                  // emoji only: first code point fallback, drawn as "?"
        CHECK_EQ(initials("\xF0\x9F\x8E\xAE Game"), "G");
        CHECK_EQ(initials("Caf\xC3\xA9 Noir"), "CN");
        CHECK_EQ(initials("\xC3\xA9\xC3\xA9\xC3\xA9 a b c"), "\xC3\x89" "AB");
        // invalid UTF-8: no crash, no over-read, never a broken sequence in the result
        {
            const char* bad[] = {"\xC3", "\xE3\x82", "\xF0\x9F\x8E", "\x80\x80", "\xFF\xFE", "\xC0\xAF", "\xED\xA0\x80", "A\xE3", "\xE3\x82 B", "\xF4\x90\x80\x80"};
            for (const char* b2 : bad) {
                std::string r = initials(b2);
                for (size_t k = 0; k < r.size();) { size_t j = k; uint32_t c = utf8_next(r, j); CHECK(c != 0xFFFD); k = j; }
                CHECK(r.size() <= 6);
            }
            CHECK_EQ(initials("A\xE3"), "A");
            CHECK_EQ(initials("\xE3\x82 B"), "B");
        }
        // a title with a real character is never blank
        for (const char* ttl : {"\xE2\x98\x85", "\xE4\xB8\xAD", "\xCE\xB1"}) CHECK(!initials(ttl).empty());
        // bytes >= 0x80 are safe (no crash on any single byte)
        for (int c = 0; c < 256; ++c) { std::string s(1, (char)c); std::string r = initials(s); (void)r; }
        // never more than 3 characters
        std::string long_name;
        for (int i = 0; i < 50; ++i) long_name += "word ";
        CHECK_EQ(initials(long_name), "WWW");
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hub_tiles_logic_test: all passed\n");
    return 0;
}
