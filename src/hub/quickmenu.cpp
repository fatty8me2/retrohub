#include "app.h"
#include "dolphin_speed.h"
#include "tex_cache_logic.h"

#include <SDL.h>
#include <shellapi.h>

#include <atomic>
#include <deque>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include "runner_ctl.h"
#include "cloud_engine.h"
#include "qm_logic.h"
#include "cheats_game.h"
#include "qm_nav.h"
#include "qm_pad.h"
#include "exit_combo.h"
#include "qm_party.h"
#include "ext_media.h"
#include "qm_card.h"
#include "challenge_ui.h"
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"

/* ---------- In-game quick menu (View + Y) and screenshots gallery ----------
 * Contract with the built-in runners: src/common/runner_ctl.h and docs/RUNNER_CTL.md. The hub writes <run_dir>/control.json,
 * the runner acts once per seq and writes control_ack.json. Save states: <states>/slot<N>.state + .png thumbnail + .json sidecar.
 * Screenshots: <vault>/<system>/<game key>/shots/<yyyyMMdd-HHmmss>.png. */

unsigned long gc_netplay_dolphin_pid();   // lobby.cpp: the GameCube NetPlay Dolphin (never A.proc), 0 when none
static bool runner_has_ctl(const hub::GameEntry& e);   // defined below
static std::atomic<bool> g_qm_showing{false};

namespace {

SDL_Renderer* g_ren = nullptr;

struct Tex { SDL_Texture* t = nullptr; int w = 0, h = 0; std::string stamp; };
/* LRU, never evicts what was used this frame, destroys evicted textures at the start of the next frame (tex_cache_logic.h). */
hubtex::TexCache<Tex> g_tex(128, [](Tex& t) { if (t.t) SDL_DestroyTexture(t.t); });   // roomy: gallery (60) + state thumbnails + viewer all fit while touched each frame
hubtex::DecodeBudget g_shot_budget(2);   // every tex_for call: at most this many new PNG decodes per frame

std::string file_stamp(const std::string& p) {
    std::error_code ec;
    auto fp = std::filesystem::u8path(p);
    auto t = std::filesystem::last_write_time(fp, ec);
    if (ec) return std::string();
    auto sz = std::filesystem::file_size(fp, ec);
    return std::to_string((long long)t.time_since_epoch().count()) + ":" + std::to_string((long long)sz);
}

/* PNG (or BMP, used by the headless screenshot aid) file -> texture, cached by path + stamp. */
Tex* tex_for(const std::string& path, hubtex::DecodeBudget* budget = &g_shot_budget) {
    if (!g_ren) return nullptr;
    if (path.size() > 4 && _stricmp(path.c_str() + path.size() - 4, ".mp4") == 0) return tex_for(path + ".jpg", budget);   // instant-replay clip: its thumbnail
    std::string st = file_stamp(path);
    if (st.empty()) return nullptr;
    Tex* hit = g_tex.find(path);
    if (hit && hit->stamp == st) return hit->t ? hit : nullptr;
    if (budget && !budget->spend()) return hit && hit->t ? hit : nullptr;   // over this frame's decode budget: keep showing the old picture (or the caller's placeholder), refresh next frame
    Tex nt;
    nt.stamp = st;
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!data.empty()) {
        if (path.size() > 4 && (path.compare(path.size() - 4, 4, ".bmp") == 0 || path.compare(path.size() - 4, 4, ".BMP") == 0)) {
            SDL_RWops* rw = SDL_RWFromConstMem(data.data(), (int)data.size());
            SDL_Surface* s = rw ? SDL_LoadBMP_RW(rw, 1) : nullptr;
            if (s) { nt.t = SDL_CreateTextureFromSurface(g_ren, s); nt.w = s->w; nt.h = s->h; SDL_FreeSurface(s); }
        } else {
            int w = 0, h = 0, n = 0;
            unsigned char* px = stbi_load_from_memory((const unsigned char*)data.data(), (int)data.size(), &w, &h, &n, 4);
            if (px) {
                nt.t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
                if (nt.t) { SDL_UpdateTexture(nt.t, nullptr, px, w * 4); SDL_SetTextureBlendMode(nt.t, SDL_BLENDMODE_BLEND); nt.w = w; nt.h = h; }
                stbi_image_free(px);
            }
        }
    }
    Tex* stored = g_tex.insert(path, std::move(nt));   // a stale previous texture for this path is buried, not destroyed mid-frame
    return stored->t ? stored : nullptr;
}

std::string trim_sep(std::string d) {
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    return d;
}

std::string time_text(long long t) {
    if (t <= 0) return "";
    std::time_t tt = (std::time_t)t;
    std::tm tm{};
    localtime_s(&tm, &tt);
    char b[48];
    std::strftime(b, sizeof b, "%b %d, %H:%M", &tm);
    return b;
}

std::string states_dir_of(const std::string& key, const std::string& sys) { return trim_sep(hub::saves::states_dir(sys, key)); }

struct SlotInfo { bool exists = false; long long time = 0; long long playtime = 0; int disc = 0; };

SlotInfo read_slot(const std::string& dir, int slot) {
    SlotInfo s;
    std::error_code ec;
    auto sp_ = std::filesystem::u8path(runctl::state_file(dir, slot));
    if (!std::filesystem::exists(sp_, ec)) return s;
    s.exists = true;
    std::string txt;
    runctl::Sidecar sc;
    if (runctl::read_text(runctl::sidecar_file(dir, slot), &txt) && runctl::parse_sidecar(txt, &sc) && sc.time > 0) { s.time = sc.time; s.playtime = sc.playtime; s.disc = sc.disc; }
    else {
        auto ft = std::filesystem::last_write_time(sp_, ec);
        if (!ec) s.time = (long long)std::chrono::duration_cast<std::chrono::seconds>(ft.time_since_epoch()).count() - 11644473600LL;   // FILETIME epoch -> unix
    }
    return s;
}

/* ---------------- the menu ---------------- */
enum class QPage { Main, Save, Load, Pad, Chat, Emote, Pass, Challenge };
bool g_open_req = false, g_shot_fake = false;
QPage g_page = QPage::Main;
bool g_ch_fake = false;                         // HUB_SHOT_QM=challenge: stand-in flow (no runner)
hub::challenge::SendPhase g_ch_fake_phase = hub::challenge::SendPhase::PickFriends;
std::vector<std::string> g_ch_fake_sel;
char g_ch_buf[48] = {0};
long long g_pending = 0;                       // seq waiting for an ack (0 = none)
std::string g_pending_what;                    // "Saved slot 3" ...
static ULONGLONG g_open_ms = 0, g_activity_ms = 0;
/* The menu's own controls (qm_nav.h): read straight from the pads every frame, independent of the OS focus and of ImGui's nav.
 * g_in gates buttons held at open (View+Y itself, A/B held in the game) until released; g_nav keeps the selected row by id. */
hubqm::MenuInput g_in;
hubqm::MenuNav g_nav;
int g_row_y = 0;               // next row's y while drawing
bool g_sub_open = false;       // a popup over the menu (Invite friends picker) owns the input: ImGui nav drives it
bool g_e2e = false;            // headless end-to-end test (qm_e2e.cpp): stand-in running game with a control file
std::vector<std::string> g_e2e_cmds;

/* ---- GameCube NetPlay + Dolphin pause / states ----
 * GameCube NetPlay never sets A.proc (that would change launch rules); its Dolphin is found through gc_netplay_dolphin_pid (lobby.cpp). */
unsigned long gc_pid() { return (g_e2e || g_shot_fake) ? 0ul : hubqm::gc_menu_pid(A.proc != nullptr, gc_netplay_dolphin_pid()); }
bool qm_online() { return g_e2e ? false : hubqm::online_for_menu(A.run.online, gc_pid()); }
const hub::GameEntry* cur_entry(const std::string& key) {
    if (const hub::GameEntry* e = find_entry(key)) return e;
    return (gc_pid() && GCO.entry.key == key) ? &GCO.entry : nullptr;
}
std::vector<unsigned long> game_pid_list() {
    if (const unsigned long g = gc_pid()) return {g};
    return running_game_pids();
}
HWND window_of_pid(DWORD pid) {   // largest visible, captioned top-level window of one process
    struct Pick { DWORD pid; HWND best; long long area; } pk{pid, nullptr, 0};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* p = (Pick*)lp;
        DWORD q = 0;
        GetWindowThreadProcessId(h, &q);
        if (q != p->pid || !IsWindowVisible(h) || !(GetWindowLongW(h, GWL_STYLE) & WS_CAPTION)) return TRUE;
        RECT r{};
        GetWindowRect(h, &r);
        const long long a = (long long)(r.right - r.left) * (r.bottom - r.top);
        if (a > p->area) { p->area = a; p->best = h; }
        return TRUE;
    }, (LPARAM)&pk);
    return pk.best;
}
/* Back to the game after the menu (every close path). GameCube NetPlay: raise its Dolphin; the hub is the foreground process
 * here (the player just used the menu), so a plain SetForegroundWindow is allowed. Everything else: launch.cpp's focus_game. */
void qm_focus_game() {
    const unsigned long g = gc_pid();
    if (!g) { focus_game(); return; }
    if (g_headless) return;
    hub_menu_topmost(false);
    HWND h = window_of_pid((DWORD)g);
    if (!h) return;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);   // popup-guard: ok (raises the GAME after the player's own View+Y menu)
    SetForegroundWindow(h);   // popup-guard: ok (raises the GAME after the player's own View+Y menu)
}
/* Leave (menu "Quit", Back+Start held): ask Dolphin to close like the player closing it; gc_tick then sees it gone and runs gc_stop_all
 * (forwarder, UI driver, room state). If Dolphin ignores the close for 20 s gc_tick ends it (GCO.close_deadline). */
void gc_leave(const char* why) {
    const unsigned long g = gc_pid();
    if (!g) return;
    qm_log(std::string("gc netplay: leaving (") + why + ")");
    if (GCO.close_deadline) return;   // already closing
    close_pid_windows(g);
    GCO.close_deadline = GetTickCount64() + 20000;
    set_notice("Leaving the game...", 6.0);
}

/* Dolphin's pause / save / load state hotkeys, as key chords (qm_logic.h) sent once the game window is in front. */
std::vector<hubqm::DolphinAct> g_dol_queue;   // the menu's buttons since it opened
std::atomic<bool> g_dol_paused{false};        // Dolphin is paused by us (toggled per chord that really went out)
void dolphin_send_acts(std::vector<hubqm::DolphinAct> acts) {
    if (acts.empty()) return;
    if (g_headless || autotest_active()) { qm_log("dolphin: " + std::to_string(acts.size()) + " menu hotkey chord(s) not injected (headless / QA)"); return; }
    std::vector<unsigned long> pids = game_pid_list();
    std::thread([pids, acts]() {
        auto game_fg = [&]() {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            for (unsigned long p : pids) if (p == pid) return !qm_is_open();
            return false;
        };
        for (int i = 0; i < 60 && !game_fg(); ++i) Sleep(50);
        if (!game_fg()) { qm_log("dolphin: game never became foreground, hotkey chord(s) not sent"); return; }
        Sleep(250);
        for (hubqm::DolphinAct a : acts) {
            const std::vector<int> keys = hubqm::dolphin_act_scans(a);
            const int n = hubqm::run_key_chord(keys, game_fg, [](int sc, bool up) {
                INPUT in{}; in.type = INPUT_KEYBOARD; in.ki.wScan = (WORD)sc; in.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0); SendInput(1, &in, sizeof(INPUT));
                Sleep(up ? 35 : 25);
            });
            qm_log("dolphin: hotkey chord " + std::string(hubqm::dolphin_act_hotkey_name(a)) + " " + std::to_string(n) + "/" + std::to_string(keys.size()));
            if (n == (int)keys.size() && a == hubqm::DolphinAct::Pause) g_dol_paused = !g_dol_paused.load();
            Sleep(150);
        }
    }).detach();
}
static const char* g_close_why = "other";
bool g_pending_close = true;                   // close the menu and return to the game when the ack arrives
int g_pending_slot = -2;
runctl::Cmd g_pending_cmd = runctl::Cmd::None;  // the command the pending ack belongs to (a failed rewind / filter / clip ack can hide its row)
hubqm::FeatureHide g_hide;                      // rows the runner answered "unsupported" for (this game session)
hubqm::OverwriteConfirm g_ovw;                  // save page: replacing a used slot takes two presses
double g_pending_at = 0;
std::string g_sys, g_key, g_title;
/* Plan 4: pause while the menu is open (offline), pass the controller, emotes. */
bool g_pause_auto = true;          // setting "Pause when menu opens" (qm_prefs.txt in the data folder); on by default
bool g_pause_manual = false;       // the Pause row: the game is meant to stand still while the menu is open (set from the setting on open)
bool g_pause_sent = false;         // the last pause value queued for the runner
bool g_prefs_loaded = false;
void enqueue_cmd(runctl::Command c, int tag = 0);   // below: commands to the runner go out one at a time
const char* g_close_req_why = "reset requested";

bool ctl_ready() { return g_e2e || (A.proc && !A.run.ctl_path.empty()); }

