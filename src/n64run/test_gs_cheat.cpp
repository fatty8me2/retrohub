#include <cstdio>
#include <vector>

#include "gs_cheat.h"

using namespace n64run;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

int main() {
    std::vector<GsLine> v;
    CHECK(gs_parse("80000010 00AB+81000020 1234", &v) && v.size() == 2);
    std::vector<uint8_t> ram(0x100, 0);
    gs_apply(ram.data(), (uint32_t)ram.size(), v);
    CHECK(ram[0x10 ^ 3] == 0xAB);
    CHECK(gs_rd16(ram.data(), 0x20) == 0x1234);
    CHECK(ram[0x20 ^ 3] == 0x12 && ram[0x21 ^ 3] == 0x34);
    std::vector<GsLine> c;
    CHECK(gs_parse("D0000010 00AB\n80000030 0001\nD1000020 9999\n80000031 0002", &c));
    gs_apply(ram.data(), (uint32_t)ram.size(), c);
    CHECK(ram[0x30 ^ 3] == 1);      // condition true -> applied
    CHECK(ram[0x31 ^ 3] == 0);      // condition false -> skipped
    std::vector<GsLine> bad;
    CHECK(!gs_parse("8000001 0001", &bad));
    CHECK(!gs_parse("", &bad));
    std::printf(g_fail ? "FAILED\n" : "gs_cheat_test OK\n");
    return g_fail ? 1 : 0;
}
