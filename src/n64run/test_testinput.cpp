#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "buttons.h"
#include "test_input.h"

using namespace n64run;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static std::vector<uint32_t> run(const std::string& spec, int slot, int n, int idx = 0) {
    TestInput t; std::string e;
    if (!t.configure(spec, slot, &e)) { std::printf("configure: %s\n", e.c_str()); g_fail++; return {}; }
    std::vector<uint32_t> v;
    for (int i = 0; i < n; i++) { t.update(); v.push_back(t.read(idx)); }
    return v;
}

int main() {
    auto a = run("random:7", 0, 6000), b = run("random:7", 0, 6000), c = run("random:7", 1, 6000);
    CHECK(a == b);          // deterministic
    CHECK(a != c);          // slot changes the stream
    // spans: runs of identical words between non-Start-forced samples are >= 5 mostly; check variety + Start cadence
    std::set<uint32_t> distinct(a.begin(), a.end());
    CHECK(distinct.size() > 50);
    int starts = 0; bool prev = false;
    uint32_t seen = 0;
    for (uint32_t w : a) { bool s = w & BTN_START; if (s && !prev) starts++; prev = s; seen |= w & 0xFFFF; }
    CHECK(starts >= 20);                 // ~6000/240 forced presses
    const uint32_t need = BTN_A | BTN_B | BTN_Z | BTN_L | BTN_R | BTN_C_UP | BTN_C_DOWN | BTN_C_LEFT | BTN_C_RIGHT | BTN_START;
    CHECK((seen & need) == need);
    bool stick = false; for (uint32_t w : a) if (w >> 16) stick = true;
    CHECK(stick);
    // stick within +-80
    for (uint32_t w : a) { int x = (int8_t)((w >> 16) & 0xFF), y = (int8_t)(w >> 24); CHECK(x >= -80 && x <= 80 && y >= -80 && y <= 80); }
    // idle-sample stability: repeated read in same sample is stable
    { TestInput t; std::string e; t.configure("random:1", 0, &e); t.update(); CHECK(t.read(0) == t.read(0)); }
    // bad specs
    { TestInput t; std::string e; CHECK(!t.configure("random:x", 0, &e)); CHECK(!t.configure("bogus", 0, &e)); CHECK(!t.configure("file:/nonexistent/zz", 0, &e)); CHECK(!t.active()); }
    // file replay
    const char* path = "n64run_testinput_tmp.txt";
    { std::ofstream f(path); f << "# c\n0,0\n3,A+START,10,-20\n5,0x0020\n7,none\n"; }
    auto r = run(std::string("file:") + path, 0, 9);
    CHECK(r.size() == 9);
    CHECK(r[2] == 0);
    CHECK(r[3] == (BTN_A | BTN_START | pack_stick(10, -20)));
    CHECK(r[4] == r[3]);
    CHECK(r[5] == BTN_Z);
    CHECK(r[7] == 0);
    std::remove(path);
    std::printf(g_fail ? "FAILED (%d)\n" : "PASS n64run_testinput_test\n", g_fail);
    return g_fail ? 1 : 0;
}