void resume_for_action();   // below
void send(runctl::Cmd c, int slot, const std::string& done_text, bool close) {
    if (g_e2e) { resume_for_action(); g_e2e_cmds.push_back(std::string(runctl::cmd_name(c)) + " " + std::to_string(slot)); push_toast(done_text, COL_GOOD, 3.0); if (close) { g_page = QPage::Main; g_close_why = "slot action"; ImGui::CloseCurrentPopup(); } return; }
    if (g_shot_fake || !ctl_ready()) return;
    resume_for_action();   // a paused runner (n64run) refuses state / screenshot commands: resume first
    runctl::Command cm; cm.cmd = c; cm.slot = slot;
    g_pending = -1;   // waiting for the queue to write it, then for the ack
    g_pending_what = done_text; g_pending_close = close; g_pending_slot = slot; g_pending_at = ImGui::GetTime(); g_pending_cmd = c;
    enqueue_cmd(cm, 2);
}

/* Screen filter / clip rows: the menu stays open; the toast is `done_text`, or the runner's own answer when that is empty. */
bool send_feature(runctl::Command cm, const std::string& done_text, bool resume_first, bool close = false) {   // false: not queued
    if (g_e2e) {
        if (resume_first) resume_for_action();
        g_e2e_cmds.push_back(std::string(runctl::cmd_name(cm.cmd)) + " " + (cm.cmd == runctl::Cmd::Shader ? cm.text : std::to_string(cm.value)));
        push_toast(done_text.empty() ? std::string("Done") : done_text, COL_GOOD, 3.0);
        return true;
    }
    if (g_shot_fake) return true;
    if (!ctl_ready()) return false;
    if (resume_first) resume_for_action();   // n64run refuses a clip while paused
    g_pending = -1;
    g_pending_what = done_text; g_pending_close = close; g_pending_slot = -2; g_pending_at = ImGui::GetTime(); g_pending_cmd = cm.cmd;
    enqueue_cmd(cm, 2);
    return true;
}

void nav_ring(ImVec2 p0, ImVec2 p1) {
    {   // a long menu scrolls: bring the pad-selected row into view once per selection change (a mouse wheel is not fought)
        static std::string last_sel;
        static ULONGLONG last_open = 0;
        if (last_open != g_open_ms) { last_open = g_open_ms; last_sel.clear(); }
        if (last_sel != g_nav.sel) { last_sel = g_nav.sel; ImGui::ScrollToRect(ImGui::GetCurrentWindow(), ImRect(p0, p1 + ImVec2(0, sp(1.f)) ), ImGuiScrollFlags_KeepVisibleEdgeY); }
    }
    ImGui::GetWindowDrawList()->AddRect(p0 - ImVec2(2, 2) * S, p1 + ImVec2(2, 2) * S, ui::ACCENT, sp(1.6f), 0, 2.5f * S);
}

/* A full-width menu button. A (pad / Enter) on the selected row or a mouse click presses it. */
bool menu_button(const char* label, bool enabled, ui::Icon ic, bool primary = false) {
    const bool en = enabled && g_pending == 0;
    const hubqm::NavAction a = g_nav.add(label, false, en, 0, g_row_y++);
    float w = ImGui::GetContentRegionAvail().x, h = sp(6.5f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ui::button(label, ImVec2(w, h), primary, en, ic);
    if (g_nav.selected(label)) nav_ring(p, p + ImVec2(w, h));
    if (a.activate && en) qm_log(std::string("menu: activate \"") + label + "\"");
    return en && (pressed || a.activate);
}

void draw_slot_card(const std::string& dir, int slot, bool saving, bool* chosen, int nx, int ny, bool armed = false) {
    SlotInfo si = read_slot(dir, slot);
    const std::string nid = "slot" + std::to_string(slot);
    float cw = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float th = cw * 0.625f, h = th + sp(5.5f);
    ImGui::PushID(slot + 5);
    bool enabled = saving || si.exists;
    const hubqm::NavAction na = g_nav.add(nid, false, enabled && g_pending == 0, nx, ny);
    ImGui::BeginDisabled(!enabled || g_pending != 0);
    bool clicked = ImGui::InvisibleButton("slot", ImVec2(cw, h));
    ImGui::EndDisabled();
    if (na.activate) { clicked = true; qm_log("menu: activate " + nid); }
    bool hot = ImGui::IsItemHovered() || g_nav.selected(nid);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q1(p.x + cw, p.y + h);
    dl->AddRectFilled(p, q1, hot ? ui::PANEL2 : ui::PANEL, sp(1.25f));
    ImVec2 a0(p.x + sp(0.5f), p.y + sp(0.5f)), a1(p.x + cw - sp(0.5f), p.y + sp(0.5f) + th - sp(1.f));
    Tex* t = si.exists ? tex_for(runctl::thumb_file(dir, slot)) : nullptr;
    if (!t && si.exists) { std::string bm = runctl::thumb_file(dir, slot); bm.replace(bm.size() - 4, 4, ".bmp"); t = tex_for(bm); }
    if (t) ui::draw_art(dl, t->t, t->w, t->h, a0, a1, sp(1.f));
    else {
        dl->AddRectFilled(a0, a1, IM_COL32(0, 0, 0, 90), sp(1.f));
        const char* e = si.exists ? "No preview" : "Empty";
        ImVec2 ts = ImGui::CalcTextSize(e);
        dl->AddText(ImVec2((a0.x + a1.x - ts.x) * 0.5f, (a0.y + a1.y - ts.y) * 0.5f), ui::TEXT3, e);
    }
    std::string name = slot < 0 ? "Resume point" : "Slot " + std::to_string(slot + 1);
    dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(p.x + sp(1.f), p.y + th + sp(0.25f)), enabled ? ui::TEXT : ui::TEXT3, name.c_str());
    std::string sub = si.exists ? time_text(si.time) : (saving ? "Save here" : "-");
    if (armed) sub = "Press again to replace";
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(p.x + sp(1.f), p.y + th + sp(0.25f) + ui::F.bold->FontSize * 1.25f), armed ? ui::WARN : ui::TEXT2, sub.c_str());
    if (hot) dl->AddRect(p, q1, ui::ACCENT, sp(1.25f), 0, 2.f);
    ImGui::PopID();
    if (clicked) *chosen = true;
}

