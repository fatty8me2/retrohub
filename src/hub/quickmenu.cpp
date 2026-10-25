#include "app.h"

#include <SDL.h>
#include <shellapi.h>

#include <ctime>
#include <fstream>
#include <map>

#include "runner_ctl.h"
#include "cloud_engine.h"
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
bool g_pending_close = true;                   // close the menu and return to the game when the ack arrives
int g_pending_slot = -2;
double g_pending_at = 0;
std::string g_sys, g_key, g_title;

bool ctl_ready() { return A.proc && !A.run.ctl_path.empty(); }

void send(runctl::Cmd c, int slot, const std::string& done_text, bool close) {
    long long seq = runctl::send_command(A.run.ctl_path, c, slot);
    if (!seq) { push_toast("Could not talk to the game.", COL_WARN, 4.0); return; }
    g_pending = seq; g_pending_what = done_text; g_pending_close = close; g_pending_slot = slot; g_pending_at = ImGui::GetTime();
}

bool menu_button(const char* label, bool enabled, ui::Icon ic, bool primary = false) {
    float w = ImGui::GetContentRegionAvail().x;
    return ui::button(label, ImVec2(w, sp(6.5f)), primary, enabled && g_pending == 0, ic);
}

void draw_slot_card(const std::string& dir, int slot, bool saving, bool* chosen) {
    SlotInfo si = read_slot(dir, slot);
    float cw = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float th = cw * 0.625f, h = th + sp(5.5f);
    ImGui::PushID(slot + 5);
    bool enabled = saving || si.exists;
    ImGui::BeginDisabled(!enabled || g_pending != 0);
    bool clicked = ImGui::InvisibleButton("slot", ImVec2(cw, h));
    ImGui::EndDisabled();
    bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
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
        draw_slot_card(dir, slot, saving, &chosen);
        if (i == 0 && ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
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
    else if (ui::button("Back", ImVec2(sp(14.f), sp(5.5f)), false, true, ui::Icon::ChevronL)) g_page = QPage::Main;
}

void check_ack() {
    if (!g_pending) return;
    std::string txt;
    runctl::Ack a;
    if (runctl::read_text(runctl::ack_path_for(A.run.ctl_path), &txt) && runctl::parse_ack(txt, &a) && a.seq == g_pending) {
        g_pending = 0;
        if (a.ok) {
            push_toast(g_pending_what, COL_GOOD, 3.0);
            if (g_pending_close) { g_page = QPage::Main; ImGui::CloseCurrentPopup(); focus_game(); }
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
        case hub::System::N64: case hub::System::GBA: case hub::System::GB: case hub::System::GBC: case hub::System::NDS: case hub::System::PS1: return true;
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

void qm_open_live() {
    if (!A.proc || A.run.key.empty()) return;
    const hub::GameEntry* e = find_entry(A.run.key);
    g_sys = e ? sys_short(*e) : A.run.system_short;
    g_key = A.run.key;
    g_title = e ? e->title : A.run.title;
    g_page = QPage::Main;
    g_pending = 0;
    g_open_req = true;
    focus_hub();
}

void qm_tick() {
    hub::saves::set_backup_screenshots(A.st.backup_shots);
    static const char* shot = std::getenv("HUB_SHOT_QM");   // screenshot aid: main | save | load (opens on a stand-in running game)
    static bool done = false;
    if (shot && !done && g_headless) {
        done = true;
        g_shot_fake = true;
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

void draw_quick_menu() {
    qm_tick();
    if (g_open_req) { g_open_req = false; ImGui::OpenPopup("##quickmenu"); }
    ImGuiIO& io = ImGui::GetIO();
    float w = std::min(g_page == QPage::Main ? sp(52.f) : sp(104.f), io.DisplaySize.x * 0.94f);
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(2.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal("##quickmenu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar(2);
    if (!open) return;
    if (!g_shot_fake && !A.proc) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    if (ui::modal_back()) {
        if (g_page != QPage::Main) g_page = QPage::Main;
        else { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); focus_game(); return; }
    }
    check_ack();

    const bool online = g_shot_fake ? false : A.run.online;
    const bool ctl = g_shot_fake || ctl_ready();
    const hub::GameEntry* e = g_shot_fake ? nullptr : find_entry(g_key);

    if (g_page != QPage::Main) {
        slot_page(g_page == QPage::Save);
        ImGui::EndPopup();
        return;
    }
    ImGui::PushFont(ui::F.title);
    ImGui::TextUnformatted("Game menu");
    ImGui::PopFont();
    ImGui::TextDisabled("%s", g_title.c_str());
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, sp(0.75f)));

    if (menu_button("Resume", true, ui::Icon::Play, true)) { ImGui::CloseCurrentPopup(); focus_game(); }
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
    if (ctl) {
        if (menu_button("Save state", !online, ui::Icon::Check)) g_page = QPage::Save;
        if (menu_button("Load state", !online, ui::Icon::Refresh)) g_page = QPage::Load;
        if (online) text_wrapped_disabled("Save states are off in online games: everyone has to stay in step.");
        if (menu_button("Take a screenshot", true, ui::Icon::Search)) send(runctl::Cmd::Screenshot, 0, "Screenshot saved", true);
    }
    if (e && cheats_supported(*e)) {
        if (menu_button("Cheats", true, ui::Icon::Bolt)) { hub::GameEntry c = *e; cheats_open(c); ImGui::CloseCurrentPopup(); }
    }
    if (!g_shot_fake && e && e->system == hub::System::PS1 && ps1_swap_available(e->key)) {
        if (menu_button("Swap disc", true, ui::Icon::Refresh)) { ps1_swap_disc(e->key); push_toast("Swapping to the next disc", COL_GOOD, 3.0); ImGui::CloseCurrentPopup(); focus_game(); }
    }
    if (g_shot_fake || fr_in_my_room()) fr_invite_button_and_picker(ImGui::GetContentRegionAvail().x, sp(6.5f));
    if (menu_button("Quit to RetroHub", true, ui::Icon::Close)) {
        if (!g_shot_fake) { if (ctl_ready()) runctl::send_command(A.run.ctl_path, runctl::Cmd::Quit, 0); else request_game_close(); }
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::TextDisabled("Press B to go back to the game.");
    ImGui::EndPopup();
}

/* ---------------- screenshots gallery (game details modal) ---------------- */

static bool is_image(const std::string& n) {
    auto dot = n.rfind('.');
    if (dot == std::string::npos) return false;
    std::string x = n.substr(dot);
    for (auto& c : x) c = (char)tolower((unsigned char)c);
    return x == ".png" || x == ".bmp";
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
    ImGui::PushFont(ui::F.bold);
    ImGui::Text("Screenshots");
    ImGui::PopFont();
    ImGui::SameLine(0, sp(1.f));
    ImGui::TextDisabled("%d", (int)s_files.size());
    if (s_files.empty()) {
        text_wrapped_disabled("None yet. Press F12 in a game, or choose \"Take a screenshot\" from the menu (View + Y on a controller).");
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        return;
    }
    ImGui::SameLine(0, sp(2.f));
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
        if (open_it) ShellExecuteW(nullptr, L"open", std::filesystem::u8path(path).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::PopID();
        ImGui::SameLine(0, sp(1.f));
    }
    ImGui::EndChild();
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
