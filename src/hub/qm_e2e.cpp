/* Headless end-to-end test of the in-game quick menu (View+Y "Game menu").
 *
 *   HUB_SCREENSHOT=<scratch>.bmp HUB_QM_E2E=1 RetroHub.exe      (ctest: hub_qm_e2e; exit code 0 = every scenario passed)
 *
 * Runs the real hub loop on SDL's dummy video driver (no window anywhere) with a stand-in running game (built-in runner, offline;
 * commands are recorded instead of written). Input comes ONLY from an SDL virtual game controller this file attaches: every real
 * joystick backend is switched off before SDL_Init and the pad reader is restricted to the virtual device, so the user's own
 * controllers are never read. Each scenario drives buttons in real time through the same path a real pad takes (SDL state + button
 * events -> cheats_pad_watch (View+Y) -> qm_pad -> MenuInput / MenuNav -> the drawn rows), and checks the menu state; a per-frame
 * watchdog fails any close that the scenario did not ask for. Results: stdout and <HUB_SCREENSHOT stem>-qm-e2e.log. */
#include "app.h"
#include "qm_pad.h"

#include <SDL.h>

#include <cstdio>
#include <fstream>
#include <functional>

namespace {

using B = SDL_GameControllerButton;
constexpr uint32_t bit(B b) { return 1u << (int)b; }
const uint32_t VIEW = bit(SDL_CONTROLLER_BUTTON_BACK), Y = bit(SDL_CONTROLLER_BUTTON_Y), A_ = bit(SDL_CONTROLLER_BUTTON_A),
               B_ = bit(SDL_CONTROLLER_BUTTON_B), UP = bit(SDL_CONTROLLER_BUTTON_DPAD_UP), DOWN = bit(SDL_CONTROLLER_BUTTON_DPAD_DOWN),
               LEFT = bit(SDL_CONTROLLER_BUTTON_DPAD_LEFT), RIGHT = bit(SDL_CONTROLLER_BUTTON_DPAD_RIGHT), LB = bit(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);

struct Step {
    std::string name;
    uint32_t hold = 0;          // buttons held during this step
    int ms = 0;                 // how long (real time)
    bool tap = false;           // press+release `hold` inside ONE frame (shorter than a frame), then wait ms
    bool allow_close = false;   // the menu may close during this step
    bool keep = false;          // a pure check: leave the buttons as the previous step set them
    std::function<bool(const QmE2EState&, std::string*)> check;   // at the end of the step
};

struct Scenario { std::string name; std::vector<Step> steps; };

SDL_Joystick* g_joy = nullptr;
std::vector<Scenario> g_sc;
size_t g_si = 0, g_step = 0;
Uint32 g_step_t0 = 0;
bool g_step_started = false, g_was_open = false, g_sc_failed = false, g_done = false, g_started = false;
int g_pass = 0, g_fail = 0, g_checks = 0, g_frames = 0;
size_t g_cmd_base = 0;
std::ofstream g_log;
std::string g_sel_trace;

void out(const std::string& s) { std::printf("%s\n", s.c_str()); std::fflush(stdout); if (g_log) g_log << s << "\n"; }

void set_buttons(uint32_t m) {
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) SDL_JoystickSetVirtualButton(g_joy, b, (m >> b) & 1u);
}

/* Commands this scenario sent, without the runner input gate ("input 0" on open / "input 1" on close: checked separately). */
std::vector<std::string> new_cmds(const QmE2EState& st) {
    std::vector<std::string> v;
    for (size_t i = std::min(g_cmd_base, st.cmds.size()); i < st.cmds.size(); ++i) if (st.cmds[i].rfind("input ", 0) != 0) v.push_back(st.cmds[i]);
    return v;
}
/* The input gate must alternate off / on, one "input 0" per open and one "input 1" per close. */
bool gate_ok(const QmE2EState& st, std::string* why) {
    int expect = 0;
    for (auto& c : st.cmds) {
        if (c.rfind("input ", 0) != 0) continue;
        if (c == "input 0" && expect == 1) continue;   // the 10 s refresh while the menu stays open
        if (c != "input " + std::to_string(expect)) { *why = "input gate out of order at '" + c + "'"; return false; }
        expect ^= 1;
    }
    return true;
}
std::string join(const std::vector<std::string>& v) { std::string s; for (auto& x : v) s += (s.empty() ? "" : ", ") + x; return s.empty() ? "(none)" : s; }