void slot_page(bool saving) {
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted(saving ? "Save state" : "Load state");
    ImGui::PopFont();
    ImGui::TextDisabled(saving ? "Pick a slot. A used slot is replaced." : "Pick a slot to go back to.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    std::string dir = states_dir_of(g_key, g_sys);
    if (saving) g_ovw.tick(g_nav.sel, (long long)GetTickCount64()); else g_ovw.cancel();   // any navigation (or a wait) cancels "press again to replace"
    const int cols = 5;
    int total = saving ? runctl::kSlots : runctl::kSlots + 1;   // load: + the resume point
    float gap = sp(1.f);
    float cw = (ImGui::GetContentRegionAvail().x - gap * (cols - 1)) / cols;
    for (int i = 0; i < total; ++i) {
        int slot = i < runctl::kSlots ? i : runctl::kAutoSlot;
        if (i % cols) ImGui::SameLine(0, gap);
        ImGui::BeginGroup();
        ImGui::PushItemWidth(cw);
        ImGui::PushID(i);
        ImGui::BeginChild("##c", ImVec2(cw, cw * 0.625f + sp(5.5f) + sp(0.5f)), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        bool chosen = false;
        draw_slot_card(dir, slot, saving, &chosen, i % cols, i / cols, saving && g_ovw.is_armed(slot));
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::PopItemWidth();
        ImGui::EndGroup();
        if (chosen) {
            if (saving) {
                const bool used = read_slot(dir, slot).exists;
                if (g_ovw.press(slot, used, (long long)GetTickCount64(), g_nav.sel) == hubqm::OverwriteConfirm::Result::Save)
                    send(runctl::Cmd::SaveState, slot, "Saved to slot " + std::to_string(slot + 1), true);
            }
            else send(runctl::Cmd::LoadState, slot, slot < 0 ? "Went back to the resume point" : "Loaded slot " + std::to_string(slot + 1), true);
        }
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (g_pending) ImGui::TextDisabled("%s", saving ? "Saving..." : "Loading...");
    else {
        const hubqm::NavAction a = g_nav.add("back", false, true, 0, (total + cols - 1) / cols);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool b = ui::button("Back", ImVec2(sp(14.f), sp(5.5f)), false, true, ui::Icon::ChevronL);
        if (g_nav.selected("back")) nav_ring(p, p + ImVec2(sp(14.f), sp(5.5f)));
        if (b || a.activate) { g_nav.sel = saving ? "Save state" : "Load state"; g_page = QPage::Main; }
    }
}

/* ---- speed / volume / mute / external screenshot state (one game session) ---- */
int g_speed_idx = hubqm::kSpeedNormal;   // resets to 1x every session
int g_vol = 100, g_launch_vol = 100;     // slider and the volume the game was launched with
bool g_mute = false;
bool g_ext_dirty = false, g_shot_online = false, g_shot_ext = false;   // g_shot_*: HUB_SHOT_QM_ONLINE / HUB_SHOT_QM_EXT stand-ins
std::string g_sess_key;
ULONGLONG g_sess_tick = 0;
double g_ext_next = 0, g_ext_shot_at = 0;
int g_ext_sessions = -1;                 // audio sessions found at the last attempt (-1 = none tried yet)
std::string g_ext_shot_path;

/* Fire and forget: the menu stays open and responsive; the runner acts at its next poll. control.json holds ONE command and a second
 * write before the runner read the first replaces it, so everything goes through a queue that writes the next command when the previous
 * one is acked (or 0.7 s passed). Repeats of the same setting (a held Volume step) are coalesced into one. */
struct Queued { runctl::Command c; int tag = 0; ULONGLONG first_try = 0; };
std::deque<Queued> g_cmdq;
hubqm::CmdPacer g_pacer;
long long g_ho_seq = 0;            // seq of the queued hand-off command (0 = none yet)
bool g_ho_wait = false;            // a hand-off is waiting for its ack
double g_ho_at = 0;
std::string g_ho_name;
std::string g_cd_name;             // pass countdown on the game's OSD
ULONGLONG g_cd_t0 = 0;
int g_cd_shown = 0;
hubqm::EmoteLimiter g_emote_lim;
double g_emote_note_at = -100;

void send_quiet(runctl::Command c) {
    if (g_e2e) { g_e2e_cmds.push_back(std::string(runctl::cmd_name(c.cmd)) + " " + (c.cmd == runctl::Cmd::Shader ? c.text : c.cmd == runctl::Cmd::Speed ? [&] { char b[24]; std::snprintf(b, sizeof b, "%g", c.speed); return std::string(b); }() : std::to_string(c.value))); return; }
    if (g_shot_fake || !ctl_ready()) return;
    enqueue_cmd(c);
}

void enqueue_cmd(runctl::Command c, int tag) {
    if (g_e2e && tag == 0) { send_quiet(c); return; }
    if (g_e2e) return;
    if (g_shot_fake || !ctl_ready()) { if (tag == 2) g_pending = 0; return; }
    const bool coalesce = c.cmd == runctl::Cmd::Speed || c.cmd == runctl::Cmd::Volume || c.cmd == runctl::Cmd::Mute || c.cmd == runctl::Cmd::Overlay ||
                          c.cmd == runctl::Cmd::Pause || c.cmd == runctl::Cmd::Input || c.cmd == runctl::Cmd::OsdCorner || c.cmd == runctl::Cmd::ReloadControls;
    if (coalesce) for (auto& q : g_cmdq) if (q.c.cmd == c.cmd) { q.c = c; return; }
    if (c.cmd == runctl::Cmd::OsdMsg) {   // a flood of toasts must not bury a hand-off or a state save
        int n = 0;
        for (auto& q : g_cmdq) if (q.c.cmd == runctl::Cmd::OsdMsg) ++n;
        if (n >= 4) { for (auto it = g_cmdq.begin(); it != g_cmdq.end(); ++it) if (it->c.cmd == runctl::Cmd::OsdMsg) { g_cmdq.erase(it); break; } }
    }
    Queued q; q.c = c; q.tag = tag;
    g_cmdq.push_back(q);
}

/* Every frame: write the head of the queue when the runner is done with the previous command. */
void cmdq_pump() {
    if (!A.proc || A.run.ctl_path.empty()) { g_cmdq.clear(); return; }
    const ULONGLONG now = GetTickCount64();
    if (g_pacer.sent_seq > g_pacer.acked_seq) {
        static ULONGLONG last_read = 0;
        if (now - last_read >= 80) {
            last_read = now;
            std::string txt;
            runctl::Ack a;
            if (runctl::read_text(runctl::ack_path_for(A.run.ctl_path), &txt) && runctl::parse_ack(txt, &a)) g_pacer.ack(a.seq);
        }
    }
    if (g_cmdq.empty() || !g_pacer.may_send((long long)now)) return;
    Queued& q = g_cmdq.front();
    if (!q.first_try) q.first_try = now;
    const long long seq = runctl::send_command_ex(A.run.ctl_path, q.c);
    if (!seq) {   // refused (an unacknowledged Quit is pending) or the file is busy: retry for a few seconds, then drop
        if (now - q.first_try < 5000) return;
        if (q.tag == 2) { g_pending = 0; push_toast("Could not talk to the game.", COL_WARN, 4.0); }
        if (q.tag == 1) { g_ho_wait = false; push_toast("Could not talk to the game.", COL_WARN, 4.0); }
        g_cmdq.pop_front();
        return;
    }
    g_pacer.sent(seq, (long long)now);
    if (q.tag == 1) g_ho_seq = seq;
    if (q.tag == 2) { g_pending = seq; g_pending_at = ImGui::GetTime(); }
    g_cmdq.pop_front();
}

/* The runner stands still while paused (n64run refuses state / screenshot / speed then): resume before such a command. */
void resume_for_action() {
    if (!g_pause_manual && !g_pause_sent) return;
    g_pause_manual = false;
    if (g_pause_sent) { g_pause_sent = false; runctl::Command c; c.cmd = runctl::Cmd::Pause; c.value = 0; enqueue_cmd(c); }
}

std::string prefs_path() { return hub::data_dir() + "qm_prefs.txt"; }
void prefs_load() {
    if (g_prefs_loaded) return;
    g_prefs_loaded = true;
    if (g_e2e || g_shot_fake) return;
    std::string txt;
    if (runctl::read_text(prefs_path(), &txt)) g_pause_auto = txt.find("pause_on_open=0") == std::string::npos;
}
void prefs_save() {
    if (g_e2e || g_shot_fake) return;
    runctl::write_text_atomic(prefs_path(), std::string("pause_on_open=") + (g_pause_auto ? "1" : "0") + "\n");
}

bool is_dolphin(const hub::GameEntry* e) { return e && e->system == hub::System::GC; }

/* Dolphin's real speed state (the keys sent so far) and the HUB_SHOT_QM_DOLPHIN stand-in. */
hubqm::DolphinSpeed g_dolphin;
bool g_shot_dolphin = false;
std::string ext_emu_name(const hub::GameEntry* e) {
    if (!e) return "this emulator";
    switch (e->system) {
        case hub::System::GC: return "Dolphin";
        case hub::System::WiiU: return "Cemu";
        case hub::System::N3DS: return "Azahar";
        case hub::System::PS2: return "PCSX2";
        case hub::System::PSP: return "PPSSPP";
        case hub::System::Switch: return hookshot_running() ? "Hookshot" : fritchii_running() ? "Fritchii" : "Lichii";
        default: return "this emulator";
    }
}
/* ---- Dolphin speed: robust application ----
 * A burst reports how many presses completed. The state (g_dolphin) is committed from THAT, never assumed: a burst that never started
 * leaves it unchanged and is retried (1.5 s, 3 s, 6 s ... up to kDolphinMaxRetries); one cut short makes it "unknown", and the next
 * plan then goes down to Dolphin's 0.1x floor and up again. View + LB held 1 s forces that path to 1x (qm_dolphin_reset). */
std::atomic<int> g_burst_state{0};   // 0 idle, 1 running, 2 finished (result in g_burst_done)
std::atomic<int> g_burst_done{-1};
std::vector<hubqm::DolphinKey> g_burst_plan;
int g_burst_target = hubqm::kSpeedNormal, g_retry_n = 0;
ULONGLONG g_retry_at = 0;
std::atomic<bool> g_close_req{false};
bool g_burst_dsu = false;    // the running burst went through the DSU pad
bool g_dsu_dead = false;     // this session: Dolphin's DSU client never connected -> keyboard path (needs the menu closed)
bool use_dsu() { return dolphin_speed_running() && !g_dsu_dead; }   // emergency reset: close the menu so the game is in front for the key burst

/* Menu closed (or a retry is due): press the Dolphin hotkeys that take it to the chosen step (offline GameCube only). */
void dolphin_apply_pending(const hub::GameEntry* e) {
    if (!is_dolphin(e) || qm_online() || g_shot_fake || g_burst_state.load() != 0 || (qm_is_open() && !use_dsu())) return;   // the DSU pad needs no focus: applies live, menu open or not
    if (!hubqm::dolphin_needs_apply(g_dolphin, g_speed_idx)) { g_retry_n = 0; return; }
    if (GetTickCount64() < g_retry_at) return;
    g_burst_plan = hubqm::dolphin_plan_for(g_dolphin, g_speed_idx);
    g_burst_target = g_speed_idx;
    std::vector<int> vks;
    for (auto k : g_burst_plan) vks.push_back(hubqm::dolphin_scan(hubqm::dolphin_vk(k)));
    if (vks.empty()) { hubqm::dolphin_commit(g_dolphin, g_burst_target, g_burst_plan, 0); return; }
    qm_log("dolphin speed: sending " + std::to_string(vks.size()) + " presses toward step " + std::to_string(g_burst_target) + (g_dolphin.known ? "" : " (state unknown: via the 0.1x floor)") + " attempt " + std::to_string(g_retry_n));
    g_burst_state = 1;
    auto burst_done = [](int done) { g_burst_done = done; g_burst_state = 2; };
    g_burst_dsu = use_dsu();
    if (g_burst_dsu) dolphin_speed_send(vks, burst_done); else dolphin_send_speed_keys(vks, burst_done);   // DSU pad: no focus / SendInput; keys only if its port was busy
}

/* UI thread, every frame: fold a finished burst into the state and schedule a retry if it did not go through. */
void dolphin_burst_poll() {
    if (g_burst_state.load() != 2) return;
    const int done = g_burst_done.load();
    g_burst_state = 0;
    if (done < 0 || (done == 0 && !g_burst_plan.empty())) {   // nothing was pressed: state unchanged
        if (g_burst_dsu && !g_dsu_dead) { g_dsu_dead = true; g_retry_at = 0; qm_log("dolphin speed: Dolphin's DSU client is not connected; using the keyboard hotkeys for this session"); return; }
        qm_log("dolphin speed: burst did not run (game not in front); will retry");
    } else {
        hubqm::dolphin_commit(g_dolphin, g_burst_target, g_burst_plan, done);
        if (done >= (int)g_burst_plan.size()) { qm_log("dolphin speed: now step " + std::to_string(g_dolphin.cur)); g_retry_n = 0; return; }
        qm_log("dolphin speed: burst cut short at " + std::to_string(done) + "/" + std::to_string(g_burst_plan.size()) + "; state now unknown, will redo");
    }
    if (g_retry_n >= hubqm::kDolphinMaxRetries) {
        g_retry_at = ~0ull;
        push_toast("Could not change Dolphin's speed. Hold View + LB for 1 s to reset it to 1x.", COL_WARN, 8.0);
        qm_log("dolphin speed: giving up after " + std::to_string(g_retry_n) + " retries");
    } else g_retry_at = GetTickCount64() + (ULONGLONG)hubqm::dolphin_retry_delay_ms(g_retry_n++);
}

void ext_apply(const hub::GameEntry* e) {
    int base = is_dolphin(e) ? g_launch_vol : 100;
    g_ext_sessions = extmedia::set_audio(game_pid_list(), hubqm::ext_session_scalar(g_vol, base), g_mute);
    if (g_ext_sessions > 0) g_ext_dirty = false;
}

/* A stepper row: label, "< value >", optional fill bar. Returns -1 / 0 / +1: Left / Right on the pad or keyboard while focused,
 * a click on the left or right half with the mouse, or A on the pad (+1; the Sound row treats any of them as a toggle). */
int stepper_row(const char* id, const char* label, const std::string& value, float fill, bool enabled, float width) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = sp(5.2f);
    const hubqm::NavAction na = g_nav.add(id, true, enabled, 0, g_row_y++);
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton("##step", ImVec2(width, h));
    ImGui::EndDisabled();
    bool hovered = ImGui::IsItemHovered(), focused = g_nav.selected(id);
    bool hot = enabled && (hovered || focused);
    int dir = 0;
    if (enabled) {
        if (clicked) dir = ImGui::GetIO().MousePos.x < p.x + width * 0.5f ? -1 : 1;
        if (na.step) { dir = na.step; qm_log(std::string("menu: step ") + id + (dir < 0 ? " -1" : " +1")); }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + width, p.y + h);
    dl->AddRectFilled(p, q, hot ? ui::PANEL2 : ui::PANEL, sp(1.25f));
    dl->AddRect(p, q, IM_COL32(255, 255, 255, 22), sp(1.25f));
    if (fill >= 0.f) {
        float bx0 = p.x + sp(1.2f), bx1 = q.x - sp(1.2f), by = q.y - sp(0.9f);
        dl->AddRectFilled(ImVec2(bx0, by - sp(0.35f)), ImVec2(bx1, by), IM_COL32(255, 255, 255, 28), sp(0.3f));
        if (fill > 0.f) dl->AddRectFilled(ImVec2(bx0, by - sp(0.35f)), ImVec2(bx0 + (bx1 - bx0) * std::min(1.f, fill), by), enabled ? ui::ACCENT : ui::TEXT3, sp(0.3f));
    }
    ImU32 tc = enabled ? ui::TEXT : ui::TEXT3;
    ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + sp(1.4f), p.y + (h - ls.y) * 0.5f - (fill >= 0.f ? sp(0.3f) : 0.f)), tc, label);
    ImGui::PushFont(ui::F.bold);
    ImVec2 vs = ImGui::CalcTextSize(value.c_str());
    ImGui::PopFont();
    float vx = q.x - sp(4.2f) - vs.x;
    float vy = p.y + (h - vs.y) * 0.5f - (fill >= 0.f ? sp(0.3f) : 0.f);
    dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(vx, vy), tc, value.c_str());
    if (enabled) {
        ui::icon(dl, ui::Icon::ChevronL, ImVec2(vx - sp(1.6f), vy + vs.y * 0.5f), sp(1.7f), hot ? ui::TEXT : ui::TEXT2);
        ui::icon(dl, ui::Icon::ChevronR, ImVec2(q.x - sp(1.9f), vy + vs.y * 0.5f), sp(1.7f), hot ? ui::TEXT : ui::TEXT2);
    }
    if (hot) dl->AddRect(p, q, ui::ACCENT, sp(1.25f), 0, 2.f);
    if (focused) nav_ring(p, q);
    ImGui::PopID();
    return dir;
}

/* ---- chat (room + friend messages on the runner OSD, quick phrases, OSK) and "How do I play" ---- */
std::string g_dm_uid, g_dm_name;   // last friend who messaged us this session: the "Send to" target besides the room
char g_chat_buf[256] = {0};
bool g_chat_to_dm = false;
double g_chat_sent_at = -100;
hubqm::ChatOsdLimiter g_chat_lim;
hubctl::Profile g_pad_prof;
bool g_pad_loaded = false;
std::string g_pad_note;
bool g_perf_shot = false;            // HUB_SHOT_QM stand-in value of the stats overlay row
int g_perf_sent = 2;                 // automatic "overlay on" sends left at session start (0, 1 pending; 2 = done)
ULONGLONG g_perf_t0 = 0;

bool runner_supports_overlay(const hub::GameEntry* e) { return e && runner_has_ctl(*e) && e->system != hub::System::N64; }

void send_osd(const std::string& text) {
    if (!A.proc || !ctl_ready() || text.empty()) return;
    runctl::Command c;
    c.cmd = runctl::Cmd::OsdMsg;
    c.text = text;
    enqueue_cmd(c);
}

void pad_load(const hub::GameEntry* e) {
    g_pad_loaded = true;
    g_pad_note.clear();
    std::string sys = g_shot_fake ? std::string("n64") : (e ? hubctl::console_key(*e) : std::string());
    hubctl::Store st;
    hubctl::load_store(hub::data_dir(), hub::saves::vault_dir(), &st);
    g_pad_prof = hubctl::resolve(st, sys, g_shot_fake ? std::string() : g_key);
    if (!g_shot_fake && e && is_native(*e) && g_pad_prof.name.compare(0, 8, "default-") == 0)
        g_pad_note = "Smash 64 has its own default layout. Change it on the Controllers page.";
}

struct PadSlot { const char* key; const char* tag; float u, v; };
const PadSlot kPadL[8] = {{"lt", "LT", -0.6f, -1.f}, {"lb", "LB", -0.6f, -0.8f}, {"back", "View", -0.15f, -0.3f}, {"ls", "Left stick", -0.6f, -0.2f},
                          {"dpup", "D-pad up", -0.2f, 0.25f}, {"dpleft", "D-pad left", -0.35f, 0.45f}, {"dpright", "D-pad right", -0.05f, 0.45f}, {"dpdown", "D-pad down", -0.2f, 0.65f}};
const PadSlot kPadR[8] = {{"rt", "RT", 0.6f, -1.f}, {"rb", "RB", 0.6f, -0.8f}, {"start", "Menu", 0.15f, -0.3f}, {"y", "Y", 0.6f, -0.4f},
                          {"x", "X", 0.45f, -0.2f}, {"b", "B", 0.75f, -0.2f}, {"a", "A", 0.6f, 0.f}, {"rs", "Right stick", 0.2f, 0.45f}};

/* A button of the menu's own nav grid (x = column, y = row): A on it or a click presses it. */
bool nav_btn(const std::string& id, const char* label, ImVec2 sz, bool primary, bool en, int nx, int ny, bool back_icon = false) {
    const hubqm::NavAction a = g_nav.add(id, false, en, nx, ny);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id.c_str());
    const bool pressed = back_icon ? ui::button(label, sz, primary, en, ui::Icon::ChevronL) : ui::button(label, sz, primary, en);
    ImGui::PopID();
    if (g_nav.selected(id)) nav_ring(p, p + sz);
    return en && (pressed || a.activate);
}

