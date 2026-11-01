// Console test: ROM hack identity (hack_logic.h). The hacks lane (L7) adds its library / base-finder / hack.json cases here.
#include "hack_logic.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubui::hacks;

int main() {
    CHECK(make_key("gba:bpre-1a2b3c4d", 0x9f8e7d6cu) == "gba:bpre-1a2b3c4d.hack-9f8e7d6c");
    CHECK(make_key("snes:x", 0) == "snes:x.hack-00000000");
    std::string base;
    uint32_t crc = 0;
    CHECK(parse_key("gba:bpre-1a2b3c4d.hack-9f8e7d6c", &base, &crc) && base == "gba:bpre-1a2b3c4d" && crc == 0x9f8e7d6cu);
    CHECK(is_hack_key("n64:0123abcd.hack-00ff00ff"));
    CHECK(!is_hack_key("gba:bpre-1a2b3c4d") && !is_hack_key(".hack-12345678") && !is_hack_key("gba:x.hack-1234567") &&
          !is_hack_key("gba:x.hack-1234567g") && !is_hack_key("gba:x.hack-123456789"));
    CHECK(parse_key(make_key("ps1:file-a.hack-b", 0x12345678u), &base, &crc) && base == "ps1:file-a.hack-b" && crc == 0x12345678u);   // last suffix wins
    if (!fails) std::printf("hub_hack_test: all passed\n");
    return fails ? 1 : 0;
}
