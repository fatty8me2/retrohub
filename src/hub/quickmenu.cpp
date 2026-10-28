#include "app.h"
#include "dolphin_speed.h"

#include <SDL.h>
#include <shellapi.h>

#include <atomic>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>

#include "runner_ctl.h"
#include "cloud_engine.h"
#include "qm_logic.h"
#include "qm_nav.h"
#include "qm_pad.h"
#include "ext_media.h"
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"

/* ---------- In-game quick menu (View + Y) and screenshots gallery ----------
 * Contract with the built-in runners: src/common/runner_ctl.h and docs/RUNNER_CTL.md. The hub writes <run_dir>/control.json,
 * the runner acts once per seq and writes control_ack.json. Save states: <states>/slot<N>.state + .png thumbnail + .json sidecar.
 * Screenshots: <vault>/<system>/<game key>/shots/<yyyyMMdd-HHmmss>.png. */

namespace {

SDL_Renderer* g_ren = nullptr;

struct Tex { SDL_Texture* t = nullptr; int w = 0, h = 0; std::string stamp; };
std::map<std::string, Tex> g_tex;

std::string file_stamp(const std::string& p) {
    std::error_code ec;
    auto fp = std::filesystem::u8path(p);
    auto t = std::filesystem::last_write_time(fp, ec);
    if (ec) return std::string();
    auto sz = std::filesystem::file_size(fp, ec);
    return std::to_string((long long)t.time_since_epoch().count()) + ":" + std::to_string((long long)sz);
}

/* PNG (or BMP, used by the headless screenshot aid) file -> texture, cached by path + stamp. */
Tex* tex_for(const std::string& path) {
    if (!g_ren) return nullptr;
    std::string st = file_stamp(path);
    if (st.empty()) return nullptr;
    auto it = g_tex.find(path);
    if (it != g_tex.end() && it->second.stamp == st) return it->second.t ? &it->second : nullptr;
    if (it != g_tex.end() && it->second.t) SDL_DestroyTexture(it->second.t);
    if (g_tex.size() > 80) { for (auto& kv : g_tex) if (kv.second.t) SDL_DestroyTexture(kv.second.t); g_tex.clear(); }
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
    g_tex[path] = nt;
    return nt.t ? &g_tex[path] : nullptr;
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
enum class QPage { Main, Save, Load };
bool g_open_req = false, g_shot_fake = false;
QPage g_page = QPage::Main;
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
static const char* g_close_why = "other";
bool g_pending_close = true;                   // close the menu and return to the game when the ack arrives
int g_pending_slot = -2;
double g_pending_at = 0;
std::string g_sys, g_key, g_title;

bool ctl_ready() { return g_e2e || (A.proc && !A.run.ctl_path.empty()); }

void send(runctl::Cmd c, int slot, const std::string& done_text, bool close) {
    if (g_e2e) { g_e2e_cmds.push_back(std::string(runctl::cmd_name(c)) + " " + std::to_string(slot)); push_toast(done_text, COL_GOOD, 3.0); if (close) { g_page = QPage::Main; g_close_why = "slot action"; ImGui::CloseCurrentPopup(); } return; }
    long long seq = runctl::send_command(A.run.ctl_path, c, slot);
    if (!seq) { push_toast("Could not talk to the game.", COL_WARN, 4.0); return; }
    g_pending = seq; g_pending_what = done_text; g_pending_close = close; g_pending_slot = slot; g_pending_at = ImGui::GetTime();
}

void nav_ring(ImVec2 p0, ImVec2 p1) {
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

void draw_slot_card(const std::string& dir, int slot, bool saving, bool* chosen, int nx, int ny) {
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
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(p.x + sp(1.f), p.y + th + sp(0.25f) + ui::F.bold->FontSize * 1.25f), ui::TEXT2, sub.c_str());
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
        draw_slot_card(dir, slot, saving, &chosen, i % cols, i / cols);
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::PopItemWidth();
        ImGui::EndGroup();
        if (chosen) {
            if (saving) send(runctl::Cmd::SaveState, slot, "Saved to slot " + std::to_string(slot + 1), true);
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

/* Fire and forget: the menu stays open and responsive; the runner acts at its next poll. */
void send_quiet(runctl::Command c) {
    if (g_e2e) { g_e2e_cmds.push_back(std::string(runctl::cmd_name(c.cmd)) + " " + (c.cmd == runctl::Cmd::Speed ? [&] { char b[24]; std::snprintf(b, sizeof b, "%g", c.speed); return std::string(b); }() : std::to_string(c.value))); return; }
    if (g_shot_fake || !ctl_ready()) return;
    if (!runctl::send_command_ex(A.run.ctl_path, c)) push_toast("Could not talk to the game.", COL_WARN, 4.0);
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
    if (!is_dolphin(e) || A.run.online || g_shot_fake || g_burst_state.load() != 0 || (qm_is_open() && !use_dsu())) return;   // the DSU pad needs no focus: applies live, menu open or not
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
    g_ext_sessions = extmedia::set_audio(running_game_pids(), hubqm::ext_session_scalar(g_vol, base), g_mute);
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

void check_ack() {
    if (!g_pending) return;
    std::string txt;
    runctl::Ack a;
    if (runctl::read_text(runctl::ack_path_for(A.run.ctl_path), &txt) && runctl::parse_ack(txt, &a) && a.seq == g_pending) {
        g_pending = 0;
        if (a.ok) {
            push_toast(g_pending_what, COL_GOOD, 3.0);
            if (g_pending_close) { g_page = QPage::Main; g_close_why = "slot action"; ImGui::CloseCurrentPopup(); focus_game(); }
        } else push_toast(a.msg.empty() ? std::string("That did not work.") : a.msg, COL_WARN, 5.0);
    } else if (ImGui::GetTime() - g_pending_at > 20.0) {
        g_pending = 0;
        push_toast("The game did not answer.", COL_WARN, 5.0);
    }
}

}  // namespace

/* ---------------- public ---------------- */

void qm_init(SDL_Renderer* ren) { g_ren = ren; }

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

static std::atomic<bool> g_qm_showing{false};

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
    g_dolphin.known = false;   // do not trust what we think Dolphin runs at: go down to its floor, then up to 1x
    g_retry_n = 0; g_retry_at = 0;
    g_close_req = g_qm_showing.load();
    push_toast("Resetting Dolphin to 1x...", COL_GOOD, 4.0);
    qm_log("emergency reset to 1x requested");
}
bool qm_is_open() { return g_qm_showing.load(); }   // thread-safe; set at the request, refreshed every draw


void qm_open_live() {
    if (!qm_game_live() || A.run.key.empty()) return;
    if (g_qm_showing.load()) { qm_log("open request ignored: menu already open"); return; }   // View+Y never re-opens / toggles an open menu
    g_retry_n = 0; g_retry_at = 0;   // a Dolphin speed change that gave up is tried again once the menu closes
    g_activity_ms = GetTickCount64();
    qm_log("open request (hub fg=" + std::string(hub_is_foreground() ? "yes" : "no") + " fg=" + qm_describe_window(GetForegroundWindow()) + ")");
    const hub::GameEntry* e = g_e2e ? nullptr : find_entry(A.run.key);
    g_sys = e ? sys_short(*e) : A.run.system_short;
    g_key = A.run.key;
    g_title = e ? e->title : A.run.title;
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
    if (!A.proc || A.run.key.empty()) { g_sess_key.clear(); return; }
    if (g_sess_key != A.run.key || g_sess_tick != A.run.start_tick) {
        g_sess_key = A.run.key; g_sess_tick = A.run.start_tick;
        g_speed_idx = hubqm::kSpeedNormal;
        g_dolphin = hubqm::DolphinSpeed();
        g_retry_n = 0; g_retry_at = 0; g_burst_state = 0; g_dsu_dead = false;
        qm_log("session start " + A.run.key);   // Dolphin.ini was written with EmulationSpeed = 1.0 at launch
        g_mute = false;
        g_vol = e ? hub::gs::volume_of(gs_values_for(*e)) : 100;
        g_launch_vol = g_vol;
        g_ext_sessions = -1;
        g_ext_dirty = true;
    }
    if (e && !is_native(*e) && !runner_has_ctl(*e)) {
        double now = ImGui::GetTime();
        bool need = g_ext_dirty || g_mute || hubqm::ext_session_scalar(g_vol, is_dolphin(e) ? g_launch_vol : 100) < 1.f;
        if (need && now >= g_ext_next) { g_ext_next = now + 2.0; ext_apply(e); }
    }
}

void qm_tick() {
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
        g_page = std::string(shot) == "save" ? QPage::Save : std::string(shot) == "load" ? QPage::Load : QPage::Main;
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

bool qm_game_live() { return A.proc != nullptr || g_e2e; }
static hubqm::FocusRetry g_focus_retry;

static void qm_body();

void draw_quick_menu() {
    qm_tick();
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
        if (!g_shot_fake) qm_log("menu opened (held at open: 0x" + [](uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%x", v); return std::string(b); }(raw) + ")");
    }
    if (was_open && !open) {
        if (!g_shot_fake) qm_log(std::string("menu closed (") + g_close_why + ")");
        g_activity_ms = GetTickCount64();
        g_qm_showing = false;
        g_sub_open = false;
        hub_menu_topmost(false);
        if (qm_game_live() && !g_e2e) dolphin_apply_pending(find_entry(g_key));   // closed this frame (Resume / B / pad combo)
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

static void qm_body() {
    if (!g_shot_fake && !qm_game_live()) { g_close_why = "game ended"; ImGui::CloseCurrentPopup(); return; }
    if (!g_shot_fake && !g_e2e && g_focus_retry.should_retry(hub_is_foreground(), (long long)GetTickCount64())) {   // bounded: something else (fullscreen Dolphin) took the foreground
        qm_log("hub is not foreground while the menu is open; re-focusing");
        focus_hub();
    }
    const bool back = g_nav.back;
    if (g_close_req.exchange(false)) { g_close_why = "reset requested"; ImGui::CloseCurrentPopup(); focus_game(); return; }
    if (back) {
        if (g_page != QPage::Main) { g_nav.sel = g_page == QPage::Save ? "Save state" : "Load state"; g_page = QPage::Main; }
        else { g_close_why = "B"; ImGui::CloseCurrentPopup(); focus_game(); return; }
    }
    check_ack();

    const bool online = g_shot_fake ? g_shot_online : A.run.online;
    const bool ctl = g_shot_fake ? !g_shot_ext : ctl_ready();
    const hub::GameEntry* e = g_shot_fake ? nullptr : find_entry(g_key);
    const bool ext = g_shot_fake ? g_shot_ext : (e && !is_native(*e) && !runner_has_ctl(*e));

    if (g_page != QPage::Main) {
        slot_page(g_page == QPage::Save);
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
    if (menu_button("Resume", true, ui::Icon::Play, true)) { g_close_why = "Resume"; ImGui::CloseCurrentPopup(); focus_game(); }
    if (ctl || ext) {   // Speed (built-in runners, offline), Volume + Mute (everything; local only, so online too)
        const float fw = ImGui::GetContentRegionAvail().x;
        const bool can_speed = hubqm::speed_available(ctl, online);
        if (ctl) {
            int d = stepper_row("speed", "Speed", can_speed ? hubqm::speed_text(g_speed_idx) : std::string("1x"), -1.f, can_speed && g_pending == 0, fw);
            if (d) {
                int ni = hubqm::speed_step(g_speed_idx, d);
                if (ni != g_speed_idx) {
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
        if (ext && g_ext_sessions == 0 && !g_shot_fake) text_wrapped_disabled("The game has not started its sound yet.");
    }
    if (ctl) {
        if (menu_button("Save state", !online, ui::Icon::Check)) { g_page = QPage::Save; g_nav.sel.clear(); }
        if (menu_button("Load state", !online, ui::Icon::Refresh)) { g_page = QPage::Load; g_nav.sel.clear(); }
        if (online) text_wrapped_disabled("Save states are off in online games: everyone has to stay in step.");
        if (menu_button("Take a screenshot", true, ui::Icon::Search)) send(runctl::Cmd::Screenshot, 0, "Screenshot saved", true);
    } else if (ext) {   // Dolphin, Cemu, Azahar, PCSX2: capture the emulator's window into the vault's shots folder
        if (menu_button("Screenshot", true, ui::Icon::Search)) {
            if (g_shot_fake) { push_toast("Screenshot saved", COL_GOOD, 3.0); }
            else if (e) {
                std::string out = trim_sep(hub::saves::game_dir(sys_short(*e), e->key)) + "\\shots\\" + runctl::shot_name(std::time(nullptr));
                if (extmedia::capture_start(running_game_hwnd(), out)) { ImGui::CloseCurrentPopup(); focus_game(); }
                else push_toast("Could not find the game window.", COL_WARN, 4.0);
            }
        }
    }
    if (e && cheats_supported(*e)) {
        if (menu_button("Cheats", true, ui::Icon::Bolt)) { hub::GameEntry c = *e; cheats_open(c); ImGui::CloseCurrentPopup(); }
    }
    if (g_shot_fake ? false : (e && e->system == hub::System::WiiU)) {   // Cemu: Ctrl+Tab = toggle pad screen (also View + RB held)
        if (menu_button("Swap GamePad / TV screen", true, ui::Icon::Refresh)) { ImGui::CloseCurrentPopup(); wiiu_toggle_gamepad_screen(); }
    }
    if (!g_shot_fake && e && e->system == hub::System::PS1 && ps1_swap_available(e->key)) {
        if (menu_button("Swap disc", true, ui::Icon::Refresh)) { ps1_swap_disc(e->key); push_toast("Swapping to the next disc", COL_GOOD, 3.0); ImGui::CloseCurrentPopup(); focus_game(); }
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
    if (menu_button("Quit to RetroHub", true, ui::Icon::Close)) {
        g_speed_idx = hubqm::dolphin_idx(g_dolphin);   // Dolphin: quitting never sends speed keys (a pending change is dropped); the next game starts at 1x anyway
        g_close_why = "Quit";
        if (g_e2e) g_e2e_cmds.push_back("quit");
        else if (!g_shot_fake) { if (ctl_ready()) runctl::send_command(A.run.ctl_path, runctl::Cmd::Quit, 0); else request_game_close(); }
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
    return x == ".png" || x == ".bmp";
}

int shots_count(const hub::GameEntry& e) {
    if (is_native(e)) return 0;
    static std::string k; static double at = -10; static int n = 0;
    if (k != e.key || ImGui::GetTime() - at > 2.0) {
        k = e.key; at = ImGui::GetTime(); n = 0;
        std::error_code ec;
        for (auto& it : std::filesystem::directory_iterator(std::filesystem::u8path(trim_sep(hub::saves::game_dir(sys_short(e), e.key)) + "\\shots"), ec))
            if (it.is_regular_file(ec) && is_image(it.path().filename().u8string())) ++n;
    }
    return n;
}

void shots_gallery(const hub::GameEntry& e) {
    if (is_native(e)) return;
    std::string dir = trim_sep(hub::saves::game_dir(sys_short(e), e.key)) + "\\shots";
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
        Tex* t = tex_for(path);
        if (t) ui::draw_art(dl, t->t, t->w, t->h, p, ImVec2(p.x + tw, p.y + th), sp(1.f));
        else dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), ui::PANEL2, sp(1.f));
        if (hot) dl->AddRect(p, ImVec2(p.x + tw, p.y + th), ui::ACCENT, sp(1.f), 0, 2.f);
        if (s_confirm == s_files[i]) {
            if (ImGui::SmallButton("Delete for good?")) {
                std::error_code ec;
                std::filesystem::remove(std::filesystem::u8path(path), ec);
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