void pad_page() {
    const hub::GameEntry* e = g_shot_fake ? nullptr : cur_entry(g_key);
    if (!g_pad_loaded) pad_load(e);
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("How do I play");
    ImGui::PopFont();
    ImGui::TextDisabled("%s - controller layout from your profile", g_title.c_str());
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    auto co = hubqm::pad_callouts(g_pad_prof);
    const float W = ImGui::GetContentRegionAvail().x, rowH = sp(5.f), H = rowH * 8.f, pw = sp(40.f), colW = (W - pw - sp(4.f)) * 0.5f;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(W, H));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(o.x + W * 0.5f, o.y + H * 0.5f);
    const float ph = H * 0.8f;
    dl->AddRectFilled(ImVec2(c.x - pw * 0.5f, c.y - ph * 0.5f), ImVec2(c.x + pw * 0.5f, c.y + ph * 0.5f), ui::PANEL, sp(6.f));
    dl->AddRect(ImVec2(c.x - pw * 0.5f, c.y - ph * 0.5f), ImVec2(c.x + pw * 0.5f, c.y + ph * 0.5f), IM_COL32(255, 255, 255, 40), sp(6.f));
    for (int side = 0; side < 2; ++side) {
        const PadSlot* sl = side == 0 ? kPadL : kPadR;
        for (int i = 0; i < 8; ++i) {
            auto it = co.find(sl[i].key);
            const bool used = it != co.end();
            const float y = o.y + (i + 0.5f) * rowH;
            const float x0 = side == 0 ? o.x : o.x + W - colW;
            const ImVec2 bp(c.x + sl[i].u * pw * 0.5f, c.y + sl[i].v * ph * 0.5f);
            const ImVec2 from(side == 0 ? x0 + colW + sp(0.5f) : x0 - sp(0.5f), y);
            dl->AddLine(from, bp, used ? IM_COL32(120, 170, 255, 150) : IM_COL32(255, 255, 255, 22), used ? 1.6f : 1.f);
            dl->AddCircleFilled(bp, sp(1.4f), used ? ui::ACCENT : ui::PANEL2);
            if (!used) dl->AddCircle(bp, sp(1.4f), IM_COL32(255, 255, 255, 40));
            ImVec4 clip(x0, y - rowH * 0.5f, x0 + colW, y + rowH * 0.5f);
            dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(x0, y - rowH * 0.45f), ui::TEXT3, hubglyph::label(ui::pad_kind, sl[i].tag), nullptr, 0.f, &clip);
            std::string lab = used ? hubqm::join_labels(it->second, 2) : std::string("-");
            dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(x0, y - rowH * 0.45f + ui::F.cap->FontSize * 1.2f), used ? ui::TEXT : ui::TEXT3, lab.c_str(), nullptr, 0.f, &clip);
        }
    }
    if (!g_pad_note.empty()) text_wrapped_disabled(g_pad_note);
    const std::string sys = g_pad_prof.system;
    if (hubctl::has_keyboard(sys)) {
        std::string k;
        for (const auto& ci : hubctl::key_controls_for(sys)) {
            auto it = g_pad_prof.keys.find(ci.name);
            if (it == g_pad_prof.keys.end()) continue;
            k += (k.empty() ? "" : "   ") + ci.label + " = " + hubctl::describe_keys(it->second);
        }
        if (!k.empty()) text_wrapped_disabled("Keyboard: " + k);
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (nav_btn("pad_back", "Back", ImVec2(sp(14.f), sp(5.5f)), false, true, 0, g_row_y++, true)) { g_nav.sel = "How do I play"; g_page = QPage::Main; }
}

void chat_send(const std::string& t, bool dm) {
    if (!osk::chat_may_send(t, false)) return;
    if (g_shot_fake) { g_chat_sent_at = ImGui::GetTime(); return; }
    if (dm) { if (!fr_send_dm(g_dm_uid, t)) return; }
    else {
        A.lobby.send_msg("{\"kind\":\"chat\",\"text\":\"" + json::escape(t) + "\"}");
        A.chat.push_back({A.st.name, t});
        A.chat_scroll = true;
    }
    send_osd(hubqm::chat_osd_text("You", t));
    g_chat_sent_at = ImGui::GetTime();
}

void chat_page() {
    const bool room = g_shot_fake || fr_in_my_room(), dm = !g_dm_uid.empty();
    if (!room && dm) g_chat_to_dm = true;
    if (room && !dm) g_chat_to_dm = false;
    const bool to_dm = g_chat_to_dm && dm;
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Chat");
    ImGui::PopFont();
    ImGui::TextDisabled(room || dm ? "Pick a quick phrase or type a message." : "Join a room to chat from here.");
    ImGui::Dummy(ImVec2(0, sp(0.75f)));
    const float fw = ImGui::GetContentRegionAvail().x;
    static const char* kModes[] = {"Off", "Room only", "Room + friends"};
    if (int d = stepper_row("gchat", "Chat on screen", kModes[std::max(0, std::min(2, A.st.game_chat))], -1.f, true, fw)) {
        A.st.game_chat = (A.st.game_chat + (d > 0 ? 1 : 2)) % 3;
        mark_dirty();
    }
    if (room && dm) {
        if (stepper_row("gchat_to", "Send to", g_chat_to_dm ? g_dm_name : std::string("Room"), -1.f, true, fw)) g_chat_to_dm = !g_chat_to_dm;
    } else if (to_dm) ImGui::TextDisabled("Sending to %s", g_dm_name.c_str());
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    const auto& ph = hubqm::quick_phrases();
    const float gap = sp(0.75f), bw = (fw - gap * (ph.size() - 1)) / ph.size();
    for (size_t i = 0; i < ph.size(); ++i) {
        if (i) ImGui::SameLine(0, gap);
        if (nav_btn("phrase" + std::to_string(i), ph[i].c_str(), ImVec2(bw, sp(6.f)), false, room || dm, (int)i, g_row_y)) chat_send(ph[i], to_dm);
    }
    ++g_row_y;
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    const hubqm::NavAction ta = g_nav.add("chat_type", false, room || dm, 0, g_row_y);   // A on it opens the on-screen keyboard (ImGui nav drives it while it is open)
    if (ta.activate) osk::request_open();
    ImVec2 tp = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemWidth(fw - sp(10.f));
    bool enter = osk::input("##qmchat", "Type a message", g_chat_buf, sizeof g_chat_buf, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, gap);
    if (g_nav.selected("chat_type")) nav_ring(tp, tp + ImVec2(fw - sp(10.f), ImGui::GetFrameHeight()));
    bool send_btn = nav_btn("chat_send", "Send", ImVec2(sp(9.f), ImGui::GetFrameHeight()), true, g_chat_buf[0] != 0 && (room || dm), 1, g_row_y++);
    if ((enter || send_btn) && g_chat_buf[0] && (room || dm)) {
        std::string t = g_chat_buf;
        g_chat_buf[0] = 0;
        chat_send(t, to_dm);
    }
    if (ImGui::GetTime() - g_chat_sent_at < 2.0) ImGui::TextDisabled("Sent.");
    else ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
    if (nav_btn("chat_back", "Back", ImVec2(sp(14.f), sp(5.5f)), false, true, 0, g_row_y++, true)) { g_nav.sel = "Chat"; g_page = QPage::Main; }
}

/* ---- Plan 4 P5: emote wheel ---- */
void emote_send(const hubqm::Emote& em) {
    if (!g_emote_lim.allow((long long)GetTickCount64())) { g_emote_note_at = ImGui::GetTime(); return; }
    if (!g_shot_fake) {
        A.lobby.send_msg(hubqm::emote_json(em.code));
        send_osd(hubqm::chat_osd_text("You", em.label));
    }
    push_toast(std::string("Sent ") + em.label, COL_GOOD, 2.0);
    g_close_why = "Emote";
    ImGui::CloseCurrentPopup();
    if (!g_shot_fake) qm_focus_game();
}

void emote_page() {
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Emotes");
    ImGui::PopFont();
    ImGui::TextDisabled("Shows on everyone's screen in the room. One every 2 seconds.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    const auto& em = hubqm::emotes();
    const float fw = ImGui::GetContentRegionAvail().x, gap = sp(0.75f), bw = (fw - gap * (em.size() - 1)) / em.size();
    for (size_t i = 0; i < em.size(); ++i) {
        if (i) ImGui::SameLine(0, gap);
        if (nav_btn(std::string("emote_") + em[i].code, em[i].label, ImVec2(bw, sp(8.f)), false, true, (int)i, 0)) emote_send(em[i]);
    }
    if (ImGui::GetTime() - g_emote_note_at < 2.0) ImGui::TextDisabled("Easy there: one emote every 2 seconds.");
    else ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
    g_row_y = 1;
    if (nav_btn("emote_back", "Back", ImVec2(sp(14.f), sp(5.5f)), false, true, 0, g_row_y++, true)) { g_nav.sel = "Emotes"; g_page = QPage::Main; }
}

/* ---- Plan 4 P4: pass the controller ---- */
struct PassTarget { int slot; std::string name; };
std::vector<PassTarget> pass_targets() {
    std::vector<PassTarget> v;
    if (g_shot_fake) { v.push_back({1, "Mickey"}); v.push_back({2, "Mia"}); return v; }
    for (const auto& m : room_members()) if (m.slot != room_my_slot()) v.push_back({m.slot, m.name});
    return v;
}
int my_port() {
    if (g_shot_fake) return 0;
    return hubqm::port_of_slot(hubui::build_ports(member_pads()).ports, room_my_slot());
}

void pass_to(const PassTarget& t) {
    const int port = my_port();
    if (port < 0) { push_toast("You don't have a controller to pass.", COL_WARN, 5.0); return; }
    qm_log("menu: pass port " + std::to_string(port) + " to slot " + std::to_string(t.slot));
    if (g_shot_fake) { push_toast("Passing to " + t.name, COL_GOOD, 3.0); return; }
    runctl::Command c;
    c.cmd = runctl::Cmd::Handoff; c.slot = port; c.value = t.slot;
    g_ho_wait = true; g_ho_seq = 0; g_ho_name = t.name; g_ho_at = ImGui::GetTime();
    enqueue_cmd(c, 1);
}

void pass_page() {
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Pass controller");
    ImGui::PopFont();
    ImGui::TextDisabled("Hand your controller to another player. The game keeps running.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    const auto tg = pass_targets();
    if (tg.empty()) ImGui::TextDisabled("Nobody else is in the room.");
    const float w = ImGui::GetContentRegionAvail().x;
    for (const auto& t : tg)
        if (nav_btn("pass_" + std::to_string(t.slot), ("Pass controller to " + t.name).c_str(), ImVec2(w, sp(6.5f)), false, !g_ho_wait && g_pending == 0, 0, g_row_y++)) pass_to(t);
    if (g_ho_wait) ImGui::TextDisabled("Asking the game...");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (nav_btn("pass_back", "Back", ImVec2(sp(14.f), sp(5.5f)), false, true, 0, g_row_y++, true)) { g_nav.sel = "Pass controller"; g_page = QPage::Main; }
}

void check_ack() {
    if (!g_pending) return;
    std::string txt;
    runctl::Ack a;
    if (runctl::read_text(runctl::ack_path_for(A.run.ctl_path), &txt) && runctl::parse_ack(txt, &a) && a.seq == g_pending) {
        g_pending = 0;
        if (a.ok) {
            push_toast(g_pending_what.empty() ? a.msg : g_pending_what, COL_GOOD, 3.0);
            if (g_pending_close) { g_page = QPage::Main; g_close_why = "slot action"; ImGui::CloseCurrentPopup(); qm_focus_game(); }
        } else {
            if (g_hide.on_fail(g_pending_cmd, a.msg)) { qm_log("row hidden: the runner answered \"" + a.msg + "\""); push_toast("That is not available for this game.", COL_WARN, 4.0); }
            else push_toast(a.msg.empty() ? std::string("That did not work.") : a.msg, COL_WARN, 5.0);
        }
        g_pending_cmd = runctl::Cmd::None;
    } else if (ImGui::GetTime() - g_pending_at > 20.0) {
        g_pending = 0;
        g_pending_cmd = runctl::Cmd::None;
        push_toast("The game did not answer.", COL_WARN, 5.0);
    }
}

/* qm_tick, every frame: the answer to a queued hand-off, and the countdown on the game's OSD. */
void handoff_tick() {
    if (g_ho_wait) {
        std::string txt;
        runctl::Ack a;
        if (g_ho_seq > 0 && !A.run.ctl_path.empty() && runctl::read_text(runctl::ack_path_for(A.run.ctl_path), &txt) && runctl::parse_ack(txt, &a) && a.seq == g_ho_seq) {
            g_ho_wait = false;
            qm_log(std::string("handoff ack ok=") + (a.ok ? "1" : "0") + " msg=" + a.msg);
            if (a.ok) {
                g_cd_name = g_ho_name; g_cd_t0 = GetTickCount64(); g_cd_shown = 0;
                if (g_qm_showing.load()) { g_close_req_why = "controller passed"; g_close_req = true; }
            } else push_toast(hubqm::handoff_fail_text(a.msg), COL_WARN, 6.0);
        } else if (ImGui::GetTime() - g_ho_at > 8.0) { g_ho_wait = false; push_toast("The game did not answer.", COL_WARN, 5.0); }
    }
    if (!g_cd_name.empty()) {
        if (!A.proc) { g_cd_name.clear(); return; }
        const ULONGLONG el = GetTickCount64() - g_cd_t0;
        const int n = (int)std::min<ULONGLONG>(3, el / 1000 + 1);
        if (n > g_cd_shown) { g_cd_shown = n; send_osd(hubqm::countdown_text(g_cd_name, n)); }
        if (el >= 3000) g_cd_name.clear();
    }
}

}  // namespace

/* ---------------- public ---------------- */
void qm_send_osd(const std::string& text) { send_osd(text); }

void qm_init(SDL_Renderer* ren) { g_ren = ren; }
void qm_textures_begin_frame() { g_tex.begin_frame(); g_shot_budget.begin_frame(); }

static std::string run_dir_for(const std::string& key) {
    std::string k = key;
    for (auto& c : k) if (!isalnum((unsigned char)c) && c != '-' && c != '_') c = '_';
    return trim_sep(hub::data_dir()) + "\\run\\" + k;
}

/* Built-in runners that understand --control. */
static bool runner_has_ctl(const hub::GameEntry& e) {
    if (is_native(e)) return false;
    switch (e.system) {
        case hub::System::N64: case hub::System::GBA: case hub::System::GB: case hub::System::GBC: case hub::System::NDS: case hub::System::PS1: case hub::System::NES: case hub::System::SNES: return true;
        default: return false;
    }
}

void qm_prepare(RunInfo& ri, const hub::GameEntry& e) {
    if (!runner_has_ctl(e)) return;
    std::string dir = run_dir_for(e.key);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    ri.ctl_path = dir + "\\control.json";
    std::filesystem::remove(std::filesystem::u8path(runctl::ack_path_for(ri.ctl_path)), ec);
    runctl::write_text_atomic(ri.ctl_path, "{\"seq\":0}");
    ri.shot_dir = trim_sep(hub::saves::game_dir(sys_short(e), e.key)) + "\\shots";
}

std::string qm_args(const RunInfo& ri, const hub::GameEntry& e, bool n64_state_dir) {
    if (ri.ctl_path.empty()) return "";
    std::string a = " --control " + quote(ri.ctl_path) + " --game-key " + quote(e.key) + " --shot-dir " + quote(ri.shot_dir);
    if (n64_state_dir) a += " --state-dir " + quote(states_dir_of(e.key, sys_short(e)));
    return a;
}

void qm_log(const std::string& line) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    static std::string dir = hub::data_dir() + "logs\\";
    static std::string path = dir + "quickmenu.log";
    CreateDirectoryA(dir.c_str(), nullptr);
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeHigh == 0 && fad.nFileSizeLow > 500000)
        MoveFileExA(path.c_str(), (path + ".1").c_str(), MOVEFILE_REPLACE_EXISTING);
    SYSTEMTIME st; GetLocalTime(&st);
    char ts[48];
    std::snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d.%03d ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::ofstream f(path, std::ios::app | std::ios::binary);
    f << ts << line << "\n";
}