/* ---- step builders ---- */
Step wait(int ms, std::string n = "wait") { Step s; s.name = n; s.ms = ms; return s; }
Step hold(uint32_t m, int ms, std::string n) { Step s; s.name = n; s.hold = m; s.ms = ms; return s; }
Step tap(uint32_t m, std::string n, int after = 160) { Step s; s.name = n; s.hold = m; s.ms = after; s.tap = true; return s; }
Step press(uint32_t m, std::string n, int ms = 70) { return hold(m, ms, n); }   // a normal press: held 70 ms (several frames)
Step expect(std::string n, std::function<bool(const QmE2EState&, std::string*)> f) { Step s; s.name = n; s.ms = 0; s.check = f; s.keep = true; return s; }
Step expect_open(bool open, std::string n) {
    return expect(n, [open](const QmE2EState& st, std::string* why) { *why = std::string("open=") + (st.open ? "1" : "0"); return st.open == open; });
}
Step expect_sel(std::string id) {
    return expect("selected " + id, [id](const QmE2EState& st, std::string* why) { *why = "sel=" + st.sel + " open=" + (st.open ? "1" : "0"); return st.open && st.sel == id; });
}
Step expect_cmds(std::vector<std::string> want, std::string n) {
    return expect(n, [want](const QmE2EState& st, std::string* why) { auto c = new_cmds(st); *why = "commands: " + join(c); return c == want; });
}
Step closing(Step s) { s.allow_close = true; return s; }

/* Open the menu with View+Y (pressed together, held 120 ms) and wait out the open cooldown. */
void open_menu(std::vector<Step>& v) {
    v.push_back(hold(VIEW | Y, 120, "View+Y"));
    v.push_back(wait(250));
    v.push_back(expect_open(true, "menu is open"));
}
/* Close with B and wait out the 700 ms cooldown before the next View+Y. */
void close_b(std::vector<Step>& v) {
    v.push_back(closing(press(B_, "B")));
    v.push_back(closing(wait(800)));
    v.push_back(expect("closed by B", [](const QmE2EState& st, std::string* why) { *why = "open=" + std::to_string(st.open) + " why=" + st.close_why; return !st.open && st.close_why == "B"; }));
}
void down_to(std::vector<Step>& v, const std::string& id, int presses) {
    for (int i = 0; i < presses; ++i) { v.push_back(press(DOWN, "down")); v.push_back(wait(60)); }
    v.push_back(expect_sel(id));
}

