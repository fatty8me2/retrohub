/* Controller profile tests (no SDL, no windows): the built-in defaults must behave bit-for-bit like the old hardcoded
 * mappings (n64run local_input, retrorun pad_mask), plus storage, ordering and expression parsing. The emulator-config
 * equivalence (Dolphin/Azahar/Cemu) lives in hub_emu_test next to the generators. */
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "controls.h"
#include "pad_map.h"
#include "retro_pad.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

using hubctl::PadState;

/* ---- the old code, copied verbatim (SDL calls replaced by PadState reads) ---- */
namespace legacy {
using namespace n64run;
enum { A_, B_, X_, Y_, BACK_, GUIDE_, START_, LS_, RS_, LSH_, RSH_, DU_, DD_, DL_, DR_ };
static void analog(int rx, int ry, int* ox, int* oy) {
    const float kDead = 0.08f;
    float x = std::max(-1.0f, std::min(1.0f, rx / 32767.0f));
    float y = std::max(-1.0f, std::min(1.0f, ry / 32767.0f));
    float mag = std::sqrt(x * x + y * y);
    if (mag <= kDead) { *ox = *oy = 0; return; }
    float scaled = std::min(1.0f, (mag - kDead) / (1.0f - kDead));
    float k = scaled / mag;
    *ox = (int)std::lround(x * k * 80.0f);
    *oy = (int)std::lround(-y * k * 80.0f);
    *ox = std::max(-80, std::min(80, *ox));
    *oy = std::max(-80, std::min(80, *oy));
}
static uint32_t n64(const PadState& s) {
    uint32_t v = 0;
    auto B = [&](int b) { return s.b[b]; };
    auto A = [&](int a) { return s.a[a]; };
    if (B(A_)) v |= BTN_A;
    if (B(X_) || B(B_) || B(Y_)) v |= BTN_B;
    if (B(START_)) v |= BTN_START;
    if (A(4) > 16000) v |= BTN_Z;
    if (B(LSH_)) v |= BTN_L;
    if (B(RSH_) || A(5) > 16000) v |= BTN_R;
    if (B(DU_)) v |= BTN_U_DPAD;
    if (B(DD_)) v |= BTN_D_DPAD;
    if (B(DL_)) v |= BTN_L_DPAD;
    if (B(DR_)) v |= BTN_R_DPAD;
    int rx = A(2), ry = A(3);
    if (rx > 16384) v |= BTN_C_RIGHT;
    if (rx < -16384) v |= BTN_C_LEFT;
    if (ry > 16384) v |= BTN_C_DOWN;
    if (ry < -16384) v |= BTN_C_UP;
    int ox, oy;
    analog(A(0), A(1), &ox, &oy);
    return v | pack_stick(ox, oy);
}
static uint32_t retro(const PadState& s) {
    uint32_t m = 0;
    auto b = [&](int sb, int id) { if (s.b[sb]) m |= 1u << id; };
    b(A_, RETRO_DEVICE_ID_JOYPAD_B);
    b(B_, RETRO_DEVICE_ID_JOYPAD_A);
    b(X_, RETRO_DEVICE_ID_JOYPAD_Y);
    b(Y_, RETRO_DEVICE_ID_JOYPAD_X);
    b(LSH_, RETRO_DEVICE_ID_JOYPAD_L);
    b(RSH_, RETRO_DEVICE_ID_JOYPAD_R);
    b(START_, RETRO_DEVICE_ID_JOYPAD_START);
    b(BACK_, RETRO_DEVICE_ID_JOYPAD_SELECT);
    b(LS_, RETRO_DEVICE_ID_JOYPAD_L3);
    b(RS_, RETRO_DEVICE_ID_JOYPAD_R3);
    b(DU_, RETRO_DEVICE_ID_JOYPAD_UP);
    b(DD_, RETRO_DEVICE_ID_JOYPAD_DOWN);
    b(DL_, RETRO_DEVICE_ID_JOYPAD_LEFT);
    b(DR_, RETRO_DEVICE_ID_JOYPAD_RIGHT);
    int lx = s.a[0], ly = s.a[1];
    const int dz = 16000;
    if (lx < -dz) m |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (lx > dz) m |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (ly < -dz) m |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
    if (ly > dz) m |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (s.a[4] > 16000) m |= 1u << RETRO_DEVICE_ID_JOYPAD_L2;
    if (s.a[5] > 16000) m |= 1u << RETRO_DEVICE_ID_JOYPAD_R2;
    return m;
}
}  // namespace legacy