void qm_dolphin_reset() {
    const hub::GameEntry* e = A.proc ? find_entry(A.run.key) : nullptr;
    if (!is_dolphin(e) || A.run.online) return;
    g_speed_idx = hubqm::kSpeedNormal;
    g_close_req_why = "reset requested";
    g_dolphin.known = false;   // do not trust what we think Dolphin runs at: go down to its floor, then up to 1x
    g_retry_n = 0; g_retry_at = 0;
    g_close_req = g_qm_showing.load();
    push_toast("Resetting Dolphin to 1x...", COL_GOOD, 4.0);
    qm_log("emergency reset to 1x requested");
}
/* A room or friend message arrived: show it on the running game's OSD (runner OSD only, nothing over the game) when the setting allows. */
void qm_chat_incoming(bool dm, const std::string& uid, const std::string& who, const std::string& text) {
    if (dm) { g_dm_uid = uid; g_dm_name = who; }
    if (!A.proc || !ctl_ready() || !hubqm::chat_osd_wanted(A.st.game_chat, dm)) return;
    std::string t = hubqm::chat_osd_text(dm ? who + " (DM)" : who, text);
    if (t.empty() || !g_chat_lim.allow((long long)GetTickCount64())) return;
    send_osd(t);
}
bool qm_is_open() { return g_qm_showing.load(); }   // thread-safe; set at the request, refreshed every draw


void qm_open_live() {
    const unsigned long gcp = gc_pid();
    if (!qm_game_live() || (A.run.key.empty() && !gcp)) return;
    if (g_qm_showing.load()) { qm_log("open request ignored: menu already open"); return; }   // View+Y never re-opens / toggles an open menu
    g_retry_n = 0; g_retry_at = 0;   // a Dolphin speed change that gave up is tried again once the menu closes
    prefs_load();
    g_activity_ms = GetTickCount64();
    qm_log("open request (hub fg=" + std::string(hub_is_foreground() ? "yes" : "no") + " fg=" + qm_describe_window(GetForegroundWindow()) + ")");
    const std::string run_key = gcp ? GCO.entry.key : A.run.key;
    const hub::GameEntry* e = g_e2e ? nullptr : cur_entry(run_key);
    g_sys = e ? sys_short(*e) : gcp ? std::string("gc") : A.run.system_short;
    g_key = run_key;
    g_title = e ? e->title : gcp ? GCO.entry.title : A.run.title;
    g_page = QPage::Main;
    g_pending = 0;
    g_open_req = true;
    g_qm_showing = true;   // pad is exempt from the focus gate from this moment
    focus_hub();
}

static bool runner_has_ctl(const hub::GameEntry& e);   // below

/* One game session = one (key, start tick): speed back to 1x, mute off, volume from the settings. External emulators get their
 * Windows audio session scaled (re-applied every 2 s until it exists, and whenever it is not at full volume). */
static void qm_session_tick(const hub::GameEntry* e) {
    const unsigned long gcp = gc_pid();
    const std::string& run_key = gcp ? GCO.entry.key : A.run.key;
    if ((!A.proc && !gcp) || run_key.empty()) { g_sess_key.clear(); return; }
    const ULONGLONG run_tick = gcp ? (ULONGLONG)gcp : A.run.start_tick;   // a new NetPlay Dolphin = a new session
    if (g_sess_key != run_key || g_sess_tick != run_tick) {
        g_sess_key = run_key; g_sess_tick = run_tick;
        g_dol_queue.clear(); g_dol_paused = false;
        g_speed_idx = hubqm::kSpeedNormal;
        g_dolphin = hubqm::DolphinSpeed();
        g_retry_n = 0; g_retry_at = 0; g_burst_state = 0; g_dsu_dead = false;
        qm_log("session start " + run_key);   // Dolphin.ini was written with EmulationSpeed = 1.0 at launch
        g_mute = false;
        g_vol = e ? hub::gs::volume_of(gs_values_for(*e)) : 100;
        g_launch_vol = g_vol;
        g_ext_sessions = -1;
        g_ext_dirty = true;
        g_dm_uid.clear(); g_dm_name.clear(); g_chat_buf[0] = 0; g_chat_to_dm = false;
        g_perf_sent = 0; g_perf_t0 = GetTickCount64();
        g_hide.reset(); g_ovw.cancel(); g_pending_cmd = runctl::Cmd::None;
        g_cmdq.clear(); g_pacer = hubqm::CmdPacer(); g_pause_manual = false; g_pause_sent = false; g_ho_wait = false; g_ho_seq = 0; g_cd_name.clear();
    }
    if (g_perf_sent < 2 && e && A.st.perf_overlay && ctl_ready() && runner_supports_overlay(e)) {   // the runner ignores commands written before it started polling: send at +4 s and again at +12 s
        const ULONGLONG dt = GetTickCount64() - g_perf_t0;
        if (dt >= (g_perf_sent == 0 ? 4000u : 12000u)) { runctl::Command c; c.cmd = runctl::Cmd::Overlay; c.value = 1; send_quiet(c); ++g_perf_sent; }
    }
    if (e && !is_native(*e) && !runner_has_ctl(*e)) {
        double now = ImGui::GetTime();
        bool need = g_ext_dirty || g_mute || hubqm::ext_session_scalar(g_vol, is_dolphin(e) ? g_launch_vol : 100) < 1.f;
        if (need && now >= g_ext_next) { g_ext_next = now + 2.0; ext_apply(e); }
    }
}

/* GameCube NetPlay (no A.proc, so launch.cpp's pad watches do not run for it): View+Y opens the game menu, Back+Start held leaves.
 * Pads are read directly (qm_pad.cpp), so this works with the game in front. Only View+Y may bring the hub forward over the game. */
static void gc_pad_watch() {
    static hubqm::OpenGate gate;
    static hubctl::ExitCombo leave;
    const unsigned long gp = gc_pid();
    if (!gp || g_headless) { leave.reset(); return; }
    bool open_down = false, leave_down = false;
    for (const uint32_t m : qm_pad_each()) {   // per pad: one player's View with another's Y is not the combo
        auto has = [&](int b) { return ((m >> b) & 1u) != 0; };
        if (has(SDL_CONTROLLER_BUTTON_BACK) && has(SDL_CONTROLLER_BUTTON_Y)) open_down = true;
        if (has(SDL_CONTROLLER_BUTTON_BACK) && has(SDL_CONTROLLER_BUTTON_START)) leave_down = true;
    }
    const long long now = (long long)GetTickCount64();
    if (gate.update(open_down, qm_is_open(), now)) { qm_log("gc netplay pad combo View+Y: open"); qm_open_live(); }
    if (leave.update(leave_down, now)) gc_leave("Back + Start held");
}