void build() {
    // rows on the stand-in (built-in runner, offline): Resume, Speed, Volume, Sound, Save state, Load state, Take a screenshot, Quit to RetroHub
    {   // 1. the phantom press from quickmenu.log (v1.28.6): the opening View+Y and an A held from the game stay down across the open
        Scenario s{"phantom press held across open (log case)"};
        s.steps.push_back(hold(A_, 200, "A held in the game"));
        s.steps.push_back(hold(A_ | VIEW | Y, 1500, "View+Y pressed with A still held; all held 1.5 s"));
        s.steps.push_back(expect_open(true, "still open after 1.5 s of held buttons"));
        s.steps.push_back(expect_cmds({}, "nothing was activated"));
        s.steps.push_back(hold(A_, 300, "View+Y released, A still held"));
        s.steps.push_back(wait(400, "everything released"));
        s.steps.push_back(expect_open(true, "still open after release"));
        s.steps.push_back(expect_sel("Resume"));
        close_b(s.steps);
        g_sc.push_back(s);
    }
    {   // 2. B held from the game across the open does not close it; a fresh B does
        Scenario s{"B held across open"};
        s.steps.push_back(hold(B_, 150, "B held in the game"));
        s.steps.push_back(hold(B_ | VIEW | Y, 900, "View+Y with B held"));
        s.steps.push_back(hold(B_, 600, "B still held"));
        s.steps.push_back(expect_open(true, "open while the old B is held"));
        s.steps.push_back(wait(200, "B released"));
        s.steps.push_back(expect_open(true, "open after B release"));
        close_b(s.steps);
        g_sc.push_back(s);
    }
    {   // 3. open / close 25 times in a row: every View+Y opens, every B closes, nothing else happens
        Scenario s{"25 open/close cycles"};
        for (int i = 0; i < 25; ++i) { open_menu(s.steps); close_b(s.steps); }
        s.steps.push_back(expect_cmds({}, "no commands from open/close"));
        g_sc.push_back(s);
    }
    {   // 4. hammering View+Y (10 presses, 60 ms apart) opens once and never closes it
        Scenario s{"rapid View+Y repeats"};
        for (int i = 0; i < 10; ++i) { s.steps.push_back(hold(VIEW | Y, 30, "View+Y")); s.steps.push_back(hold(VIEW, 30, "Y up")); }
        s.steps.push_back(wait(300));
        s.steps.push_back(expect_open(true, "open after the burst"));
        s.steps.push_back(expect_sel("Resume"));
        s.steps.push_back(wait(1500, "1.5 s idle"));
        s.steps.push_back(expect_open(true, "never closed by itself"));
        close_b(s.steps);
        g_sc.push_back(s);
    }
    {   // 5. Speed row: every step, both directions, clamps, and back to 1x
        Scenario s{"speed row all steps"};
        open_menu(s.steps);
        down_to(s.steps, "speed", 1);
        for (int i = 0; i < 7; ++i) { s.steps.push_back(press(RIGHT, "right")); s.steps.push_back(wait(60)); }   // 1x -> 1.5 2 3 5 8 Max (+1 clamped)
        s.steps.push_back(expect_cmds({"speed 1.5", "speed 2", "speed 3", "speed 5", "speed 8", "speed 0"}, "1x -> Max, one command per step, clamped at Max"));
        s.steps.push_back(expect("speed index Max", [](const QmE2EState& st, std::string* w) { *w = "idx=" + std::to_string(st.speed_idx); return st.speed_idx == 8; }));
        for (int i = 0; i < 6; ++i) { s.steps.push_back(press(LEFT, "left")); s.steps.push_back(wait(60)); }   // Max -> 1x
        s.steps.push_back(expect("back at 1x", [](const QmE2EState& st, std::string* w) { *w = "idx=" + std::to_string(st.speed_idx) + " cmds " + join(new_cmds(st)); return st.speed_idx == 2 && new_cmds(st).back() == "speed 1"; }));
        for (int i = 0; i < 4; ++i) { s.steps.push_back(press(LEFT, "left")); s.steps.push_back(wait(60)); }   // -> 0.5 0.25 (clamped)
        s.steps.push_back(expect("clamped at 0.25x", [](const QmE2EState& st, std::string* w) { *w = "idx=" + std::to_string(st.speed_idx); return st.speed_idx == 0 && new_cmds(st).back() == "speed 0.25"; }));
        s.steps.push_back(press(A_, "A on the speed row = +1"));
        s.steps.push_back(wait(60));
        s.steps.push_back(press(RIGHT, "right"));
        s.steps.push_back(wait(60));
        s.steps.push_back(expect("A steps +1, then right: 1x", [](const QmE2EState& st, std::string* w) { *w = "idx=" + std::to_string(st.speed_idx) + " open=" + std::to_string(st.open); return st.open && st.speed_idx == 2 && new_cmds(st).back() == "speed 1"; }));
        close_b(s.steps);
        g_sc.push_back(s);
    }
    {   // 6. held right auto-repeats (0.25x .. Max in one hold); the speed survives close / reopen
        Scenario s{"held right repeats; speed kept across reopen"};
        open_menu(s.steps);
        down_to(s.steps, "speed", 1);
        s.steps.push_back(hold(RIGHT, 1600, "right held 1.6 s"));
        s.steps.push_back(wait(100));
        s.steps.push_back(expect("repeat reached Max", [](const QmE2EState& st, std::string* w) { *w = "idx=" + std::to_string(st.speed_idx) + " cmds " + join(new_cmds(st)); return st.speed_idx == 8; }));
        close_b(s.steps);
        open_menu(s.steps);
        s.steps.push_back(expect("reopened at Max", [](const QmE2EState& st, std::string* w) { *w = "idx=" + std::to_string(st.speed_idx); return st.open && st.speed_idx == 8; }));
        down_to(s.steps, "speed", 1);
        s.steps.push_back(hold(LEFT, 1600, "left held 1.6 s"));
        s.steps.push_back(wait(100));
        for (int i = 0; i < 2; ++i) { s.steps.push_back(press(RIGHT, "right")); s.steps.push_back(wait(60)); }
        s.steps.push_back(expect("back to 1x", [](const QmE2EState& st, std::string* w) { *w = "idx=" + std::to_string(st.speed_idx); return st.speed_idx == 2 && new_cmds(st).back() == "speed 1"; }));
        close_b(s.steps);
        g_sc.push_back(s);
    }
    {   // 7. a press shorter than one frame still counts (event latch)
        Scenario s{"sub-frame taps"};
        open_menu(s.steps);
        s.steps.push_back(tap(DOWN, "down tapped inside one frame"));
        s.steps.push_back(expect_sel("speed"));
        s.steps.push_back(tap(RIGHT, "right tapped inside one frame"));
        s.steps.push_back(expect_cmds({"speed 1.5"}, "tap stepped the speed"));
        s.steps.push_back(tap(LEFT, "left tap"));
        s.steps.push_back(closing(tap(B_, "B tapped inside one frame", 800)));
        s.steps.push_back(expect("closed by the tap", [](const QmE2EState& st, std::string* w) { *w = "open=" + std::to_string(st.open) + " why=" + st.close_why; return !st.open && st.close_why == "B"; }));
        g_sc.push_back(s);
    }
    {   // 8. Save page and back: grid nav, B returns to the main page on the same row, then B closes
        Scenario s{"save page round trip"};
        open_menu(s.steps);
        down_to(s.steps, "Save state", 4);
        s.steps.push_back(press(A_, "A"));
        s.steps.push_back(wait(150));
        s.steps.push_back(expect("on the save page", [](const QmE2EState& st, std::string* w) { *w = "page=" + std::to_string(st.page) + " sel=" + st.sel; return st.open && st.page == 1; }));
        s.steps.push_back(press(RIGHT, "right")); s.steps.push_back(wait(60));
        s.steps.push_back(press(DOWN, "down")); s.steps.push_back(wait(60));
        s.steps.push_back(expect_sel("slot6"));
        s.steps.push_back(press(B_, "B")); s.steps.push_back(wait(150));
        s.steps.push_back(expect("main page, Save state row", [](const QmE2EState& st, std::string* w) { *w = "page=" + std::to_string(st.page) + " sel=" + st.sel; return st.open && st.page == 0 && st.sel == "Save state"; }));
        s.steps.push_back(expect_cmds({}, "no save was made"));
        down_to(s.steps, "Load state", 1);
        s.steps.push_back(press(A_, "A")); s.steps.push_back(wait(150));
        s.steps.push_back(press(DOWN, "down to Back")); s.steps.push_back(wait(60));
        s.steps.push_back(press(DOWN, "down to Back")); s.steps.push_back(wait(60));
        s.steps.push_back(press(A_, "A on Back")); s.steps.push_back(wait(150));
        s.steps.push_back(expect("Back returned to main", [](const QmE2EState& st, std::string* w) { *w = "page=" + std::to_string(st.page) + " sel=" + st.sel; return st.open && st.page == 0 && st.sel == "Load state"; }));
        close_b(s.steps);
        g_sc.push_back(s);
    }
    {   // 9. Save to a slot closes the menu (explicit action) and records the command
        Scenario s{"save to slot 1"};
        open_menu(s.steps);
        down_to(s.steps, "Save state", 4);
        s.steps.push_back(press(A_, "A")); s.steps.push_back(wait(150));
        s.steps.push_back(closing(press(A_, "A on slot 1")));
        s.steps.push_back(closing(wait(800)));
        s.steps.push_back(expect("closed by the save", [](const QmE2EState& st, std::string* w) { *w = "open=" + std::to_string(st.open) + " why=" + st.close_why + " cmds " + join(new_cmds(st)); return !st.open && st.close_why == "slot action" && new_cmds(st) == std::vector<std::string>{"save_state 0"}; }));
        g_sc.push_back(s);
    }
    {   // 10. A on Resume closes; A held into the next open does not
        Scenario s{"Resume, and A held into the next open"};
        open_menu(s.steps);
        s.steps.push_back(closing(press(A_, "A on Resume")));
        s.steps.push_back(closing(wait(800)));
        s.steps.push_back(expect("closed by Resume", [](const QmE2EState& st, std::string* w) { *w = "open=" + std::to_string(st.open) + " why=" + st.close_why; return !st.open && st.close_why == "Resume"; }));
        s.steps.push_back(hold(A_, 100, "A down"));
        s.steps.push_back(hold(A_ | VIEW | Y, 300, "View+Y while A held"));
        s.steps.push_back(hold(A_, 700, "A still held"));
        s.steps.push_back(expect_open(true, "held A did not resume"));
        s.steps.push_back(wait(100, "A up"));
        s.steps.push_back(closing(press(A_, "fresh A")));
        s.steps.push_back(closing(wait(800)));
        s.steps.push_back(expect("fresh A resumed", [](const QmE2EState& st, std::string* w) { *w = "open=" + std::to_string(st.open) + " why=" + st.close_why; return !st.open && st.close_why == "Resume"; }));
        g_sc.push_back(s);
    }
    {   // 11. volume and mute rows; View held (part of the combo) while navigating still works
        Scenario s{"volume / mute, View held while navigating"};
        open_menu(s.steps);
        s.steps.push_back(hold(VIEW, 100, "View held"));
        s.steps.push_back(hold(VIEW | DOWN, 70, "down (View held)")); s.steps.push_back(hold(VIEW, 60, ""));
        s.steps.push_back(hold(VIEW | DOWN, 70, "down (View held)")); s.steps.push_back(hold(VIEW, 60, ""));
        s.steps.push_back(expect_sel("volume"));
        s.steps.push_back(press(LEFT, "left")); s.steps.push_back(wait(60));
        s.steps.push_back(press(DOWN, "down")); s.steps.push_back(wait(60));
        s.steps.push_back(press(A_, "A toggles mute")); s.steps.push_back(wait(60));
        s.steps.push_back(expect("volume 95, muted", [](const QmE2EState& st, std::string* w) { *w = "vol=" + std::to_string(st.vol) + " mute=" + std::to_string(st.mute) + " " + join(new_cmds(st)); return st.vol == 95 && st.mute && new_cmds(st) == std::vector<std::string>{"volume 95", "mute 1"}; }));
        s.steps.push_back(press(RIGHT, "right toggles back")); s.steps.push_back(wait(60));
        s.steps.push_back(press(UP, "up")); s.steps.push_back(wait(60));
        s.steps.push_back(press(RIGHT, "right")); s.steps.push_back(wait(60));
        s.steps.push_back(expect("volume 100, unmuted", [](const QmE2EState& st, std::string* w) { *w = "vol=" + std::to_string(st.vol) + " mute=" + std::to_string(st.mute); return st.vol == 100 && !st.mute; }));
        close_b(s.steps);
        g_sc.push_back(s);
    }
    {   // 12. B held from closing, then View+Y while still holding B: opens and stays open
        Scenario s{"B held across close and reopen"};
        open_menu(s.steps);
        s.steps.push_back(closing(hold(B_, 900, "B pressed and held (closes, cooldown passes)")));
        s.steps.push_back(hold(B_ | VIEW | Y, 300, "View+Y with B still held"));
        s.steps.push_back(hold(B_, 600, "B held"));
        s.steps.push_back(expect_open(true, "reopened, old B ignored"));
        s.steps.push_back(wait(100));
        close_b(s.steps);
        g_sc.push_back(s);
    }
    {   // 13. Quit is an explicit action
        Scenario s{"quit to RetroHub"};
        open_menu(s.steps);
        s.steps.push_back(hold(UP, 70, "up (wraps? no: clamps at Resume)")); s.steps.push_back(wait(60));
        s.steps.push_back(expect_sel("Resume"));
        s.steps.push_back(hold(DOWN, 1400, "down held to the bottom")); s.steps.push_back(wait(100));
        s.steps.push_back(expect_sel("Quit to RetroHub"));
        s.steps.push_back(closing(press(A_, "A on Quit")));
        s.steps.push_back(closing(wait(300)));
        s.steps.push_back(expect("quit", [](const QmE2EState& st, std::string* w) { *w = "open=" + std::to_string(st.open) + " why=" + st.close_why + " " + join(new_cmds(st)); return !st.open && st.close_why == "Quit" && new_cmds(st) == std::vector<std::string>{"quit"}; }));
        g_sc.push_back(s);
    }
}

}  // namespace