static PadState random_state(std::mt19937& rng) {
    static const int edge[] = {0, 1, -1, 2000, 2621, 2622, 15999, 16000, 16001, 16383, 16384, 16385, -15999, -16000, -16001,
                               -16384, -16385, 32767, -32768, 8000, -8000, 30000, -30000};
    PadState s;
    for (int i = 0; i < hubctl::kNumButtons; i++) s.b[i] = (rng() % 4) == 0;
    for (int i = 0; i < hubctl::kNumAxes; i++) {
        unsigned r = rng() % 3;
        int v = r == 0 ? edge[rng() % (sizeof edge / sizeof *edge)] : (int)(rng() % 65536) - 32768;
        if (i >= 4 && v < 0 && r != 0) v = -v / 2;   // triggers are 0..32767 on real pads
        s.a[i] = v;
    }
    return s;
}

static void test_defaults_equal_legacy() {
    n64run::PadMap n(hubctl::default_profile("n64"));
    retrorun::RetroPad r(hubctl::default_profile("retro"));
    std::mt19937 rng(64);
    long checked = 0;
    for (int i = 0; i < 300000; i++) {
        PadState s = random_state(rng);
        if (n.word(s) != legacy::n64(s)) { CHECK(false && "n64 default != legacy"); std::printf("  state %d\n", i); break; }
        if (r.mask(s) != legacy::retro(s)) { CHECK(false && "retro default != legacy"); std::printf("  state %d\n", i); break; }
        checked++;
    }
    /* exhaustive sweep of one axis pair across the whole range (the stick maths) */
    for (int x = -32768; x <= 32767; x += 97)
        for (int y = -32768; y <= 32767; y += 389) {
            PadState s;
            s.a[0] = x; s.a[1] = y; s.a[2] = y; s.a[3] = x;
            CHECK(n.word(s) == legacy::n64(s));
            CHECK(r.mask(s) == legacy::retro(s));
        }
    std::printf("defaults vs legacy: %ld random states + sweep\n", checked);
}

static void test_custom_profile() {
    hubctl::Store st;
    std::string err;
    CHECK(hubctl::set_mapping(st, "n64", "", "z", "rightshoulder", &err));
    CHECK(!hubctl::set_mapping(st, "n64", "", "z", "nosuchbutton", &err) && !err.empty());
    CHECK(!hubctl::set_mapping(st, "n64", "", "nosuch", "a"));
    CHECK(hubctl::set_analog(st, "n64", "", "stick", 0.2, 0.5));
    hubctl::Profile p = hubctl::resolve(st, "n64");
    CHECK(p.name == "n64-custom");
    n64run::PadMap m(p);
    PadState s;
    s.b[10] = true;   // right shoulder
    CHECK((m.word(s) & n64run::BTN_Z) && (m.word(s) & n64run::BTN_R));
    s = PadState();
    s.a[0] = 32767;   // full right, sensitivity 0.5 => 40
    CHECK(((m.word(s) >> 16) & 0xFF) == 40);
    s.a[0] = 3000;    // inside the 0.2 deadzone
    CHECK((m.word(s) >> 16) == 0);
    /* other systems and the built-in default are untouched */
    CHECK(hubctl::resolve(st, "retro").name == "default-retro");
    CHECK(hubctl::default_profile("n64").buttons["z"] == "lefttrigger@16000");
    /* per-game override beats the system default; reset goes back */
    CHECK(hubctl::set_mapping(st, "n64", "smash64", "a", "b"));
    CHECK(hubctl::resolve(st, "n64", "smash64").buttons["a"] == "b");
    CHECK(hubctl::resolve(st, "n64", "other").buttons["a"] == "a");
    CHECK(hubctl::resolve(st, "n64", "smash64").buttons["z"] == "rightshoulder");   // override started from the system profile
    hubctl::reset(st, "n64", "smash64");
    CHECK(hubctl::resolve(st, "n64", "smash64").buttons["a"] == "a");
    hubctl::reset(st, "n64");
    CHECK(hubctl::resolve(st, "n64").same_mapping(hubctl::default_profile("n64")));
}