void qm_tick() {
    {   // Plan 4 p4-social: friend challenges (challenge_ui.cpp): host callbacks into the command queue, then the per-frame tick
        static bool host_set = false;
        if (!host_set) {
            host_set = true;
            hubui::ChallengeHost h;
            h.save_state = [](int slot) { runctl::Command c; c.cmd = runctl::Cmd::SaveState; c.slot = slot; enqueue_cmd(c); };
            h.corner = [](int corner, const std::string& t) { runctl::Command c; c.cmd = runctl::Cmd::OsdCorner; c.slot = corner; c.text = t; enqueue_cmd(c); };
            h.osd = [](const std::string& t) { runctl::Command c; c.cmd = runctl::Cmd::OsdMsg; c.text = t; enqueue_cmd(c); };
            hubui::challenge_set_host(std::move(h));
        }
        hubui::challenge_tick(g_qm_showing.load());
    }
    {   // Plan 4: pause while the menu is open (offline only), resume on close; the queue writes one command at a time
        const bool menu = g_qm_showing.load() && !g_shot_fake;
        const bool online_now = qm_online();
        const bool can = qm_game_live() && ctl_ready();
        const bool want = menu && can && hubqm::pause_available(true, online_now) && g_pause_manual;
        if (want != g_pause_sent) { g_pause_sent = want; runctl::Command c; c.cmd = runctl::Cmd::Pause; c.value = want ? 1 : 0; send_quiet(c); }
        if (!menu && g_pause_manual) g_pause_manual = false;
        if (!g_e2e) { cmdq_pump(); handoff_tick(); }
    }
    {   // runner input gate: "input off" while the menu is open (refreshed every 10 s: the runner re-enables itself after 30 s), "input on" on close
        static bool sent_off = false;
        static double next_refresh = 0;
        const bool want_off = g_qm_showing.load() && !g_shot_fake && qm_game_live() && ctl_ready();
        const double now = ImGui::GetTime();
        if (want_off && (!sent_off || now >= next_refresh)) { runctl::Command c; c.cmd = runctl::Cmd::Input; c.value = 0; send_quiet(c); sent_off = true; next_refresh = now + 10.0; }
        else if (!want_off && sent_off) { sent_off = false; if (qm_game_live() && ctl_ready()) { runctl::Command c; c.cmd = runctl::Cmd::Input; c.value = 1; send_quiet(c); } }
    }
    hub::saves::set_backup_screenshots(A.st.backup_shots);
    if (A.proc && !g_shot_fake) {   // evidence: every foreground change near a menu event
        static HWND last_fg = nullptr;
        HWND fg = GetForegroundWindow();
        const ULONGLONG tn = GetTickCount64();
        if (fg != last_fg) {
            if (g_qm_showing.load() || tn - g_activity_ms < 30000) qm_log("foreground -> " + qm_describe_window(fg) + (g_qm_showing.load() ? " (menu open)" : ""));
            last_fg = fg;
        }
        if (!g_qm_showing.load()) g_close_req = false;
        dolphin_burst_poll();
        dolphin_apply_pending(find_entry(A.run.key));
    }
    if (A.proc) qm_session_tick(find_entry(A.run.key));
    else if (gc_pid()) qm_session_tick(cur_entry(GCO.entry.key));
    gc_pad_watch();
    {   // finished external-emulator window capture
        std::string m;
        int st = extmedia::capture_poll(&m);
        if (st == 2) push_toast("Screenshot saved", COL_GOOD, 3.0);
        else if (st == 3) push_toast(m.empty() ? std::string("Could not capture the game window.") : m, COL_WARN, 5.0);
    }
    static const char* shot = std::getenv("HUB_SHOT_QM");   // screenshot aid: main | save | load (opens on a stand-in running game)
    static bool done = false;
    if (shot && !done && g_headless) {
        done = true;
        g_shot_fake = true;
        g_shot_online = std::getenv("HUB_SHOT_QM_ONLINE") != nullptr;
        g_shot_ext = std::getenv("HUB_SHOT_QM_EXT") != nullptr;
        g_shot_dolphin = std::getenv("HUB_SHOT_QM_DOLPHIN") != nullptr;   // with HUB_SHOT_QM_EXT: the external emulator is Dolphin
        g_vol = std::getenv("HUB_SHOT_QM_VOL") ? std::atoi(std::getenv("HUB_SHOT_QM_VOL")) : 100;
        g_speed_idx = std::getenv("HUB_SHOT_QM_SPEED") ? std::atoi(std::getenv("HUB_SHOT_QM_SPEED")) : hubqm::kSpeedNormal;
        g_mute = std::getenv("HUB_SHOT_QM_MUTE") != nullptr;
        g_sys = "n64"; g_key = "n64:shot"; g_title = "Super Smash Bros.";
        g_page = std::string(shot) == "save" ? QPage::Save : std::string(shot) == "load" ? QPage::Load : std::string(shot) == "pad" ? QPage::Pad : std::string(shot) == "chat" ? QPage::Chat : std::string(shot) == "emote" ? QPage::Emote : std::string(shot) == "pass" ? QPage::Pass : std::string(shot) == "challenge" ? QPage::Challenge : QPage::Main;
        if (std::string(shot) == "challenge") { g_ch_fake = true; g_ch_fake_sel = {"u1"}; }
        g_perf_shot = std::getenv("HUB_SHOT_QM_PERF") != nullptr;
        g_open_req = true;
        std::string dir = states_dir_of(g_key, g_sys);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
        for (int i : {0, 2, 3, 5}) {   // a few used slots with synthetic previews (BMP is enough for the loader)
            std::string base = runctl::state_file(dir, i);
            std::ofstream(std::filesystem::u8path(base), std::ios::binary) << "x";
            runctl::Sidecar sc; sc.game_key = g_key; sc.time = (long long)std::time(nullptr) - 3600 * (i + 1); sc.playtime = 600;
            runctl::write_text_atomic(runctl::sidecar_file(dir, i), runctl::make_sidecar(sc));
            SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, 320, 200, 32, SDL_PIXELFORMAT_ARGB8888);
            if (s) {
                SDL_FillRect(s, nullptr, SDL_MapRGB(s->format, (Uint8)(40 + i * 30), (Uint8)(90 + i * 20), (Uint8)(200 - i * 25)));
                SDL_Rect r{20 + i * 25, 60, 80, 90};
                SDL_FillRect(s, &r, SDL_MapRGB(s->format, 240, 200, 60));
                std::string b = runctl::thumb_file(dir, i);
                b.replace(b.size() - 4, 4, ".bmp");
                SDL_SaveBMP(s, b.c_str());
                SDL_FreeSurface(s);
            }
        }
    }
}

/* While the menu itself owns the input, ImGui's keyboard / gamepad navigation is off (main.cpp calls this before NewFrame), so
 * nothing in ImGui can activate a button from a pad press: the menu's own controls (qm_nav.h) are the only path. */
void qm_pre_newframe() {
    static bool saved = false;
    static ImGuiConfigFlags saved_flags = 0;
    constexpr ImGuiConfigFlags kNav = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGuiIO& io = ImGui::GetIO();
    const bool want_off = g_qm_showing.load() && !g_sub_open && !g_shot_fake;
    if (want_off && !saved) { saved_flags = io.ConfigFlags & kNav; io.ConfigFlags &= ~kNav; saved = true; }
    else if (!want_off && saved && (g_sub_open || qm_nav_bits(qm_pad_raw()) == 0)) {   // closed: wait until the closing press is up
        io.ConfigFlags |= saved_flags; saved = false;
    }
}

bool qm_game_live() { return A.proc != nullptr || g_e2e || gc_pid() != 0; }
static hubqm::FocusRetry g_focus_retry;

static void qm_body();