bool qm_e2e_active() {
    static const bool on = std::getenv("HUB_QM_E2E") && std::getenv("HUB_SCREENSHOT");
    return on;
}

void qm_e2e_boot() {
    // never open the user's real controllers: every joystick backend off; only the virtual device below exists
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
}

void qm_e2e_start() {
    std::string lp = std::getenv("HUB_SCREENSHOT");
    size_t dot = lp.rfind('.');
    g_log.open(lp.substr(0, dot == std::string::npos ? lp.size() : dot) + "-qm-e2e.log");
    SDL_VirtualJoystickDesc d{};
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.name = "RetroHub QM E2E pad";
    const int idx = SDL_JoystickAttachVirtualEx(&d);
    g_joy = idx >= 0 ? SDL_JoystickOpen(idx) : nullptr;
    if (!g_joy) { out(std::string("FAIL could not attach the virtual joystick: ") + SDL_GetError()); g_fail = 1; g_done = true; return; }
    refresh_pads();   // A.pads now holds the virtual controller (the added event also refreshes)
    const SDL_JoystickID id = SDL_JoystickInstanceID(g_joy);
    qm_pad_restrict((int)id, true);
    int found = 0;
    for (auto* c : A.pads) if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) == id) ++found;
    if (!found) { out("FAIL the virtual joystick is not an SDL game controller"); g_fail = 1; g_done = true; return; }
    for (int b = 0; b <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT; ++b) {   // the virtual button index must be the same game-controller button
        set_buttons(1u << b);
        SDL_JoystickUpdate();
        SDL_GameControllerUpdate();
        const uint32_t raw = qm_pad_raw();
        if (raw != (1u << b)) { out("FAIL virtual button " + std::to_string(b) + " reads as mask " + std::to_string(raw)); g_fail = 1; g_done = true; return; }
    }
    set_buttons(0);
    SDL_JoystickUpdate();
    qm_pad_take_taps();
    qm_e2e_enable();
    build();
    out("qm e2e: SDL " + std::to_string(SDL_MAJOR_VERSION) + "." + std::to_string(SDL_MINOR_VERSION) + "." + std::to_string(SDL_PATCHLEVEL) + ", virtual pad ok, " + std::to_string(g_sc.size()) + " scenarios");
    g_started = true;
}