static void test_expr() {
    std::vector<hubctl::Term> t;
    CHECK(hubctl::parse_expr("a", 16384, &t) && t.size() == 1 && !t[0].axis && t[0].idx == 0);
    CHECK(hubctl::parse_expr("x | leftx-@9000 |righttrigger", 16384, &t) && t.size() == 3);
    CHECK(t[1].axis && t[1].sign == -1 && t[1].threshold == 9000 && t[2].threshold == 16384 && t[2].sign == 0);
    CHECK(!hubctl::valid_expr("") && !hubctl::valid_expr("a|") && !hubctl::valid_expr("zz") && !hubctl::valid_expr("a@5")
          && !hubctl::valid_expr("leftx-@0") && !hubctl::valid_expr("start+"));
    CHECK(hubctl::term_name(t[1]) == "leftx-");
    /* every default expression is valid and every control of every system has one */
    for (auto& sys : hubctl::systems()) {
        hubctl::Profile p = hubctl::default_profile(sys);
        for (auto& c : hubctl::controls_for(sys)) {
            if (c.analog) CHECK(p.analogs.count(c.name));
            else { CHECK(p.buttons.count(c.name)); CHECK(hubctl::valid_expr(p.buttons[c.name])); }
        }
    }
}

static void test_ports() {
    std::vector<std::string> pads = {"guidA#1", "guidB#7", "guidA#2", "guidC"};
    CHECK((hubctl::order_pads({}, pads) == std::vector<int>{0, 1, 2, 3}));
    CHECK((hubctl::order_pads({"guidC"}, pads) == std::vector<int>{3, 0, 1, 2}));
    CHECK((hubctl::order_pads({"guidA#2", "guidB#7"}, pads) == std::vector<int>{2, 1, 0, 3}));
    CHECK((hubctl::order_pads({"guidB"}, pads) == std::vector<int>{1, 0, 2, 3}));            // guid only
    CHECK((hubctl::order_pads({"guidA", "guidA"}, pads) == std::vector<int>{0, 2, 1, 3}));   // two identical pads, listed twice
    CHECK((hubctl::order_pads({"gone", "guidC"}, pads) == std::vector<int>{3, 0, 1, 2}));    // unplugged pad ignored
}