void draw_quick_menu() {
    qm_tick();
    hubui::challenge_shot_draw();
    if (g_open_req) { g_open_req = false; ImGui::OpenPopup("##quickmenu"); }
    ImGuiIO& io = ImGui::GetIO();
    float w = std::min(g_page == QPage::Main ? sp(52.f) : sp(104.f), io.DisplaySize.x * 0.94f);
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(2.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal("##quickmenu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    static bool was_open = false;
    const uint32_t raw = qm_pad_raw(), taps = qm_pad_take_taps();
    const uint32_t bits = qm_nav_bits(raw) | qm_keyboard_nav_bits(), tap_bits = qm_nav_bits(taps);
    const bool open_frame = open && !was_open;
    if (open && !was_open) {
        g_open_ms = GetTickCount64(); g_close_why = "other"; g_focus_retry.reset();
        g_in.open(bits | qm_keyboard_held_async());   // everything held right now (View+Y, A/B from the game, Enter...) is ignored until released
        g_nav.reset();
        g_sub_open = false;
        g_pause_manual = hubqm::auto_pause_wanted(g_pause_auto, true, g_shot_fake ? !g_shot_ext : ctl_ready(), g_shot_fake ? g_shot_online : qm_online());
        g_emote_note_at = -100;
        if (!g_shot_fake) qm_log("menu opened (held at open: 0x" + [](uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%x", v); return std::string(b); }(raw) + ")");
    }
    if (was_open && !open) {
        if (!g_shot_fake) qm_log(std::string("menu closed (") + g_close_why + ")");
        g_activity_ms = GetTickCount64();
        g_qm_showing = false;
        g_sub_open = false;
        hub_menu_topmost(false);
        if (qm_game_live() && !g_e2e) dolphin_apply_pending(find_entry(g_key));   // closed this frame (Resume / B / pad combo)
        {   // Dolphin menu hotkeys: the queued buttons, plus one Toggle Pause to carry on when it was paused
            std::vector<hubqm::DolphinAct> q;
            q.swap(g_dol_queue);
            if (!g_shot_fake && !g_e2e && qm_game_live() && std::string(g_close_why) != "Quit") dolphin_send_acts(hubqm::dolphin_close_plan(q, g_dol_paused.load()).acts);
        }
    }
    was_open = open;
    g_qm_showing = open && !g_shot_fake;
    if (!open) return;
    {   // the menu's own controls
        static bool sub_was = false;
        hubqm::NavEvents ev;
        if (g_sub_open) { sub_was = true; qm_pad_take_taps(); }
        else {
            if (sub_was) { sub_was = false; g_in.open(bits); }   // the picker just closed: its closing B must not also close the menu
            ev = g_in.update(bits, (long long)GetTickCount64(), open_frame ? 0u : tap_bits);
        }
        if (ev.back) qm_log("menu: back");
        g_nav.begin(ev);
        g_row_y = 0;
    }
    const QPage page_before = g_page;
    qm_body();
    {   // something opened over the menu (Invite friends picker): find the menu in the open-popup stack, look above it
        ImGuiContext& gc = *ImGui::GetCurrentContext();
        ImGuiWindow* me = ImGui::GetCurrentWindow();
        int at = -1;
        for (int i = 0; i < gc.OpenPopupStack.Size; ++i) if (gc.OpenPopupStack[i].Window == me) at = i;
        const bool sub = at >= 0 && at < gc.OpenPopupStack.Size - 1;
        if (sub != g_sub_open && !g_shot_fake) qm_log(sub ? "menu: a popup opened over the menu (ImGui nav drives it)" : "menu: popup over the menu closed");
        g_sub_open = sub;
    }
    if (g_page != page_before) { g_nav.rows.clear(); g_nav.building.clear(); g_nav.act = hubqm::NavAction(); }   // new page: its rows come next frame; keep the chosen sel
    else g_nav.end();
    ImGui::EndPopup();
}


/* ---- Plan 4 p4-social: "Challenge a friend from here" (state machine: challenge_flow.h, engine: challenge_ui.cpp) ---- */

void challenge_page() {
    using namespace hub::challenge;
    const bool finishing = !g_ch_fake && hubui::challenge_running();   // "I did it" on a score challenge: type the score
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted(finishing ? "Your score" : "Challenge a friend");
    ImGui::PopFont();
    const float fw = ImGui::GetContentRegionAvail().x;
    if (finishing) {
        ImGui::TextDisabled("Type the score you got.");
        const hubqm::NavAction ta = g_nav.add("ch_type", false, true, 0, g_row_y);
        if (ta.activate) osk::request_open();
        ImVec2 tp = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemWidth(fw - sp(12.f));
        osk::input("##chscore", "Score", g_ch_buf, sizeof g_ch_buf);
        if (g_nav.selected("ch_type")) nav_ring(tp, tp + ImVec2(fw - sp(12.f), ImGui::GetFrameHeight()));
        ImGui::SameLine();
        if (nav_btn("ch_post", "Send result", ImVec2(sp(11.f), ImGui::GetFrameHeight()), true, g_ch_buf[0] != 0, 1, g_row_y++)) {
            int64_t v = 0;
            if (parse_score(g_ch_buf, &v)) { hubui::challenge_run_finish(true, g_ch_buf); g_page = QPage::Main; g_close_why = "challenge done"; ImGui::CloseCurrentPopup(); qm_focus_game(); }
            else push_toast("Type your score as a number.", COL_WARN, 4.0);
        }
        if (nav_btn("ch_back", "Back", ImVec2(sp(14.f), sp(5.5f)), false, true, 0, g_row_y++, true)) { g_nav.sel = "I did it"; g_page = QPage::Main; }
        return;
    }
    const hubui::ChallengeSendView& v0 = hubui::challenge_send_view();
    const SendPhase ph = g_ch_fake ? g_ch_fake_phase : v0.phase;
    const std::string title = g_ch_fake ? g_title : v0.title;
    ImGui::TextDisabled("%s", title.c_str());
    ImGui::Dummy(ImVec2(0, sp(0.75f)));
    auto back_to_main = [&]() { if (!g_ch_fake) hubui::challenge_send_cancel(); g_nav.sel = "Challenge a friend from here"; g_page = QPage::Main; };
    switch (ph) {
    case SendPhase::Saving:
        ImGui::TextUnformatted("Saving the game...");
        break;
    case SendPhase::PickGoal: {
        ImGui::TextUnformatted("What's the goal?");
        const float bw = fw;
        if (nav_btn("ch_g_time", "Fastest time", ImVec2(bw, sp(6.5f)), false, true, 0, g_row_y++)) hubui::challenge_send_goal(GoalKind::FastestTime);
        if (nav_btn("ch_g_just", "Just beat it", ImVec2(bw, sp(6.5f)), false, true, 0, g_row_y++)) hubui::challenge_send_goal(GoalKind::JustBeatIt);
        if (nav_btn("ch_g_score", "Score", ImVec2(bw, sp(6.5f)), false, true, 0, g_row_y++)) hubui::challenge_send_goal(GoalKind::Score);
        break;
    }
    case SendPhase::TypeScore: {
        ImGui::TextUnformatted("Type the score to beat.");
        const hubqm::NavAction ta = g_nav.add("ch_type", false, true, 0, g_row_y);
        if (ta.activate) osk::request_open();
        ImVec2 tp = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemWidth(fw - sp(10.f));
        osk::input("##chscore", "Score", g_ch_buf, sizeof g_ch_buf);
        if (g_nav.selected("ch_type")) nav_ring(tp, tp + ImVec2(fw - sp(10.f), ImGui::GetFrameHeight()));
        ImGui::SameLine();
        int64_t tmp = 0;
        if (nav_btn("ch_next", "Next", ImVec2(sp(9.f), ImGui::GetFrameHeight()), true, parse_score(g_ch_buf, &tmp), 1, g_row_y++)) hubui::challenge_send_score(g_ch_buf);
        break;
    }
    case SendPhase::PickFriends: {
        std::vector<std::pair<std::string, std::string>> fl;   // uid, name (online first)
        if (g_ch_fake) fl = {{"u1", "Mickey"}, {"u2", "Mia"}, {"u3", "Jules"}};
        else {
            for (int pass = 0; pass < 2; pass++)
                for (const auto& f : FR.cli.friends()) if ((f.online() ? 0 : 1) == pass) fl.push_back({f.uid, f.name});
        }
        const auto& sel = g_ch_fake ? g_ch_fake_sel : v0.selected;
        ImGui::TextUnformatted(fl.empty() ? "You have no friends to challenge yet." : "Who do you want to challenge? (up to 8)");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        int shown = 0;
        for (const auto& f : fl) {
            if (++shown > 8 + 4) break;   // the list scrolls the menu; keep it short
            const bool on = std::find(sel.begin(), sel.end(), f.first) != sel.end();
            const hub::GameEntry* unused = nullptr; (void)unused;
            if (nav_btn("ch_f_" + f.first, ((on ? "[x]  " : "[ ]  ") + f.second).c_str(), ImVec2(fw, sp(5.5f)), on, true, 0, g_row_y++)) {
                if (g_ch_fake) hub::challenge::friend_toggle(&g_ch_fake_sel, f.first);
                else if (!hubui::challenge_send_toggle_friend(f.first)) push_toast("You can challenge up to 8 friends at once.", COL_WARN, 4.0);
            }
        }
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (nav_btn("ch_go", sel.empty() ? "Send challenge" : ("Send challenge to " + std::to_string(sel.size())).c_str(), ImVec2(fw, sp(6.5f)), true, !sel.empty() && g_pending == 0, 0, g_row_y++)) {
            if (g_ch_fake) push_toast("Challenge sent", COL_GOOD, 3.0); else hubui::challenge_send_go();
        }
        break;
    }
    case SendPhase::Uploading: case SendPhase::Sending:
        ImGui::TextUnformatted(v0.status.empty() ? "Sending..." : v0.status.c_str());
        ImGui::TextDisabled("You can keep this menu open; it only takes a moment.");
        break;
    case SendPhase::Sent:
        text_wrapped_disabled(v0.status.c_str());
        text_wrapped_disabled("Their results will show up under the challenge in your chat.");
        if (nav_btn("ch_done", "Done", ImVec2(fw, sp(6.5f)), true, true, 0, g_row_y++)) { hubui::challenge_send_reset(); g_nav.sel = "Challenge a friend from here"; g_page = QPage::Main; }
        return;
    case SendPhase::Failed:
        ImGui::PushStyleColor(ImGuiCol_Text, ui::BAD);
        ImGui::TextWrapped("%s", v0.error.c_str());
        ImGui::PopStyleColor();
        if (nav_btn("ch_close", "Close", ImVec2(fw, sp(6.5f)), true, true, 0, g_row_y++)) { hubui::challenge_send_reset(); g_nav.sel = "Challenge a friend from here"; g_page = QPage::Main; }
        return;
    default: break;
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    const bool can_back = ph != SendPhase::Uploading && ph != SendPhase::Sending;
    if (can_back && nav_btn("ch_back", "Back", ImVec2(sp(14.f), sp(5.5f)), false, true, 0, g_row_y++, true)) {
        if (ph == SendPhase::TypeScore || ph == SendPhase::PickFriends) { if (g_ch_fake) g_ch_fake_phase = SendPhase::PickGoal; else hubui::challenge_send_back(); }
        else back_to_main();
    }
}

static void qm_body() {
    if (!g_shot_fake && !qm_game_live()) { g_close_why = "game ended"; ImGui::CloseCurrentPopup(); return; }
    if (!g_shot_fake && !g_e2e && g_focus_retry.should_retry(hub_is_foreground(), (long long)GetTickCount64())) {   // bounded: something else (fullscreen Dolphin) took the foreground
        qm_log("hub is not foreground while the menu is open; re-focusing");
        focus_hub();
    }
    const bool back = g_nav.back;
    if (g_close_req.exchange(false)) { g_close_why = g_close_req_why; ImGui::CloseCurrentPopup(); qm_focus_game(); return; }
    if (back) {
        if (g_page != QPage::Main) { g_nav.sel = g_page == QPage::Save ? "Save state" : g_page == QPage::Load ? "Load state" : g_page == QPage::Chat ? "Chat" : g_page == QPage::Emote ? "Emotes" : g_page == QPage::Pass ? "Pass controller" : g_page == QPage::Challenge ? "Challenge a friend from here" : "How do I play"; g_page = QPage::Main; }
        else { g_close_why = "B"; ImGui::CloseCurrentPopup(); qm_focus_game(); return; }
    }
    check_ack();

    const bool online = g_shot_fake ? g_shot_online : qm_online();
    const bool ctl = g_shot_fake ? !g_shot_ext : ctl_ready();
    const hub::GameEntry* e = g_shot_fake ? nullptr : cur_entry(g_key);
    const bool ext = g_shot_fake ? g_shot_ext : (e && !is_native(*e) && !runner_has_ctl(*e));

    if (g_page != QPage::Main) {
        if (g_page == QPage::Pad) pad_page();
        else if (g_page == QPage::Chat) chat_page();
        else if (g_page == QPage::Emote) emote_page();
        else if (g_page == QPage::Pass) pass_page();
        else if (g_page == QPage::Challenge) challenge_page();
        else slot_page(g_page == QPage::Save);
        return;
    }
    ImGui::PushFont(ui::F.title);
    ImGui::TextUnformatted("Game menu");
    ImGui::PopFont();
    ImGui::TextDisabled("%s", g_title.c_str());
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, sp(0.75f)));

    if (!g_shot_fake && !rplay_status_line().empty()) {   // Wii U remote play host: who is watching, how fast, how laggy
        ImGui::PushStyleColor(ImGuiCol_Text, ui::GOOD);
        ImGui::TextUnformatted(rplay_status_line().c_str());
        ImGui::PopStyleColor();
    }
    if (menu_button("Resume", true, ui::Icon::Play, true)) { g_close_why = "Resume"; ImGui::CloseCurrentPopup(); qm_focus_game(); }
    if (ctl || ext) {   // Speed (built-in runners, offline), Volume + Mute (everything; local only, so online too)
        const float fw = ImGui::GetContentRegionAvail().x;
        const bool can_speed = hubqm::speed_available(ctl, online);
        if (ctl) {
            int d = stepper_row("speed", "Speed", can_speed ? hubqm::speed_text(g_speed_idx) : std::string("1x"), -1.f, can_speed && g_pending == 0, fw);
            if (d) {
                int ni = hubqm::speed_step(g_speed_idx, d);
                if (ni != g_speed_idx) {
                    resume_for_action();   // n64run refuses a speed change while paused
                    g_speed_idx = ni;
                    runctl::Command c; c.cmd = runctl::Cmd::Speed; c.speed = hubqm::speed_value(ni);
                    send_quiet(c);
                }
            }
            if (!can_speed) text_wrapped_disabled("Not available online: everyone has to stay in step.");
        } else {   // external emulators: Dolphin through its speed hotkeys (sent on Resume); the others show why it is off
            const bool dol = g_shot_fake ? g_shot_dolphin : is_dolphin(e);
            const auto row = hubqm::speed_row(false, dol, online, true);
            if (row == hubqm::SpeedRow::Enabled) {
                int d = stepper_row("speed", "Speed", hubqm::dolphin_speed_text(g_speed_idx), -1.f, true, fw);
                if (d) g_speed_idx = hubqm::dolphin_speed_step(g_speed_idx, d);   // DSU pad: applied at once (qm_tick); keyboard fallback: when the menu closes
                if (g_speed_idx != hubqm::dolphin_idx(g_dolphin)) text_wrapped_disabled(use_dsu() ? "Changing Dolphin's speed..." : "Applies when you resume.");
            } else {
                stepper_row("speed", "Speed", "1x", -1.f, false, fw);
                if (row == hubqm::SpeedRow::Online) text_wrapped_disabled("Not available online: everyone has to stay in step.");
                else text_wrapped_disabled("Speed isn't supported by " + ext_emu_name(e) + ".");
            }
        }
        int d = stepper_row("volume", "Volume", std::to_string(g_vol) + "%", g_mute ? 0.f : g_vol / 100.f, g_pending == 0, fw);
        if (d) {
            int nv = hubqm::volume_step(g_vol, d);
            if (nv != g_vol) {
                g_vol = nv;
                if (ctl) { runctl::Command c; c.cmd = runctl::Cmd::Volume; c.value = nv; send_quiet(c); }
                else { g_ext_dirty = true; g_ext_next = 0; }
            }
        }
        if (stepper_row("mute", "Sound", g_mute ? "Muted" : "On", -1.f, g_pending == 0, fw)) {   // any left / right / A toggles
            g_mute = !g_mute;
            if (ctl) { runctl::Command c; c.cmd = runctl::Cmd::Mute; c.value = g_mute ? 1 : 0; send_quiet(c); }
            else { g_ext_dirty = true; g_ext_next = 0; }
        }
        if (ctl && (g_shot_fake || runner_supports_overlay(e))) {   // FPS / ping / input delay / rollback on the game's OSD (display only, fine online)
            const bool on = g_shot_fake ? g_perf_shot : A.st.perf_overlay;
            if (stepper_row("perf", "Stats overlay", on ? "On" : "Off", -1.f, g_pending == 0, fw)) {
                if (g_shot_fake) g_perf_shot = !g_perf_shot;
                else { A.st.perf_overlay = !A.st.perf_overlay; mark_dirty(); g_perf_sent = 2; }
                runctl::Command c; c.cmd = runctl::Cmd::Overlay; c.value = (g_shot_fake ? g_perf_shot : A.st.perf_overlay) ? 1 : 0; send_quiet(c);
            }
        }
        if (ext && g_ext_sessions == 0 && !g_shot_fake) text_wrapped_disabled("The game has not started its sound yet.");
    }
    if (!ctl && !g_shot_fake && e && e->system == hub::System::PSP) {   // PPSSPP: its own quick save / load (F2 / F4), sent to the game window
        if (menu_button("Save state", !online, ui::Icon::Check)) { ImGui::CloseCurrentPopup(); qm_focus_game(); ppsspp_send_state(true); }
        if (menu_button("Load state", !online, ui::Icon::Refresh)) { ImGui::CloseCurrentPopup(); qm_focus_game(); ppsspp_send_state(false); }
    }
    if (ctl) {
        if (menu_button("Save state", !online, ui::Icon::Check)) { g_page = QPage::Save; g_nav.sel.clear(); g_ovw.cancel(); }
        if (menu_button("Load state", !online, ui::Icon::Refresh)) { g_page = QPage::Load; g_nav.sel.clear(); }
        if (online) text_wrapped_disabled("Save states are off in online games: everyone has to stay in step.");
        if (menu_button("Take a screenshot", true, ui::Icon::Search)) send(runctl::Cmd::Screenshot, 0, "Screenshot saved", true);
        {   // Rewind hint (no row), screen filter, instant-replay clip: only where the runner can do them (qm_logic.h feature_row_visible)
            const bool runner = g_shot_fake || g_e2e || (e && runner_has_ctl(*e));
            const bool n64 = e && e->system == hub::System::N64;
            const bool e2e_off = g_e2e && !std::getenv("HUB_QM_E2E_FEATURES");   // the e2e scenarios count rows: the stand-in shows none unless asked
            auto vis = [&](hubqm::QmFeature f) { return !e2e_off && hubqm::feature_row_visible(f, runner, online, n64, g_hide.is(f)); };
            // No latching Rewind row: rewind is a hold in retrorun (LT + View, main.cpp "hold LT + View ... to rewind"; fixed, not configurable).
            if (vis(hubqm::QmFeature::Rewind)) text_wrapped_disabled(hubqm::kRewindHint);
            if (vis(hubqm::QmFeature::Filter) && menu_button("Next screen filter", true, ui::Icon::Refresh)) {
                runctl::Command c; c.cmd = runctl::Cmd::Shader; c.text = "next";
                send_feature(c, "", false);   // the toast is the runner's answer, e.g. "Filter: CRT"
            }
            if (vis(hubqm::QmFeature::Clip) && menu_button("Save a clip of the last 30 seconds", true, ui::Icon::Check)) {
                runctl::Command c; c.cmd = runctl::Cmd::Clip;
                send_feature(c, "Saving the clip. It will appear in your screenshots.", true);
            }
        }
        if (hubqm::pause_available(true, online)) {   // Plan 4: the game stands still while the menu is open (offline only)
            const float pw = ImGui::GetContentRegionAvail().x;
            if (stepper_row("pause", "Pause game", g_pause_manual ? "Paused" : "Running", -1.f, g_pending == 0, pw)) g_pause_manual = !g_pause_manual;   // qm_tick sends pause 1 / 0
            if (stepper_row("pause_auto", "Pause when menu opens", g_pause_auto ? "On" : "Off", -1.f, g_pending == 0, pw)) { g_pause_auto = !g_pause_auto; prefs_save(); }
        }
        {   // Plan 4 p4-social: friend challenges (built-in runners, offline)
            const std::string csys = e ? sys_short(*e) : g_sys;
            const auto cr = hub::challenge::menu_rows(csys, true, online, hubui::challenge_running(), hub::challenge::send_busy(hubui::challenge_send_view().phase));
            if (cr.challenge) {
                if (menu_button("Challenge a friend from here", cr.challenge_enabled, ui::Icon::Bolt)) {
                    g_ch_fake = g_shot_fake;
                    if (g_shot_fake || (resume_for_action(), hubui::challenge_send_begin(g_key, g_title, csys, states_dir_of(g_key, csys)))) { g_page = QPage::Challenge; g_nav.sel.clear(); g_ch_buf[0] = 0; }
                }
                if (!cr.challenge_enabled && !cr.why.empty()) text_wrapped_disabled(cr.why.c_str());
            }
            if (cr.did_it && menu_button("I did it", true, ui::Icon::Check, true)) {
                if (hubui::challenge_goal_is_score()) { g_page = QPage::Challenge; g_nav.sel.clear(); g_ch_buf[0] = 0; }
                else { hubui::challenge_run_finish(true, ""); g_close_why = "challenge done"; ImGui::CloseCurrentPopup(); qm_focus_game(); }
            }
            if (cr.give_up && menu_button("Give up", true, ui::Icon::Close)) { hubui::challenge_run_finish(false, ""); g_close_why = "challenge gave up"; ImGui::CloseCurrentPopup(); qm_focus_game(); }
        }
    } else if (ext) {   // Dolphin, Cemu, Azahar, PCSX2: capture the emulator's window into the vault's shots folder
        if (g_shot_fake ? g_shot_dolphin : is_dolphin(e)) {   // Dolphin: pause and quick slot 1, through the hotkeys RetroHub binds (qm_logic.h)
            if (menu_button("Save state", !online, ui::Icon::Check)) { g_dol_queue.push_back(hubqm::DolphinAct::SaveState); g_close_why = "Dolphin save state"; ImGui::CloseCurrentPopup(); qm_focus_game(); }
            if (menu_button("Load state", !online, ui::Icon::Refresh)) { g_dol_queue.push_back(hubqm::DolphinAct::LoadState); g_close_why = "Dolphin load state"; ImGui::CloseCurrentPopup(); qm_focus_game(); }
            if (online) text_wrapped_disabled("Save states are off in online games: everyone has to stay in step.");
            else {
                if (!g_dol_paused.load() && menu_button("Pause game", true, ui::Icon::Play)) { g_dol_queue.push_back(hubqm::DolphinAct::Pause); g_close_why = "Dolphin pause"; ImGui::CloseCurrentPopup(); qm_focus_game(); }
                text_wrapped_disabled(g_dol_paused.load() ? "The game is paused. Resume carries on." : "Uses Dolphin's quick slot 1.");
            }
        }
        if (menu_button("Screenshot", true, ui::Icon::Search)) {
            if (g_shot_fake) { push_toast("Screenshot saved", COL_GOOD, 3.0); }
            else if (e) {
                std::string out = trim_sep(hub::saves::game_dir(sys_short(*e), e->key)) + "\\shots\\" + runctl::shot_name(std::time(nullptr));
                if (extmedia::capture_start(gc_pid() ? (void*)window_of_pid((DWORD)gc_pid()) : running_game_hwnd(), out)) { ImGui::CloseCurrentPopup(); qm_focus_game(); }
                else push_toast("Could not find the game window.", COL_WARN, 4.0);
            }
        }
    }
    if (e && !gc_pid() && cheats_supported(*e)) {   // GameCube NetPlay: the codes were synced at launch
        if (menu_button("Cheats", true, ui::Icon::Bolt)) { hub::GameEntry c = *e; cheats_open_from_game(c); ImGui::CloseCurrentPopup(); }
    }
    if (g_shot_fake ? false : (e && e->system == hub::System::WiiU)) {   // Cemu: Ctrl+Tab = toggle pad screen (also View + RB held)
        if (menu_button("Swap GamePad / TV screen", true, ui::Icon::Refresh)) { ImGui::CloseCurrentPopup(); wiiu_toggle_gamepad_screen(); }
    }
    if (!g_shot_fake && e && e->system == hub::System::PS1 && ps1_swap_available(e->key)) {
        if (menu_button("Swap disc", true, ui::Icon::Refresh)) { ps1_swap_disc(e->key); push_toast("Swapping to the next disc", COL_GOOD, 3.0); ImGui::CloseCurrentPopup(); qm_focus_game(); }
    }
    if (g_shot_fake || fr_in_my_room()) {   // the picker is a popup over the menu; while it is open ImGui's nav drives it (g_sub_open)
        const bool can = fr_in_my_room();
        const hubqm::NavAction a = g_nav.add("Invite friends", false, can, 0, g_row_y++);
        if (a.activate) { FR.open_picker = true; qm_log("menu: activate \"Invite friends\""); }
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float iw = ImGui::GetContentRegionAvail().x;
        fr_invite_button_and_picker(iw, sp(6.5f));
        if (g_nav.selected("Invite friends")) nav_ring(p, p + ImVec2(iw, sp(6.5f)));
    }
    if (g_shot_fake || fr_in_my_room() || !g_dm_uid.empty()) {
        if (menu_button("Chat", true, ui::Icon::Search)) { g_page = QPage::Chat; g_nav.sel.clear(); }
    }
    if (online && (g_shot_fake || fr_in_my_room())) {   // Plan 4 P5: GG / NICE! / LOL / OOF / ONE MORE? to everyone's OSD
        if (menu_button("Emotes", true, ui::Icon::Bolt)) { g_page = QPage::Emote; g_nav.sel.clear(); }
    }
    if (ctl && online && (g_shot_fake || (room_connected() && my_port() >= 0)) && !pass_targets().empty()) {   // Plan 4 P4: hand the controller to another player
        if (menu_button("Pass controller", g_ho_wait == false, ui::Icon::Refresh)) { g_page = QPage::Pass; g_nav.sel.clear(); }
    }
    if (g_shot_fake || (e && !hubctl::console_key(*e).empty())) {
        if (menu_button("How do I play", true, ui::Icon::Play)) { g_pad_loaded = false; g_page = QPage::Pad; g_nav.sel.clear(); }
    }
    if (menu_button("Quit to RetroHub", true, ui::Icon::Close)) {
        g_speed_idx = hubqm::dolphin_idx(g_dolphin);   // Dolphin: quitting never sends speed keys (a pending change is dropped); the next game starts at 1x anyway
        g_close_why = "Quit";
        if (g_e2e) g_e2e_cmds.push_back("quit");
        else if (!g_shot_fake) { if (gc_pid()) gc_leave("menu Quit"); else if (ctl_ready()) runctl::send_command(A.run.ctl_path, runctl::Cmd::Quit, 0); else request_game_close(); }
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::TextDisabled("Press B to go back to the game.");
}

/* ---------------- screenshots gallery (game details modal) ---------------- */

static bool is_image(const std::string& n) {
    auto dot = n.rfind('.');
    if (dot == std::string::npos) return false;
    std::string x = n.substr(dot);
    for (auto& c : x) c = (char)tolower((unsigned char)c);
    return x == ".png" || x == ".bmp" || x == ".mp4";   // .mp4 = instant-replay clip (clip.h), shown by its .mp4.jpg thumbnail
}

int shots_count(const hub::GameEntry& e) {
    if (is_native(e)) return 0;
    static std::string k; static double at = -10; static int n = 0;
    if (k != e.key || ImGui::GetTime() - at > 2.0) {
        k = e.key; at = ImGui::GetTime(); n = 0;
        std::error_code ec;
        for (auto& it : std::filesystem::directory_iterator(std::filesystem::u8path(trim_sep(hub::saves::game_dir_path(sys_short(e), e.key)) + "\\shots"), ec))
            if (it.is_regular_file(ec) && is_image(it.path().filename().u8string())) ++n;
    }
    return n;
}

void shots_gallery(const hub::GameEntry& e) {
    if (is_native(e)) return;
    std::string dir = trim_sep(hub::saves::game_dir_path(sys_short(e), e.key)) + "\\shots";
    static std::string s_key; static double s_at = -10; static std::vector<std::string> s_files;
    static std::string s_confirm;
    if (s_key != e.key || ImGui::GetTime() - s_at > 2.0) {
        s_key = e.key; s_at = ImGui::GetTime();
        s_files.clear();
        std::error_code ec;
        for (auto& it : std::filesystem::directory_iterator(std::filesystem::u8path(dir), ec))
            if (it.is_regular_file(ec) && is_image(it.path().filename().u8string())) s_files.push_back(it.path().filename().u8string());
        std::sort(s_files.begin(), s_files.end(), std::greater<std::string>());   // newest first (names are timestamps)
    }
    if (s_files.empty()) return;   // the tab is hidden when there are none
    if (g_headless && std::getenv("HUB_SHOT_VIEWER")) {   // screenshot aid: open the full-size viewer once
        static bool once = false;
        if (!once) { once = true; std::vector<std::string> all; for (auto& f : s_files) all.push_back(dir + "\\" + f);
            shot_viewer_open(e, all, 1, [](const std::string& p, int* w, int* h) -> SDL_Texture* { Tex* t = tex_for(p); if (!t) return nullptr; *w = t->w; *h = t->h; return t->t; }); }
    }
    if (ImGui::SmallButton("Open folder")) ShellExecuteW(nullptr, L"open", std::filesystem::u8path(dir).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const float th = sp(13.f), tw = th * 1.6f;
    ImGui::BeginChild("##shots", ImVec2(0, th + sp(7.f)), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_HorizontalScrollbar);
    for (size_t i = 0; i < s_files.size() && i < 60; ++i) {
        std::string path = dir + "\\" + s_files[i];
        ImGui::PushID((int)i);
        ImGui::BeginGroup();
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool open_it = ImGui::InvisibleButton("shot", ImVec2(tw, th));
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        Tex* t = tex_for(path, &g_shot_budget);   // placeholder until its turn: a few decodes per frame, not 60 at once
        if (t) ui::draw_art(dl, t->t, t->w, t->h, p, ImVec2(p.x + tw, p.y + th), sp(1.f));
        else dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), ui::PANEL2, sp(1.f));
        if (hot) dl->AddRect(p, ImVec2(p.x + tw, p.y + th), ui::ACCENT, sp(1.f), 0, 2.f);
        if (s_files[i].size() > 4 && _stricmp(s_files[i].c_str() + s_files[i].size() - 4, ".mp4") == 0) {   // clip: dark disc with a play triangle
            ImVec2 c(p.x + tw * 0.5f, p.y + th * 0.5f);
            dl->AddCircleFilled(c, sp(3.f), IM_COL32(0, 0, 0, 170), 24);
            ui::icon(dl, ui::Icon::Play, c, sp(3.f), IM_COL32(255, 255, 255, 235));
        }
        if (s_confirm == s_files[i]) {
            if (ImGui::SmallButton("Delete for good?")) {
                std::error_code ec;
                std::filesystem::remove(std::filesystem::u8path(path), ec);
                if (path.size() > 4 && _stricmp(path.c_str() + path.size() - 4, ".mp4") == 0) { std::error_code e2; std::filesystem::remove(std::filesystem::u8path(path + ".jpg"), e2); }
                s_confirm.clear(); s_at = -10;
            }
        } else {
            if (ImGui::SmallButton("Delete")) s_confirm = s_files[i];
        }
        ImGui::EndGroup();
        if (open_it) {   // full-size in-app viewer (shot_viewer.cpp); "Open in viewer" lives inside it
            std::vector<std::string> all;
            for (auto& f : s_files) all.push_back(dir + "\\" + f);
            shot_viewer_open(e, all, (int)i, [](const std::string& p, int* w, int* h) -> SDL_Texture* {
                Tex* t = tex_for(p); if (!t) return nullptr; *w = t->w; *h = t->h; return t->t; });
        }
        ImGui::PopID();
        ImGui::SameLine(0, sp(1.f));
    }
    ImGui::EndChild();
    if (shot_viewer_draw()) s_at = -10;   // a picture was deleted: rescan
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
}

/* Headless screenshot aid (HUB_SHOT_GALLERY=1): drops six synthetic pictures into the selected game's shots folder. */
void qm_seed_gallery(const hub::GameEntry& e) {
    static bool done = false;
    if (done || !g_headless || !std::getenv("HUB_SHOT_GALLERY")) return;
    done = true;
    std::string dir = trim_sep(hub::saves::game_dir(sys_short(e), e.key)) + "\\shots";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    for (int i = 0; i < 6; ++i) {
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, 320, 200, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!s) continue;
        SDL_FillRect(s, nullptr, SDL_MapRGB(s->format, (Uint8)(30 + i * 35), (Uint8)(120 - i * 12), (Uint8)(210 - i * 30)));
        SDL_Rect r{30 + i * 30, 70, 70, 80};
        SDL_FillRect(s, &r, SDL_MapRGB(s->format, 250, 210, 70));
        char nm[64];
        std::snprintf(nm, sizeof nm, "\\2026102%d-12%02d00.bmp", i % 5, i);
        SDL_SaveBMP(s, (dir + nm).c_str());
        SDL_FreeSurface(s);
    }
}

/* ---------------- headless end-to-end test hooks (qm_e2e.cpp) ---------------- */
void qm_e2e_enable() {
    g_e2e = true;
    A.run.key = "n64:e2e"; A.run.title = "E2E stand-in"; A.run.system_short = "n64"; A.run.online = false;
}
QmE2EState qm_e2e_state() {
    QmE2EState st;
    st.open = g_qm_showing.load();
    st.page = g_page == QPage::Main ? 0 : g_page == QPage::Save ? 1 : 2;
    st.sel = g_nav.sel;
    st.speed_idx = g_speed_idx;
    st.vol = g_vol;
    st.mute = g_mute;
    st.close_why = g_close_why;
    st.cmds = g_e2e_cmds;
    st.imgui_nav = (ImGui::GetIO().ConfigFlags & (ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad)) == (ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
    return st;
}

/* Controllers page -> running game (controls_live_push): one queue for every hub-to-runner command, so a reload waits for the previous
 * command's ack and repeated saves collapse into one reload. */
void qm_send_command(const runctl::Command& c) { send_quiet(c); }