bool qm_e2e_frame() {
    if (g_done) return false;
    if (!g_started) return true;
    ++g_frames;
    const QmE2EState st = qm_e2e_state();
    Scenario& sc = g_sc[g_si];
    const bool have_step = g_step < sc.steps.size();
    if (g_was_open && !st.open && !(have_step && sc.steps[g_step].allow_close)) {
        out("  FAIL [" + sc.name + "] the menu closed by itself during step '" + (have_step ? sc.steps[g_step].name : "?") + "' (why=" + st.close_why + ")");
        ++g_fail; g_sc_failed = true;
    }
    g_was_open = st.open;
    if (have_step) {
        Step& s = sc.steps[g_step];
        if (!g_step_started) {
            g_step_started = true;
            g_step_t0 = SDL_GetTicks();
            if (s.keep) {}
            else if (s.tap) { set_buttons(s.hold); SDL_JoystickUpdate(); set_buttons(0); }   // down and up before the next frame reads the state
            else set_buttons(s.hold);
        }
        if (SDL_GetTicks() - g_step_t0 < (Uint32)s.ms) return true;
        if (s.check) {
            std::string why;
            ++g_checks;
            if (!s.check(st, &why)) { out("  FAIL [" + sc.name + "] " + s.name + ": " + why); ++g_fail; g_sc_failed = true; }
        }
        ++g_step;
        g_step_started = false;
        return true;
    }
    // scenario finished: release everything, make sure the menu is closed before the next one
    set_buttons(0);
    if (st.open) { out("  FAIL [" + sc.name + "] scenario ended with the menu open"); ++g_fail; g_sc_failed = true; }
    {   // the runner's input gate: off while the menu is open, on again after it closed
        std::string why;
        ++g_checks;
        if (!gate_ok(st, &why) || (!st.cmds.empty() && st.cmds.back() == "input 0")) { out("  FAIL [" + sc.name + "] " + (why.empty() ? std::string("runner input left off after close") : why)); ++g_fail; g_sc_failed = true; }
    }
    out(std::string(g_sc_failed ? "FAIL " : "PASS ") + sc.name);
    if (!g_sc_failed) ++g_pass;
    g_cmd_base = st.cmds.size();
    g_sc_failed = false;
    g_step = 0;
    if (++g_si >= g_sc.size()) {
        out("qm e2e: " + std::to_string(g_pass) + "/" + std::to_string(g_sc.size()) + " scenarios passed, " + std::to_string(g_checks) + " checks, " + std::to_string(g_fail) + " failures, " + std::to_string(g_frames) + " frames");
        g_done = true;
        return false;
    }
    return true;
}

int qm_e2e_exit_code() { return g_fail == 0 && g_pass == (int)g_sc.size() && !g_sc.empty() ? 0 : 1; }