static void test_storage() {
    namespace fs = std::filesystem;
    fs::path d = fs::temp_directory_path() / "hubcontrols_test";
    fs::remove_all(d);
    std::string ud = (d / "userdata").string(), vault = (d / "vault").string() + "/";
    hubctl::Store st;
    hubctl::set_mapping(st, "gc", "", "z", "leftshoulder");
    hubctl::set_mapping(st, "retro", "gba1", "a", "x|y");
    hubctl::set_analog(st, "3ds", "", "circle_pad", 0.25, 1.5);
    hubctl::set_port_order(st, {"030000005e040000", "guid#9"});
    st.stamp = 100;
    CHECK(hubctl::save_store(st, ud, vault));
    CHECK(fs::exists(d / "userdata" / "controls.json") && fs::exists(d / "vault" / "controls.json"));
    hubctl::Store b;
    CHECK(hubctl::load_store(ud, vault, &b));
    CHECK(b.stamp == 100 && b.port_order == st.port_order && b.system_default == st.system_default && b.game_override == st.game_override);
    CHECK(hubctl::resolve(b, "gc").buttons["z"] == "leftshoulder");
    CHECK(hubctl::resolve(b, "retro", "gba1").buttons["a"] == "x|y");
    CHECK(hubctl::resolve(b, "3ds").analogs["circle_pad"].deadzone == 0.25 && hubctl::resolve(b, "3ds").analogs["circle_pad"].sensitivity == 1.5);
    CHECK(hubctl::mapping_hash(hubctl::resolve(b, "gc")) == hubctl::mapping_hash(hubctl::resolve(st, "gc")));
    CHECK(hubctl::resolve(b, "gc").port_order == st.port_order);
    /* the newer copy wins (a second PC edited the vault copy) */
    hubctl::Store newer = b;
    hubctl::set_mapping(newer, "gc", "", "z", "rightshoulder");
    newer.stamp = 200;
    hubctl::save_store(newer, "", vault);
    hubctl::Store c;
    CHECK(hubctl::load_store(ud, vault, &c) && c.stamp == 200 && hubctl::resolve(c, "gc").buttons["z"] == "rightshoulder");
    /* nothing stored / garbage -> defaults, no crash */
    hubctl::Store e;
    CHECK(!hubctl::load_store((d / "nothing").string(), "", &e) && e.profiles.empty());
    { std::ofstream f(d / "userdata" / "controls.json", std::ios::trunc); f << "{not json"; }
    CHECK(!hubctl::load_store(ud, "", &e));
    CHECK(!hubctl::from_json("{\"profiles\":{\"x\":{\"system\":\"nope\"}}}", &e) || e.profiles.empty());
    /* resolved profile file for the runners */
    hubctl::Profile p = hubctl::resolve(c, "n64");
    p.port_order = {"g1"};
    std::string pf = (d / "n64.json").string();
    CHECK(hubctl::write_profile_file(pf, p));
    hubctl::Profile q;
    CHECK(hubctl::read_profile_file(pf, &q) && q.same_mapping(p) && q.port_order == p.port_order && q.system == "n64");
    /* a profile file with a bad expression keeps the default for that control */
    CHECK(hubctl::profile_from_json("{\"profile\":{\"system\":\"n64\",\"buttons\":{\"a\":\"bogus\",\"b\":\"y\"}}}", &q));
    CHECK(q.buttons["a"] == "a" && q.buttons["b"] == "y");
    fs::remove_all(d);
}

static void test_describe() {
    CHECK(hubctl::describe_expr("a") == "A");
    CHECK(hubctl::describe_expr("x|b|y") == "X / B / Y");
    CHECK(hubctl::describe_expr("leftx-@9000") == "Left stick left");
    CHECK(hubctl::describe_expr("righty-") == "Right stick up");
    CHECK(hubctl::describe_expr("lefty+") == "Left stick down");
    CHECK(hubctl::describe_expr("lefttrigger@16000") == "LT");
    CHECK(hubctl::describe_expr("rightshoulder|righttrigger") == "RB / RT");
    CHECK(hubctl::describe_expr("dpup") == "D-pad up");
    CHECK(hubctl::describe_expr("bogus") == "Unbound");
    CHECK(hubctl::describe_expr("") == "Unbound");
    /* every default binding of every system has a readable name */
    for (auto& sys : hubctl::systems())
        for (auto& kv : hubctl::default_profile(sys).buttons) CHECK(hubctl::describe_expr(kv.second) != "Unbound" && hubctl::describe_expr(kv.second) != "?");
}

/* Controls page flows: per-game override, reset, port order survive a store round trip. */
static void test_page_flows() {
    hubctl::Store s;
    CHECK(hubctl::set_mapping(s, "n64", "", "a", "b"));
    CHECK(hubctl::resolve(s, "n64").buttons["a"] == "b");
    CHECK(hubctl::set_mapping(s, "n64", "gameX", "a", "y"));      // per-game copy starts from the system profile
    CHECK(hubctl::resolve(s, "n64", "gameX").buttons["a"] == "y");
    CHECK(hubctl::resolve(s, "n64", "gameX").buttons["start"] == "start");
    CHECK(hubctl::resolve(s, "n64", "other").buttons["a"] == "b");  // others keep the system profile
    hubctl::reset(s, "n64", "gameX");
    CHECK(hubctl::resolve(s, "n64", "gameX").buttons["a"] == "b");
    hubctl::reset(s, "n64");
    CHECK(hubctl::resolve(s, "n64").buttons["a"] == "a");
    hubctl::set_port_order(s, {"guidB", "guidA"});
    hubctl::Store t;
    CHECK(hubctl::from_json(hubctl::to_json(s), &t));
    CHECK(t.port_order.size() == 2 && t.port_order[0] == "guidB");
    /* profile file handed to a runner carries the mapping but the synced input format is unchanged: only the stick/buttons
     * that feed the same PadState -> button bits; the mapping hash differs only when a binding changed */
    hubctl::Store u;
    CHECK(hubctl::mapping_hash(hubctl::resolve(u, "n64")) == hubctl::mapping_hash(hubctl::default_profile("n64")));
    hubctl::set_analog(u, "n64", "", "stick", 0.2, 1.5);
    CHECK(hubctl::mapping_hash(hubctl::resolve(u, "n64")) != hubctl::mapping_hash(hubctl::default_profile("n64")));
}

/* ---- keyboard pad, rumble, sensitivity: defaults reproduce the old fixed keyboard maps ---- */
static void test_keyboard() {
    using namespace n64run;
    uint8_t k[hubctl::kKeyStateSize];
    auto press = [&](const char* name) { std::memset(k, 0, sizeof k); k[hubctl::key_code(name)] = 1; };
    hubctl::Profile n = hubctl::default_profile("n64");
    hubctl::KeyMapper km(n);
    auto dn = [&](const char* c) { return km.down(km.id(c), k); };
    /* the old local_input.cpp table (SDL scancodes: X=27 C=6 Z=29 RETURN=40 A=4 S=22 I=12 J=13 K=14 L=15 T=23 F=9 G=10 H=11) */
    struct { const char* key; int sc; const char* ctl; } old[] = {
        {"x", 27, "a"}, {"c", 6, "b"}, {"z", 29, "z"}, {"return", 40, "start"}, {"a", 4, "l"}, {"s", 22, "r"}, {"i", 12, "c_up"},
        {"j", 13, "c_left"}, {"k", 14, "c_down"}, {"l", 15, "c_right"}, {"t", 23, "dpad_up"}, {"f", 9, "dpad_left"}, {"g", 10, "dpad_down"},
        {"h", 11, "dpad_right"}, {"up", 82, "stick_up"}, {"down", 81, "stick_down"}, {"left", 80, "stick_left"}, {"right", 79, "stick_right"}};
    for (auto& o : old) { CHECK(hubctl::key_code(o.key) == o.sc); press(o.key); CHECK(dn(o.ctl)); CHECK(!dn(o.ctl[0] == 'a' ? "b" : "a")); }
    /* retro: old kbd_mask (arrows, Z=B X=A A=Y S=X Q=L W=R Enter/KP_Enter=Start RShift=Select) */
    hubctl::Profile r = hubctl::default_profile("retro");
    retrorun::RetroPad rp(r);
    auto m = [&](const char* name) { press(name); return rp.key_mask(k); };
    CHECK(m("z") == 1u << RETRO_DEVICE_ID_JOYPAD_B && m("x") == 1u << RETRO_DEVICE_ID_JOYPAD_A && m("a") == 1u << RETRO_DEVICE_ID_JOYPAD_Y);
    CHECK(m("s") == 1u << RETRO_DEVICE_ID_JOYPAD_X && m("q") == 1u << RETRO_DEVICE_ID_JOYPAD_L && m("w") == 1u << RETRO_DEVICE_ID_JOYPAD_R);
    CHECK(m("return") == 1u << RETRO_DEVICE_ID_JOYPAD_START && m("kp_enter") == 1u << RETRO_DEVICE_ID_JOYPAD_START);
    CHECK(m("rshift") == 1u << RETRO_DEVICE_ID_JOYPAD_SELECT && m("up") == 1u << RETRO_DEVICE_ID_JOYPAD_UP);
    /* Defaults: hash unchanged by the new fields; remaps and rumble change it and survive JSON */
    hubctl::Store s;
    CHECK(hubctl::mapping_hash(hubctl::resolve(s, "n64")) == hubctl::mapping_hash(n));
    CHECK(hubctl::set_key(s, "n64", "", "a", "space|x"));
    CHECK(!hubctl::set_key(s, "n64", "", "a", "nokey") && !hubctl::set_key(s, "n64", "", "bogus", "x") && !hubctl::set_key(s, "3ds", "", "a", "x"));
    CHECK(hubctl::mapping_hash(hubctl::resolve(s, "n64")) != hubctl::mapping_hash(n));
    hubctl::Store t;
    CHECK(hubctl::from_json(hubctl::to_json(s), &t));
    CHECK(hubctl::resolve(t, "n64").keys["a"] == "space|x");
    hubctl::KeyMapper k2(hubctl::resolve(t, "n64"));
    press("space");
    CHECK(k2.down(k2.id("a"), k) && !km.down(km.id("a"), k));
    CHECK(hubctl::describe_keys("space|return") == "Space / Enter" && hubctl::describe_keys("zz") == "Unbound");
    CHECK(hubctl::has_keyboard("n64") && hubctl::has_keyboard("gc") && hubctl::has_keyboard("retro") && !hubctl::has_keyboard("3ds") && !hubctl::has_keyboard("wiiu"));
    for (auto& kv : hubctl::default_profile("retro").keys) { bool f = false; for (auto& c : hubctl::key_controls_for("retro")) f |= c.name == kv.first; CHECK(f); }
    CHECK(hubctl::set_key(s, "retro", "", "l2", "e"));   // controls without a default key can be bound too, and survive JSON
    hubctl::Store s2;
    CHECK(hubctl::from_json(hubctl::to_json(s), &s2) && hubctl::resolve(s2, "retro").keys["l2"] == "e");
    /* rumble: n64 only, per game, default off, hash-neutral until on */
    hubctl::Store u;
    CHECK(!hubctl::resolve(u, "n64", "g1").rumble);
    CHECK(hubctl::set_rumble(u, "n64", "g1", true) && hubctl::resolve(u, "n64", "g1").rumble && !hubctl::resolve(u, "n64").rumble);
    CHECK(!hubctl::set_rumble(u, "gc", "", true));
    CHECK(hubctl::mapping_hash(hubctl::resolve(u, "n64", "g1")) != hubctl::mapping_hash(n));
    hubctl::Store v;
    CHECK(hubctl::from_json(hubctl::to_json(u), &v) && hubctl::resolve(v, "n64", "g1").rumble);
    hubctl::Profile pf;
    CHECK(hubctl::profile_from_json(hubctl::profile_to_json(hubctl::resolve(u, "n64", "g1")), &pf) && pf.rumble);
    /* sensitivity scales the n64 stick (the only analog consumer inside RetroHub's runners) */
    hubctl::Profile hot = n;
    hot.analogs["stick"].sensitivity = 0.5;
    hubctl::PadState ps;
    ps.a[0] = 32767;
    CHECK(((PadMap(n).word(ps) >> 16) & 0xFF) != 0 || (PadMap(n).word(ps) & 0xFF) != 0);
    CHECK(PadMap(hot).word(ps) != PadMap(n).word(ps));
}

int main() {
    test_keyboard();
    test_describe();
    test_page_flows();
    test_defaults_equal_legacy();
    test_custom_profile();
    test_expr();
    test_ports();
    test_storage();
    std::printf(g_fail ? "FAILED (%d)\n" : "controls tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
