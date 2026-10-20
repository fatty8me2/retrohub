/* ClassicHub.exe - the launcher front door.
 *
 * Layout: full-window custom UI (no floating windows): left sidebar nav + content pages
 * (Library, Play Online, Controllers, Settings, About). Drawn with Dear ImGui on SDL_Renderer.
 *
 * Launch contracts:
 *  - Emulated games:  n64run.exe --rom <path> --game <id> [--net <server> --room <CODE>-<launch>
 *                     --slot <lobby slot> --players <n> --ports <p0,p1,p2,p3> --delay <d>]
 *  - Smash (native):  BattleShip.exe --from-hub   (ALWAYS passed so it skips handing back to the hub)
 *                     Online, environment variables:
 *                       SSB64_LOBBY_HOST=<server>  (host)   or  SSB64_LOBBY_JOIN=<server> (joiners)
 *                       SSB64_LOBBY_CODE=<CODE>             (BattleShip joins/hosts this exact code;
 *                                                           players pick fighter/stage in its Online window)
 *                       (no autostart: the host presses Start match in Smash)
 */
#define SDL_MAIN_HANDLED
#define IMGUI_DEFINE_MATH_OPERATORS
#include <SDL.h>
#include <SDL_syswm.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <windows.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "games.h"
#include "logic.h"
#include "session.h"

#ifndef HUB_VERSION_STR
#define HUB_VERSION_STR "0.0.0"
#endif

using hub::GameInfo;
using hub::RomFile;

/* ============================ helpers ============================ */

static float S = 1.0f;  // DPI scale

static std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string U(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static bool file_exists(const std::string& p) {
    if (p.empty()) return false;
    DWORD a = GetFileAttributesW(W(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::string dir_of(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? std::string(".") : p.substr(0, i + 1);
}
static std::string file_name(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}

/* IFileOpenDialog: folder or file picker. Returns "" on cancel. */
static std::string pick_path(HWND owner, bool folder, const wchar_t* filter_name, const wchar_t* filter_spec) {
    std::string result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0);
    dlg->SetOptions(opts);
    if (!folder && filter_name) {
        COMDLG_FILTERSPEC f[1] = {{filter_name, filter_spec}};
        dlg->SetFileTypes(1, f);
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                result = U(p);
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    dlg->Release();
    return result;
}

/* ============================ drawing helpers ============================ */

static ImU32 C(float r, float g, float b, float a = 1.f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)); }
static ImU32 mix(ImU32 a, ImU32 b, float t) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                                 x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}
static ImU32 with_alpha(ImU32 c, float a) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w = a;
    return ImGui::ColorConvertFloat4ToU32(v);
}

static const ImU32 COL_ACCENT = IM_COL32(98, 118, 255, 255);
static const ImU32 COL_GOOD = IM_COL32(52, 190, 110, 255);
static const ImU32 COL_BAD = IM_COL32(220, 90, 80, 255);
static const ImU32 COL_WARN = IM_COL32(235, 170, 60, 255);
static const ImU32 COL_PANEL = IM_COL32(32, 33, 40, 255);
static const ImU32 COL_PANEL2 = IM_COL32(42, 44, 54, 255);

/* Rounded rect filled with a diagonal gradient (recolours the vertices AddRectFilled produced). */
static void gradient_round_rect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1, float rounding) {
    int start = dl->VtxBuffer.Size;
    dl->AddRectFilled(p0, p1, IM_COL32_WHITE, rounding);
    float w = std::max(1.f, p1.x - p0.x), h = std::max(1.f, p1.y - p0.y);
    for (int i = start; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        float t = ((v.pos.x - p0.x) / w + (v.pos.y - p0.y) / h) * 0.5f;
        t = std::min(1.f, std::max(0.f, t));
        ImU32 a = v.col >> 24;
        v.col = (mix(c0, c1, t) & 0x00FFFFFF) | (a << 24);
    }
}

/* Pill at an absolute position; returns its width. */
static float pill_at(ImDrawList* dl, ImVec2 p, const char* text, ImU32 bg, ImU32 fg = IM_COL32_WHITE) {
    ImVec2 ts = ImGui::CalcTextSize(text);
    float px = 10 * S, py = 3 * S;
    ImVec2 sz(ts.x + 2 * px, ts.y + 2 * py);
    dl->AddRectFilled(p, p + sz, bg, sz.y * 0.5f);
    dl->AddText(p + ImVec2(px, py), fg, text);
    return sz.x;
}
static void pill(const char* text, ImU32 bg, ImU32 fg = IM_COL32_WHITE) {
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = pill_at(ImGui::GetWindowDrawList(), p, text, bg, fg);
    ImGui::Dummy(ImVec2(w, ts.y + 6 * S));
}

static bool button(const char* label, ImVec2 size, bool primary = false, bool enabled = true) {
    if (!enabled) ImGui::BeginDisabled();
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, COL_ACCENT);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mix(COL_ACCENT, IM_COL32_WHITE, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, mix(COL_ACCENT, IM_COL32_BLACK, 0.2f));
    }
    bool r = ImGui::Button(label, size);
    if (primary) ImGui::PopStyleColor(3);
    if (!enabled) ImGui::EndDisabled();
    return r;
}

/* Per-game colour pair (text/gradients only, no art). */
static void game_colors(const std::string& id, ImU32* a, ImU32* b) {
    if (id == "smash64") { *a = IM_COL32(214, 48, 49, 255); *b = IM_COL32(250, 150, 40, 255); }
    else if (id == "mp1") { *a = IM_COL32(30, 70, 200, 255); *b = IM_COL32(70, 170, 245, 255); }
    else if (id == "mp2") { *a = IM_COL32(110, 45, 190, 255); *b = IM_COL32(205, 95, 225, 255); }
    else if (id == "mp3") { *a = IM_COL32(20, 130, 70, 255); *b = IM_COL32(125, 205, 80, 255); }
    else if (id == "remix") { *a = IM_COL32(0, 130, 150, 255); *b = IM_COL32(60, 220, 230, 255); }
    else { *a = IM_COL32(70, 78, 100, 255); *b = IM_COL32(120, 130, 160, 255); }
}
static const char* short_name(const std::string& id) {
    if (id == "smash64") return "Smash";
    if (id == "mp1") return "MP1";
    if (id == "mp2") return "MP2";
    if (id == "mp3") return "MP3";
    if (id == "remix") return "Remix";
    return "?";
}

/* ============================ app state ============================ */

enum class Page { Library, Online, Controllers, Settings, About };

struct ChatLine { std::string who, text; };

struct App {
    SDL_Window* win = nullptr;
    HWND hwnd = nullptr;
    hubui::Settings st;
    std::string settings_path;
    bool dirty = false;

    Page page = Page::Library;
    ImFont *f_body = nullptr, *f_title = nullptr, *f_h2 = nullptr, *f_big = nullptr;

    // ROM scan
    std::thread scan_th;
    std::atomic<bool> scanning{false}, scan_ready{false};
    std::vector<RomFile> scan_result;
    std::map<std::string, RomFile> game_rom;
    std::vector<RomFile> other_roms;

    // lobby
    hubnet::LobbyClient lobby;
    bool in_room = false;
    std::string code;
    std::string join_buf;
    std::map<int, hubui::PeerInfo> peers;
    std::map<int, bool> ready;
    std::vector<int> last_slots;
    bool hello_dirty = false, was_connected = false;
    bool my_ready = false;
    std::string selected_game;   // room's chosen game
    int launch_counter = 0;
    std::vector<ChatLine> chat;
    char chat_buf[256] = {0};
    bool chat_scroll = false;

    // child process
    HANDLE proc = nullptr;
    std::string proc_title;

    // ui
    std::string sel_game;        // library selection
    std::map<std::string, float> card_anim;
    std::map<int, float> nav_anim;
    std::string notice;
    double notice_until = 0;

    // controllers
    std::vector<SDL_GameController*> pads;
    int pad_sel = 0;
} A;

static void set_notice(const std::string& s, double secs = 8.0) {
    A.notice = s;
    A.notice_until = ImGui::GetTime() + secs;
}
static void mark_dirty() { A.dirty = true; }

static std::string server_url() {
    return A.st.server.empty() ? std::string(hubnet::default_server()) : A.st.server;
}
static std::string smash_exe_path() {
    if (file_exists(A.st.smash_exe)) return A.st.smash_exe;
    if (const GameInfo* g = hub::find_game("smash64")) {
        std::string p = hub::exe_dir() + g->native_exe;
        if (file_exists(p)) return p;
    }
    return "";
}
static std::string n64run_path() { return hub::exe_dir() + "n64run.exe"; }

/* ---------- ROM scanning ---------- */

static void start_scan() {
    if (A.scanning) return;
    if (A.scan_th.joinable()) A.scan_th.join();
    if (A.st.rom_folder.empty()) return;
    A.scanning = true;
    std::string folder = A.st.rom_folder;
    A.scan_th = std::thread([folder]() {
        A.scan_result = hub::scan_roms(folder);
        A.scan_ready = true;
    });
}
static void apply_scan() {
    A.game_rom.clear();
    A.other_roms.clear();
    for (auto& r : A.scan_result) {
        if (!r.game_id.empty()) A.game_rom.emplace(r.game_id, r);
        else A.other_roms.push_back(r);
    }
    A.hello_dirty = true;
}

static bool owns_game(const GameInfo& g) {
    return g.engine == hub::Engine::Native ? !smash_exe_path().empty() : A.game_rom.count(g.id) > 0;
}
static std::map<std::string, std::string> my_games() {
    std::map<std::string, std::string> m;
    for (auto& g : hub::catalog()) {
        if (g.engine == hub::Engine::Native) { if (!smash_exe_path().empty()) m[g.id] = "native"; }
        else if (A.game_rom.count(g.id)) m[g.id] = A.game_rom[g.id].md5;
    }
    return m;
}

/* ---------- process launching ---------- */

static bool spawn(const std::string& exe, const std::string& args, const std::string& cwd,
                  const std::vector<std::pair<std::string, std::string>>& env, const std::string& title) {
    if (A.proc) { set_notice("A game is already running."); return false; }
    if (!file_exists(exe)) { set_notice("Cannot find " + file_name(exe) + " (" + exe + ")"); return false; }
    std::wstring cmd = L"\"" + W(exe) + L"\" " + W(args);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    for (auto& e : env) SetEnvironmentVariableW(W(e.first).c_str(), W(e.second).c_str());
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(W(exe).c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, W(cwd).c_str(), &si, &pi);
    DWORD err = GetLastError();
    for (auto& e : env) SetEnvironmentVariableW(W(e.first).c_str(), nullptr);
    if (!ok) { set_notice("Could not start " + file_name(exe) + " (error " + std::to_string(err) + ")"); return false; }
    CloseHandle(pi.hThread);
    A.proc = pi.hProcess;
    A.proc_title = title;
    return true;
}

static void poll_process() {
    if (!A.proc) return;
    if (WaitForSingleObject(A.proc, 0) != WAIT_OBJECT_0) return;
    DWORD code = 0;
    GetExitCodeProcess(A.proc, &code);
    CloseHandle(A.proc);
    A.proc = nullptr;
    bool native = (A.proc_title.find("Smash") != std::string::npos);
    set_notice(A.proc_title + ": " + (native && code == 0 ? "closed." : hubui::exit_code_text(code)), 20.0);
}

static void play_local(const GameInfo& g) {
    A.st.last_game = g.id;
    mark_dirty();
    if (g.engine == hub::Engine::Native) {
        std::string exe = smash_exe_path();
        spawn(exe, "--from-hub", dir_of(exe), {}, g.title);
    } else {
        auto it = A.game_rom.find(g.id);
        if (it == A.game_rom.end()) { set_notice("ROM for " + g.title + " not found."); return; }
        spawn(n64run_path(), "--rom \"" + it->second.path + "\" --game " + g.id, hub::exe_dir(), {}, g.title);
    }
}

static void launch_online(const GameInfo& g, int launch_no, int delay, bool is_host) {
    A.st.last_game = g.id;
    mark_dirty();
    if (g.engine == hub::Engine::Native) {
        std::string exe = smash_exe_path();
        std::vector<std::pair<std::string, std::string>> env;
        env.push_back({is_host ? "SSB64_LOBBY_HOST" : "SSB64_LOBBY_JOIN", server_url()});
        env.push_back({"SSB64_LOBBY_CODE", A.code});
        spawn(exe, "--from-hub", dir_of(exe), env, g.title);
        return;
    }
    auto it = A.game_rom.find(g.id);
    if (it == A.game_rom.end()) { set_notice("You don't have the ROM for " + g.title + "."); return; }
    std::vector<int> slots;
    for (auto& m : A.lobby.members()) slots.push_back(m.slot);
    std::string args = "--rom \"" + it->second.path + "\" --game " + g.id + " --net \"" + server_url() +
                       "\" --room " + A.code + "-" + std::to_string(launch_no) + " --slot " +
                       std::to_string(A.lobby.my_slot()) + " --players " + std::to_string(slots.size()) +
                       " --ports " + hubui::ports_arg(slots) + " --delay " + std::to_string(delay);
    spawn(n64run_path(), args, hub::exe_dir(), {}, g.title);
}

/* ---------- lobby ---------- */

static std::string member_name(int slot) {
    for (auto& m : A.lobby.members()) if (m.slot == slot) return m.name;
    return "Player " + std::to_string(slot + 1);
}

static void leave_room() {
    A.lobby.stop();
    A.in_room = false;
    A.peers.clear();
    A.ready.clear();
    A.last_slots.clear();
    A.chat.clear();
    A.selected_game.clear();
    A.my_ready = false;
    A.was_connected = false;
    A.code.clear();
}

static void enter_room(bool host, const std::string& code) {
    if (A.st.name.empty()) { set_notice("Pick a player name in Settings first."); return; }
    if (A.proc) { set_notice("Close the running game first."); return; }
    leave_room();
    A.code = code;
    if (!A.lobby.start(server_url(), code, host, A.st.name)) {
        set_notice("Could not connect to the lobby server.");
        return;
    }
    A.in_room = true;
    A.page = Page::Online;
    if (host && !A.st.last_game.empty()) A.selected_game = A.st.last_game;
}

static bool is_host() { return A.lobby.connected() && A.lobby.my_slot() == 0; }

static std::vector<hubui::PeerInfo> peer_list() {
    std::vector<hubui::PeerInfo> v;
    int me = A.lobby.my_slot();
    for (auto& m : A.lobby.members()) {
        hubui::PeerInfo p;
        auto it = A.peers.find(m.slot);
        if (it != A.peers.end()) p = it->second;
        if (m.slot == me) { p.hello = true; p.games = my_games(); }
        p.slot = m.slot;
        p.name = m.name;
        v.push_back(p);
    }
    return v;
}

static void lobby_tick() {
    if (!A.in_room) return;
    A.lobby.update();
    if (!A.lobby.connected()) return;

    // Membership changes -> forget departed peers, re-announce ourselves.
    std::vector<int> slots;
    for (auto& m : A.lobby.members()) slots.push_back(m.slot);
    std::sort(slots.begin(), slots.end());
    bool changed = !A.was_connected || slots != A.last_slots;
    if (changed) {
        std::set<int> alive(slots.begin(), slots.end());
        for (auto it = A.peers.begin(); it != A.peers.end();) it = alive.count(it->first) ? std::next(it) : A.peers.erase(it);
        for (auto it = A.ready.begin(); it != A.ready.end();) it = alive.count(it->first) ? std::next(it) : A.ready.erase(it);
        A.last_slots = slots;
        A.hello_dirty = true;
        A.was_connected = true;
    }
    if (A.hello_dirty) {
        A.hello_dirty = false;
        A.lobby.send_msg(hubui::hello_json("1", my_games()));
        A.lobby.send_msg(std::string("{\"kind\":\"ready\",\"ready\":") + (A.my_ready ? "true" : "false") + "}");
        if (is_host() && !A.selected_game.empty())
            A.lobby.send_msg("{\"kind\":\"select\",\"game\":\"" + hubui::json_escape(A.selected_game) + "\"}");
    }

    int from;
    std::string body;
    while (A.lobby.poll_msg(&from, &body)) {
        hubui::Json j;
        if (!hubui::json_parse(body, &j) || j.type != hubui::Json::Obj) continue;
        std::string kind = j.str("kind");
        if (kind == "hello") {
            hubui::PeerInfo& p = A.peers[from];
            hubui::parse_hello(j, &p);
        } else if (kind == "select") {
            if (from == 0) A.selected_game = j.str("game");
        } else if (kind == "ready") {
            const hubui::Json* r = j.get("ready");
            A.ready[from] = r && r->type == hubui::Json::Bool && r->b;
        } else if (kind == "chat") {
            A.chat.push_back({member_name(from), j.str("text")});
            if (A.chat.size() > 200) A.chat.erase(A.chat.begin());
            A.chat_scroll = true;
        } else if (kind == "launch") {
            if (from != 0 || is_host()) continue;
            const GameInfo* g = hub::find_game(j.str("game"));
            if (!g) continue;
            if (!owns_game(*g)) { set_notice("The host started " + g->title + " but you don't have it."); continue; }
            if (A.proc) continue;
            launch_online(*g, (int)j.num("launch", 1), (int)j.num("delay", 2), false);
        }
    }
}

/* ============================ theme / fonts ============================ */

static void setup_theme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 12;
    s.ChildRounding = 12;
    s.FrameRounding = 8;
    s.PopupRounding = 10;
    s.GrabRounding = 8;
    s.ScrollbarRounding = 8;
    s.WindowPadding = ImVec2(16, 16);
    s.FramePadding = ImVec2(14, 10);
    s.ItemSpacing = ImVec2(12, 12);
    s.ItemInnerSpacing = ImVec2(10, 8);
    s.ScrollbarSize = 12;
    s.FrameBorderSize = 0;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.ScaleAllSizes(scale);

    ImVec4* c = s.Colors;
    auto cv = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
    c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.078f, 0.094f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.125f, 0.15f, 1);
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.97f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.57f, 0.65f, 1);
    c[ImGuiCol_Button] = cv(COL_PANEL2);
    c[ImGuiCol_ButtonHovered] = cv(mix(COL_PANEL2, COL_ACCENT, 0.45f));
    c[ImGuiCol_ButtonActive] = cv(mix(COL_PANEL2, COL_ACCENT, 0.7f));
    c[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.115f, 0.14f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.16f, 0.2f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.17f, 0.18f, 0.24f, 1);
    c[ImGuiCol_SliderGrab] = cv(COL_ACCENT);
    c[ImGuiCol_SliderGrabActive] = cv(mix(COL_ACCENT, IM_COL32_WHITE, 0.25f));
    c[ImGuiCol_CheckMark] = cv(COL_ACCENT);
    c[ImGuiCol_Header] = cv(with_alpha(COL_ACCENT, 0.35f));
    c[ImGuiCol_HeaderHovered] = cv(with_alpha(COL_ACCENT, 0.55f));
    c[ImGuiCol_HeaderActive] = cv(with_alpha(COL_ACCENT, 0.75f));
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.26f, 0.32f, 1);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_NavCursor] = cv(IM_COL32(255, 255, 255, 230));
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
}

static void setup_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    const char* reg = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* bold = "C:\\Windows\\Fonts\\segoeuib.ttf";
    bool have_reg = GetFileAttributesA(reg) != INVALID_FILE_ATTRIBUTES;
    bool have_bold = GetFileAttributesA(bold) != INVALID_FILE_ATTRIBUTES;
    auto load = [&](bool b, float px) -> ImFont* {
        const char* p = b ? (have_bold ? bold : reg) : reg;
        if (have_reg || (b && have_bold)) return io.Fonts->AddFontFromFileTTF(p, px * scale);
        return nullptr;
    };
    A.f_body = load(false, 19);
    if (!A.f_body) {
        // Fallback: built-in font, enlarged.
        ImFontConfig cfg;
        cfg.SizePixels = 19 * scale;
        A.f_body = io.Fonts->AddFontDefault(&cfg);
        A.f_title = A.f_h2 = A.f_big = A.f_body;
        return;
    }
    A.f_title = load(true, 34);
    A.f_h2 = load(true, 24);
    A.f_big = load(true, 84);
    if (!A.f_title) A.f_title = A.f_body;
    if (!A.f_h2) A.f_h2 = A.f_body;
    if (!A.f_big) A.f_big = A.f_body;
}

/* ============================ widgets / pages ============================ */

static void page_header(const char* title, const char* sub) {
    ImGui::PushFont(A.f_title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (sub && *sub) ImGui::TextDisabled("%s", sub);
    ImGui::Dummy(ImVec2(0, 6 * S));
}

static void begin_panel(const char* id, ImVec2 size = ImVec2(0, 0), ImU32 bg = COL_PANEL) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * S, 18 * S));
    ImGuiChildFlags fl = ImGuiChildFlags_AlwaysUseWindowPadding;
    if (size.y == 0) fl |= ImGuiChildFlags_AutoResizeY;
    ImGui::BeginChild(id, size, fl);
}
static void end_panel() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

static void text_wrapped_disabled(const std::string& s) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::PopStyleColor();
}

static void choose_rom_folder() {
    std::string p = pick_path(A.hwnd, true, nullptr, nullptr);
    if (p.empty()) return;
    A.st.rom_folder = p;
    mark_dirty();
    start_scan();
}
static void locate_smash() {
    std::string p = pick_path(A.hwnd, false, L"BattleShip.exe", L"*.exe");
    if (p.empty()) return;
    A.st.smash_exe = p;
    mark_dirty();
    A.hello_dirty = true;
}

/* ---------- Library ---------- */

static void game_card(const GameInfo& g, ImVec2 size) {
    ImGui::PushID(g.id.c_str());
    ImGui::InvisibleButton("card", size);
    bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    if (ImGui::IsItemClicked() || ImGui::IsItemActivated()) { A.sel_game = g.id; }
    if (ImGui::IsItemFocused() && A.sel_game != g.id && ImGui::GetIO().NavActive) A.sel_game = g.id;
    float& an = A.card_anim[g.id];
    an += ((hov ? 1.f : 0.f) - an) * std::min(1.f, ImGui::GetIO().DeltaTime * 14.f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float grow = an * 6 * S;
    ImVec2 p0 = ImGui::GetItemRectMin() - ImVec2(grow, grow), p1 = ImGui::GetItemRectMax() + ImVec2(grow, grow);
    float r = 18 * S;
    bool owned = owns_game(g);

    ImU32 c0, c1;
    game_colors(g.id, &c0, &c1);
    if (!owned) { c0 = mix(c0, IM_COL32(40, 42, 52, 255), 0.45f); c1 = mix(c1, IM_COL32(40, 42, 52, 255), 0.45f); }

    dl->AddRectFilled(p0 + ImVec2(0, 6 * S), p1 + ImVec2(0, 10 * S), IM_COL32(0, 0, 0, (int)(60 + 60 * an)), r + 4 * S);
    gradient_round_rect(dl, p0, p1, c0, c1, r);
    dl->PushClipRect(p0, p1, true);
    dl->AddCircleFilled(ImVec2(p1.x - 40 * S, p0.y + 50 * S), 70 * S, IM_COL32(255, 255, 255, 28), 48);
    dl->AddCircleFilled(ImVec2(p1.x - 10 * S, p1.y - 10 * S), 50 * S, IM_COL32(0, 0, 0, 30), 48);
    dl->PopClipRect();
    if (hov) dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 14), r);
    if (A.sel_game == g.id) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 235), r, 0, 3 * S);

    float pad = 16 * S;
    // engine badge
    bool native = g.engine == hub::Engine::Native;
    pill_at(dl, p0 + ImVec2(pad, pad * 0.8f), native ? "NATIVE" : "EMULATED", IM_COL32(0, 0, 0, 110));
    // title
    dl->AddText(A.f_h2, A.f_h2->FontSize, p0 + ImVec2(pad, pad * 0.8f + 38 * S), IM_COL32_WHITE, g.title.c_str(), nullptr,
                size.x - 2 * pad);
    // bottom row: players + status
    char pl[32];
    std::snprintf(pl, sizeof pl, "1-%d players", g.max_players);
    ImVec2 pty = ImVec2(p0.x + pad, p1.y - pad - ImGui::GetFontSize() - 6 * S);
    float w = pill_at(dl, pty, owned ? "Ready" : "ROM missing", owned ? COL_GOOD : IM_COL32(25, 25, 30, 200));
    dl->AddText(ImVec2(pty.x + w + 12 * S, pty.y + 3 * S), IM_COL32(255, 255, 255, 220), pl);
    ImGui::PopID();
}

static void page_library() {
    page_header("Library", "Pick a game, then play locally or host an online room.");

    if (button("Choose ROM folder", ImVec2(0, 0), A.st.rom_folder.empty())) choose_rom_folder();
    ImGui::SameLine();
    if (button("Rescan", ImVec2(0, 0), false, !A.st.rom_folder.empty() && !A.scanning)) start_scan();
    ImGui::SameLine();
    if (A.scanning) ImGui::TextDisabled("Scanning...");
    else if (A.st.rom_folder.empty()) ImGui::TextDisabled("No ROM folder chosen yet");
    else ImGui::TextDisabled("%s  (%d games found)", A.st.rom_folder.c_str(), (int)A.game_rom.size());
    ImGui::Dummy(ImVec2(0, 8 * S));

    const auto& cat = hub::catalog();
    if (A.sel_game.empty()) A.sel_game = A.st.last_game;
    if (!hub::find_game(A.sel_game) && !cat.empty()) A.sel_game = cat[0].id;

    float avail = ImGui::GetContentRegionAvail().x;
    float gap = 20 * S, min_w = 250 * S;
    int cols = std::max(1, (int)((avail + gap) / (min_w + gap)));
    float cw = std::min(340 * S, (avail - gap * (cols - 1)) / cols);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    for (size_t i = 0; i < cat.size(); ++i) {
        if (i % cols) ImGui::SameLine();
        game_card(cat[i], ImVec2(cw, 190 * S));
    }
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, 8 * S));

    // details panel
    const GameInfo* g = hub::find_game(A.sel_game);
    if (g) {
        bool native = g->engine == hub::Engine::Native;
        bool owned = owns_game(*g);
        ImU32 c0, c1;
        game_colors(g->id, &c0, &c1);
        begin_panel("details");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        dl->AddRectFilled(wp + ImVec2(0, 18 * S), wp + ImVec2(5 * S, ImGui::GetWindowHeight() - 18 * S), c1, 3 * S);
        ImGui::PushFont(A.f_h2);
        ImGui::TextUnformatted(g->title.c_str());
        ImGui::PopFont();
        pill(native ? "NATIVE PORT" : "EMULATED", COL_PANEL2);
        ImGui::SameLine();
        pill(owned ? "Ready" : "ROM missing", owned ? COL_GOOD : mix(COL_BAD, IM_COL32_BLACK, 0.3f));
        ImGui::SameLine();
        char pl[32];
        std::snprintf(pl, sizeof pl, "1-%d players", g->max_players);
        pill(pl, COL_PANEL2);
        ImGui::Dummy(ImVec2(0, 4 * S));
        if (owned) {
            std::string where = native ? smash_exe_path() : A.game_rom[g->id].path;
            text_wrapped_disabled(where);
            ImGui::Dummy(ImVec2(0, 4 * S));
            bool idle = A.proc == nullptr;
            if (button("Play Locally", ImVec2(190 * S, 52 * S), true, idle)) play_local(*g);
            ImGui::SameLine();
            if (button("Host Online", ImVec2(190 * S, 52 * S), false, idle)) {
                A.st.last_game = g->id;
                mark_dirty();
                enter_room(true, hubnet::new_room_code());
            }
            if (native) {
                ImGui::SameLine();
                if (button("Locate BattleShip.exe", ImVec2(0, 52 * S))) locate_smash();
            }
        } else if (native) {
            text_wrapped_disabled("BattleShip.exe was not found under games/smashClassic/ next to the hub. "
                                  "Place the Smash Classic folder there, or browse to BattleShip.exe.");
            if (button("Locate BattleShip.exe", ImVec2(0, 48 * S), true)) locate_smash();
        } else {
            text_wrapped_disabled("No matching ROM found. Choose the folder that holds your own " + g->title +
                                  " dump (.z64, .v64 or .n64). Roms are matched by their checksum, not file name.");
            if (button("Choose ROM folder", ImVec2(0, 48 * S), true)) choose_rom_folder();
        }
        end_panel();
    }

    if (!A.other_roms.empty()) {
        ImGui::Dummy(ImVec2(0, 8 * S));
        ImGui::PushFont(A.f_h2);
        ImGui::TextUnformatted("Other ROMs");
        ImGui::PopFont();
        text_wrapped_disabled("Valid N64 ROMs that Classic Hub does not support yet.");
        for (auto& r : A.other_roms) {
            std::string label = (r.header_name.empty() ? file_name(r.path) : r.header_name) + "   -   " + file_name(r.path) + "   (not supported yet)";
            ImGui::TextDisabled("%s", label.c_str());
        }
    }
}

/* ---------- Online ---------- */

static void member_card(const hubui::PeerInfo* p, int slot, ImVec2 size) {
    ImGui::BeginChild(("m" + std::to_string(slot)).c_str(), size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetWindowPos(), p1 = p0 + size;
    static const ImU32 slotc[4] = {IM_COL32(250, 150, 40, 255), IM_COL32(70, 170, 245, 255), IM_COL32(205, 95, 225, 255), IM_COL32(125, 205, 80, 255)};
    if (!p) {
        dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 40), 14 * S, 0, 1.5f * S);
        const char* t = "Open slot";
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(p0 + (size - ts) * 0.5f, IM_COL32(255, 255, 255, 80), t);
        ImGui::EndChild();
        return;
    }
    dl->AddRectFilled(p0, p1, COL_PANEL, 14 * S);
    ImVec2 cc = p0 + ImVec2(38 * S, 40 * S);
    dl->AddCircleFilled(cc, 24 * S, slotc[slot & 3], 32);
    char init[2] = {(char)std::toupper((unsigned char)(p->name.empty() ? '?' : p->name[0])), 0};
    ImVec2 its = ImGui::CalcTextSize(init);
    dl->AddText(A.f_h2, A.f_h2->FontSize, cc - ImVec2(its.x * 0.6f, its.y * 0.62f), IM_COL32(20, 20, 25, 255), init);
    dl->AddText(A.f_h2, A.f_h2->FontSize, ImVec2(p0.x + 74 * S, p0.y + 14 * S), IM_COL32_WHITE, p->name.c_str());
    ImGui::SetCursorScreenPos(p0 + ImVec2(74 * S, 46 * S));
    bool host = slot == 0;
    bool rd = host || A.ready[slot];
    if (slot == A.lobby.my_slot()) rd = host || A.my_ready;
    if (host) pill("HOST", COL_ACCENT);
    else pill(rd ? "Ready" : "Not ready", rd ? COL_GOOD : IM_COL32(70, 72, 86, 255));
    // owned games
    ImGui::SetCursorScreenPos(p0 + ImVec2(16 * S, 88 * S));
    if (!p->hello) {
        ImGui::TextDisabled("checking games...");
    } else {
        bool first = true;
        for (auto& g : hub::catalog()) {
            if (!p->games.count(g.id)) continue;
            ImU32 a, b;
            game_colors(g.id, &a, &b);
            if (!first) ImGui::SameLine(0, 6 * S);
            first = false;
            pill(short_name(g.id), mix(a, IM_COL32_BLACK, 0.15f));
        }
        if (first) ImGui::TextDisabled("no supported games");
    }
    ImGui::EndChild();
}

static void page_online_home() {
    page_header("Play Online", "Host a room and share the code, or join a friend's room.");
    float avail = ImGui::GetContentRegionAvail().x;
    float gap = 20 * S;
    bool two = avail > 760 * S;
    float w = two ? (avail - gap) / 2 : avail;

    begin_panel("host", ImVec2(w, 0));
    ImGui::PushFont(A.f_h2);
    ImGui::TextUnformatted("Host a room");
    ImGui::PopFont();
    text_wrapped_disabled("Create a room, pick a game everyone owns, and share the 6-letter code.");
    ImGui::Dummy(ImVec2(0, 8 * S));
    if (button("Host Room", ImVec2(-1, 64 * S), true, A.proc == nullptr)) enter_room(true, hubnet::new_room_code());
    end_panel();

    if (two) ImGui::SameLine();

    begin_panel("join", ImVec2(w, 0));
    ImGui::PushFont(A.f_h2);
    ImGui::TextUnformatted("Join a room");
    ImGui::PopFont();
    text_wrapped_disabled("Type the code your friend gave you.");
    ImGui::Dummy(ImVec2(0, 8 * S));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%s", A.join_buf.c_str());
    ImGui::PushFont(A.f_h2);
    ImGui::SetNextItemWidth(w * 0.5f);
    bool enter = ImGui::InputTextWithHint("##code", "ABC234", buf, 7, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopFont();
    A.join_buf = hubui::normalize_code(buf);
    bool valid = hubnet::valid_room_code(A.join_buf);
    ImGui::SameLine();
    bool go = button("Join", ImVec2(120 * S, 0), true, valid && A.proc == nullptr);
    if (!A.join_buf.empty() && !valid) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_WARN), "Codes are 6 letters/digits (no I, O, 0, 1).");
    if (go || (enter && valid)) enter_room(false, A.join_buf);
    end_panel();
}

static void page_online_room() {
    // Header: big code + actions
    ImGui::PushFont(A.f_big);
    std::string spaced;
    for (size_t i = 0; i < A.code.size(); ++i) { if (i) spaced += ' '; spaced += A.code[i]; }
    ImGui::PushStyleColor(ImGuiCol_Text, COL_ACCENT);
    ImGui::TextUnformatted(spaced.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine(0, 30 * S);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, 8 * S));
    if (button("Copy code", ImVec2(150 * S, 44 * S), true)) { SDL_SetClipboardText(A.code.c_str()); set_notice("Room code copied.", 3); }
    ImGui::SameLine();
    if (button("Leave room", ImVec2(150 * S, 44 * S))) { leave_room(); ImGui::EndGroup(); return; }
    ImGui::EndGroup();

    if (A.lobby.failed()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_BAD), "%s", A.lobby.status().c_str());
        return;
    }
    if (!A.lobby.connected()) {
        ImGui::TextDisabled("Connecting...  %s", A.lobby.status().c_str());
        return;
    }

    auto peers = peer_list();
    bool host = is_host();
    float total_w = ImGui::GetContentRegionAvail().x;
    float chat_w = std::max(280 * S, total_w * 0.34f);
    float left_w = total_w - chat_w - 20 * S;
    float h = ImGui::GetContentRegionAvail().y;
    if (h < 420 * S) h = 420 * S;

    ImGui::BeginChild("roomleft", ImVec2(left_w, h), ImGuiChildFlags_NavFlattened);

    // member cards (4 slots)
    float gap = 14 * S;
    int cols = left_w > 560 * S ? 2 : 1;
    float mw = (left_w - gap * (cols - 1)) / cols;
    std::map<int, const hubui::PeerInfo*> by_slot;
    for (auto& p : peers) by_slot[p.slot] = &p;
    for (int s = 0; s < 4; ++s) {
        if (s % cols) ImGui::SameLine(0, gap);
        member_card(by_slot.count(s) ? by_slot[s] : nullptr, s, ImVec2(mw, 124 * S));
    }

    ImGui::Dummy(ImVec2(0, 4 * S));
    if (!host) {
        bool before = A.my_ready;
        if (ImGui::Checkbox("I'm ready", &A.my_ready) && before != A.my_ready) A.hello_dirty = true;
    }
    ImGui::PushFont(A.f_h2);
    ImGui::TextUnformatted(host ? "Choose a game" : "Host's game");
    ImGui::PopFont();

    // game picker
    for (auto& g : hub::catalog()) {
        auto sel = hubui::game_selectable(g.id, g.max_players, peers, 0);
        bool chosen = A.selected_game == g.id;
        ImGui::PushID(g.id.c_str());
        ImVec2 sz(left_w, 54 * S);
        bool can_click = host && sel.ok;
        if (!can_click) ImGui::BeginDisabled();
        bool clicked = ImGui::InvisibleButton("row", sz);
        bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (!can_click) ImGui::EndDisabled();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
        ImU32 a, b;
        game_colors(g.id, &a, &b);
        dl->AddRectFilled(p0, p1, chosen ? with_alpha(a, 0.35f) : (hov ? COL_PANEL2 : COL_PANEL), 12 * S);
        if (chosen) dl->AddRect(p0, p1, b, 12 * S, 0, 2 * S);
        dl->AddRectFilled(p0 + ImVec2(10 * S, 10 * S), p0 + ImVec2(16 * S, 44 * S), sel.ok ? b : IM_COL32(90, 92, 105, 255), 3 * S);
        dl->AddText(p0 + ImVec2(30 * S, 13 * S), sel.ok ? IM_COL32_WHITE : IM_COL32(130, 132, 145, 255), g.title.c_str());
        if (!sel.ok) {
            ImVec2 ts = ImGui::CalcTextSize(sel.reason.c_str());
            dl->AddText(ImVec2(p1.x - ts.x - 16 * S, p0.y + 14 * S), mix(COL_BAD, IM_COL32_WHITE, 0.25f), sel.reason.c_str());
        } else if (chosen) {
            dl->AddText(ImVec2(p1.x - ImGui::CalcTextSize("Selected").x - 16 * S, p0.y + 14 * S), IM_COL32(255, 255, 255, 220), "Selected");
        }
        if (clicked && can_click) {
            A.selected_game = g.id;
            A.st.last_game = g.id;
            mark_dirty();
            A.lobby.send_msg("{\"kind\":\"select\",\"game\":\"" + hubui::json_escape(g.id) + "\"}");
        }
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0, 6 * S));
    if (host) {
        const GameInfo* g = hub::find_game(A.selected_game);
        bool ok = false;
        std::string why;
        if (!g) why = "Choose a game to start";
        else {
            auto s = hubui::game_selectable(g->id, g->max_players, peers, 0);
            ok = s.ok;
            why = s.reason;
            if (ok)
                for (auto& p : peers)
                    if (p.slot != 0 && !A.ready[p.slot]) { ok = false; why = p.name + " isn't ready yet"; break; }
        }
        if (A.proc) { ok = false; why = "In game..."; }
        if (button("Launch", ImVec2(220 * S, 60 * S), true, ok)) {
            int delay = A.st.input_delay;
            unsigned seed = std::random_device{}();
            ++A.launch_counter;
            A.lobby.send_msg(hubui::launch_json(g->id, A.launch_counter, delay, seed));
            launch_online(*g, A.launch_counter, delay, true);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", ok ? "Everyone is ready." : why.c_str());
    } else {
        const GameInfo* g = hub::find_game(A.selected_game);
        ImGui::TextDisabled("%s", g ? ("Waiting for the host to start " + g->title + "...").c_str() : "Waiting for the host to pick a game...");
    }
    ImGui::EndChild();

    // chat
    ImGui::SameLine(0, 20 * S);
    begin_panel("chat", ImVec2(chat_w, h));
    ImGui::PushFont(A.f_h2);
    ImGui::TextUnformatted("Chat");
    ImGui::PopFont();
    float input_h = ImGui::GetFrameHeight() + 8 * S;
    ImGui::BeginChild("chatlog", ImVec2(0, -input_h), ImGuiChildFlags_None);
    for (auto& l : A.chat) {
        ImGui::PushStyleColor(ImGuiCol_Text, COL_ACCENT);
        ImGui::TextUnformatted(l.who.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", l.text.c_str());
    }
    if (A.chat_scroll) { ImGui::SetScrollHereY(1.f); A.chat_scroll = false; }
    ImGui::EndChild();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##chat", "Say something...", A.chat_buf, sizeof A.chat_buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string t = A.chat_buf;
        A.chat_buf[0] = 0;
        if (!t.empty()) {
            A.lobby.send_msg("{\"kind\":\"chat\",\"text\":\"" + hubui::json_escape(t) + "\"}");
            A.chat.push_back({A.st.name, t});
            A.chat_scroll = true;
        }
        ImGui::SetKeyboardFocusHere(-1);
    }
    end_panel();
}

static void page_online() {
    if (A.in_room) page_online_room();
    else page_online_home();
}

/* ---------- Controllers ---------- */

static void refresh_pads() {
    for (auto* p : A.pads) SDL_GameControllerClose(p);
    A.pads.clear();
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i))
            if (SDL_GameController* c = SDL_GameControllerOpen(i)) A.pads.push_back(c);
    if (A.pad_sel >= (int)A.pads.size()) A.pad_sel = 0;
}

static void stick(ImDrawList* dl, ImVec2 c, float r, float x, float y, const char* label) {
    dl->AddCircleFilled(c, r, COL_PANEL2, 40);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 40), 40, 2);
    dl->AddCircleFilled(c + ImVec2(x, y) * (r * 0.7f), r * 0.28f, COL_ACCENT, 24);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(c + ImVec2(-ts.x * 0.5f, r + 6 * S), IM_COL32(150, 152, 165, 255), label);
}
static void btn_dot(ImDrawList* dl, ImVec2 c, float r, bool on, const char* label) {
    dl->AddCircleFilled(c, r, on ? COL_ACCENT : COL_PANEL2, 24);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(c - ts * 0.5f, on ? IM_COL32_WHITE : IM_COL32(150, 152, 165, 255), label);
}

static void page_controllers() {
    page_header("Controllers", "Connected game controllers and the N64 port each one will use.");
    if (A.pads.empty()) {
        begin_panel("nopad");
        ImGui::TextUnformatted("No game controllers detected.");
        text_wrapped_disabled("Plug in or pair a controller; it appears here immediately. The first controller becomes port 1, the second port 2, and so on.");
        end_panel();
        return;
    }
    begin_panel("padlist");
    for (size_t i = 0; i < A.pads.size(); ++i) {
        const char* nm = SDL_GameControllerName(A.pads[i]);
        char line[256];
        std::snprintf(line, sizeof line, "%d.  %s", (int)i, nm ? nm : "Controller");
        if (ImGui::Selectable(line, A.pad_sel == (int)i, 0, ImVec2(0, 36 * S))) A.pad_sel = (int)i;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 110 * S);
        if (i < 4) { char pt[16]; std::snprintf(pt, sizeof pt, "Port %d", (int)i + 1); pill(pt, COL_ACCENT); }
        else pill("unused", COL_PANEL2);
    }
    end_panel();

    ImGui::Dummy(ImVec2(0, 8 * S));
    ImGui::PushFont(A.f_h2);
    ImGui::TextUnformatted("Test input");
    ImGui::PopFont();
    ImGui::TextDisabled("Move sticks and press buttons to see them light up.");
    SDL_GameController* pad = A.pads[std::min(A.pad_sel, (int)A.pads.size() - 1)];
    ImVec2 o = ImGui::GetCursorScreenPos();
    float h = 230 * S;
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, o + ImVec2(ImGui::GetItemRectSize().x, h), COL_PANEL, 14 * S);
    auto ax = [&](SDL_GameControllerAxis a) { float v = SDL_GameControllerGetAxis(pad, a) / 32767.f; return std::max(-1.f, std::min(1.f, v)); };
    auto bt = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(pad, b) != 0; };
    float r = 48 * S;
    stick(dl, o + ImVec2(100 * S, 100 * S), r, ax(SDL_CONTROLLER_AXIS_LEFTX), ax(SDL_CONTROLLER_AXIS_LEFTY), "Left stick");
    stick(dl, o + ImVec2(360 * S, 130 * S), r * 0.8f, ax(SDL_CONTROLLER_AXIS_RIGHTX), ax(SDL_CONTROLLER_AXIS_RIGHTY), "Right stick (C)");
    float bx = 560 * S, by = 100 * S, br = 20 * S;
    btn_dot(dl, o + ImVec2(bx + 50 * S, by), br, bt(SDL_CONTROLLER_BUTTON_B), "B");
    btn_dot(dl, o + ImVec2(bx, by + 50 * S), br, bt(SDL_CONTROLLER_BUTTON_A), "A");
    btn_dot(dl, o + ImVec2(bx, by - 50 * S), br, bt(SDL_CONTROLLER_BUTTON_Y), "Y");
    btn_dot(dl, o + ImVec2(bx - 50 * S, by), br, bt(SDL_CONTROLLER_BUTTON_X), "X");
    float dx = 700 * S, dy = 100 * S;
    btn_dot(dl, o + ImVec2(dx, dy - 40 * S), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_DPAD_UP), "^");
    btn_dot(dl, o + ImVec2(dx, dy + 40 * S), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_DPAD_DOWN), "v");
    btn_dot(dl, o + ImVec2(dx - 40 * S, dy), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_DPAD_LEFT), "<");
    btn_dot(dl, o + ImVec2(dx + 40 * S, dy), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_DPAD_RIGHT), ">");
    btn_dot(dl, o + ImVec2(230 * S, 30 * S), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_LEFTSHOULDER), "L");
    btn_dot(dl, o + ImVec2(490 * S, 30 * S), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER), "R");
    btn_dot(dl, o + ImVec2(270 * S, 190 * S), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_BACK), "Bk");
    btn_dot(dl, o + ImVec2(330 * S, 190 * S), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_START), "St");
    // triggers
    float lt = ax(SDL_CONTROLLER_AXIS_TRIGGERLEFT), rt = ax(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    dl->AddRectFilled(o + ImVec2(20 * S, 200 * S), o + ImVec2(20 * S + 100 * S, 212 * S), COL_PANEL2, 6 * S);
    dl->AddRectFilled(o + ImVec2(20 * S, 200 * S), o + ImVec2(20 * S + 100 * S * std::max(0.f, lt), 212 * S), COL_ACCENT, 6 * S);
    dl->AddRectFilled(o + ImVec2(450 * S, 200 * S), o + ImVec2(550 * S, 212 * S), COL_PANEL2, 6 * S);
    dl->AddRectFilled(o + ImVec2(450 * S, 200 * S), o + ImVec2(450 * S + 100 * S * std::max(0.f, rt), 212 * S), COL_ACCENT, 6 * S);
    dl->AddText(o + ImVec2(20 * S, 214 * S), IM_COL32(150, 152, 165, 255), "LT");
    dl->AddText(o + ImVec2(450 * S, 214 * S), IM_COL32(150, 152, 165, 255), "RT");
}

/* ---------- Settings ---------- */

static void page_settings() {
    page_header("Settings", "");
    begin_panel("set1");
    ImGui::TextUnformatted("Player name");
    char nb[32];
    std::snprintf(nb, sizeof nb, "%s", A.st.name.c_str());
    ImGui::SetNextItemWidth(320 * S);
    if (ImGui::InputText("##name", nb, 16)) { A.st.name = nb; mark_dirty(); }
    ImGui::Dummy(ImVec2(0, 8 * S));

    ImGui::TextUnformatted("ROM folder");
    text_wrapped_disabled(A.st.rom_folder.empty() ? "(not set)" : A.st.rom_folder);
    if (button("Choose folder...", ImVec2(0, 0))) choose_rom_folder();
    ImGui::SameLine();
    if (button("Rescan", ImVec2(0, 0), false, !A.st.rom_folder.empty() && !A.scanning)) start_scan();
    ImGui::Dummy(ImVec2(0, 8 * S));

    ImGui::TextUnformatted("Smash Classic location");
    std::string se = smash_exe_path();
    text_wrapped_disabled(se.empty() ? "BattleShip.exe not found" : se);
    if (button("Browse for BattleShip.exe...", ImVec2(0, 0))) locate_smash();
    if (!A.st.smash_exe.empty()) {
        ImGui::SameLine();
        if (button("Use default", ImVec2(0, 0))) { A.st.smash_exe.clear(); mark_dirty(); A.hello_dirty = true; }
    }
    ImGui::Dummy(ImVec2(0, 8 * S));

    ImGui::TextUnformatted("Online input delay");
    ImGui::SetNextItemWidth(320 * S);
    if (ImGui::SliderInt("##delay", &A.st.input_delay, 1, 6, "%d frames")) mark_dirty();
    ImGui::TextDisabled("About %d ms at 60 fps. Higher is smoother on slow connections but feels laggier.",
                        A.st.input_delay * 1000 / 60);
    ImGui::Dummy(ImVec2(0, 8 * S));

    if (ImGui::CollapsingHeader("Advanced")) {
        char sb[256];
        std::snprintf(sb, sizeof sb, "%s", A.st.server.c_str());
        ImGui::TextUnformatted("Lobby server URL");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##srv", hubnet::default_server(), sb, sizeof sb)) { A.st.server = sb; mark_dirty(); }
        ImGui::TextDisabled("Leave empty for the default server.");
    }
    end_panel();
}

static void page_about() {
    page_header("About", "");
    begin_panel("about");
    ImGui::PushFont(A.f_h2);
    ImGui::TextUnformatted("Classic Hub");
    ImGui::PopFont();
    ImGui::TextDisabled("Version %s", HUB_VERSION_STR);
    ImGui::Dummy(ImVec2(0, 6 * S));
    ImGui::TextWrapped("One library for N64 games with friends: room codes, one-click launch, online play.");
    ImGui::Dummy(ImVec2(0, 6 * S));
    text_wrapped_disabled("Smash Classic runs as a native port with rollback netplay. Other games run in the built-in "
                          "mupen64plus runner with lockstep input netplay. You supply your own ROMs; none are included.");
    ImGui::Dummy(ImVec2(0, 6 * S));
    text_wrapped_disabled("Updates: new versions are delivered as new releases; replace the folder to update. "
                          "Your settings live in userdata/hub.json.");
    end_panel();
}

/* ---------- frame ---------- */

static bool nav_item(Page pg, const char* label, float w) {
    int key = (int)pg;
    ImGui::PushID(key);
    ImGui::InvisibleButton("nav", ImVec2(w, 46 * S));
    bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    bool clicked = ImGui::IsItemClicked() || ImGui::IsItemActivated();
    bool sel = A.page == pg;
    float& an = A.nav_anim[key];
    float target = sel ? 1.f : (hov ? 0.5f : 0.f);
    an += (target - an) * std::min(1.f, ImGui::GetIO().DeltaTime * 14.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    dl->AddRectFilled(p0, p1, with_alpha(COL_ACCENT, 0.28f * an), 12 * S);
    if (sel) dl->AddRectFilled(p0 + ImVec2(0, 10 * S), p0 + ImVec2(4 * S, 36 * S), COL_ACCENT, 2 * S);
    dl->AddText(p0 + ImVec2(20 * S, 11 * S), mix(IM_COL32(170, 174, 190, 255), IM_COL32_WHITE, an), label);
    ImGui::PopID();
    return clicked;
}

static void draw_frame() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);

    // Sidebar
    float sw = 240 * S;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(21, 22, 28, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * S, 22 * S));
    ImGui::BeginChild("side", ImVec2(sw, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(A.f_title);
    ImGui::TextUnformatted("Classic Hub");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 14 * S));
    float iw = sw - 28 * S;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 6 * S));
    if (nav_item(Page::Library, "Library", iw)) A.page = Page::Library;
    if (nav_item(Page::Online, A.in_room ? "Play Online  (in room)" : "Play Online", iw)) A.page = Page::Online;
    if (nav_item(Page::Controllers, "Controllers", iw)) A.page = Page::Controllers;
    if (nav_item(Page::Settings, "Settings", iw)) A.page = Page::Settings;
    if (nav_item(Page::About, "About / Updates", iw)) A.page = Page::About;
    ImGui::PopStyleVar();
    // player card at the bottom
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 78 * S);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, p + ImVec2(iw, 56 * S), COL_PANEL, 12 * S);
        dl->AddCircleFilled(p + ImVec2(28 * S, 28 * S), 14 * S, COL_ACCENT, 24);
        dl->AddText(p + ImVec2(52 * S, 8 * S), IM_COL32_WHITE, A.st.name.c_str());
        const char* stt = A.proc ? "In game" : (A.in_room ? (A.lobby.connected() ? "In a room" : "Connecting") : "Offline");
        dl->AddText(p + ImVec2(52 * S, 29 * S), IM_COL32(150, 152, 165, 255), stt);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 0);

    // Content
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32 * S, 28 * S));
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    ImGui::PopStyleVar();

    if (A.proc) {
        begin_panel("ingame", ImVec2(0, 0), mix(COL_PANEL, COL_ACCENT, 0.25f));
        ImGui::Text("In game...  %s is running. The hub will be ready again when it closes.", A.proc_title.c_str());
        end_panel();
    }

    float status_h = 36 * S;
    ImGui::BeginChild("page", ImVec2(0, -status_h), ImGuiChildFlags_NavFlattened);
    switch (A.page) {
        case Page::Library: page_library(); break;
        case Page::Online: page_online(); break;
        case Page::Controllers: page_controllers(); break;
        case Page::Settings: page_settings(); break;
        case Page::About: page_about(); break;
    }
    ImGui::EndChild();

    // Status bar
    std::string left = A.in_room ? A.lobby.status() : "Not in a room";
    bool show_notice = !A.notice.empty() && ImGui::GetTime() < A.notice_until;
    ImGui::Separator();
    ImGui::TextDisabled("%s", left.c_str());
    if (show_notice) {
        ImGui::SameLine();
        ImGui::TextDisabled("   |   ");
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_WARN), "%s", A.notice.c_str());
    }
    ImGui::EndChild();
    ImGui::End();
}

/* ============================ main ============================ */

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    SDL_SetMainReady();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        MessageBoxA(nullptr, SDL_GetError(), "Classic Hub", MB_ICONERROR);
        return 1;
    }

    float ddpi = 96.f;
    SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr);
    S = std::max(1.0f, ddpi / 96.0f);

    // settings
    A.settings_path = hub::data_dir() + "hub.json";
    A.st.server = hubnet::default_server();
    hubui::load_settings(A.settings_path, &A.st);
    if (A.st.name.empty()) A.st.name = "Player";
    if (A.st.rom_folder.empty()) {
        // Default ROM folder: %USERPROFILE%\Desktop\Everything\N64Games if it exists.
        wchar_t up[MAX_PATH];
        if (GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH)) {
            std::wstring d = std::wstring(up) + L"\\Desktop\\Everything\\N64Games";
            DWORD a = GetFileAttributesW(d.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) A.st.rom_folder = U(d);
        }
    }

    A.win = SDL_CreateWindow("Classic Hub", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)(1100 * S), (int)(720 * S),
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    SDL_Renderer* ren = A.win ? SDL_CreateRenderer(A.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!A.win || !ren) {
        MessageBoxA(nullptr, SDL_GetError(), "Classic Hub", MB_ICONERROR);
        return 1;
    }
    SDL_SetWindowMinimumSize(A.win, (int)(820 * S), (int)(560 * S));
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(A.win, &wm)) A.hwnd = wm.info.win.window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    setup_fonts(S);
    setup_theme(S);
    ImGui_ImplSDL2_InitForSDLRenderer(A.win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);

    refresh_pads();
    start_scan();

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
            if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) refresh_pads();
        }
        if (A.scan_ready.exchange(false)) {
            if (A.scan_th.joinable()) A.scan_th.join();
            A.scanning = false;
            apply_scan();
        }
        poll_process();
        lobby_tick();
        if (A.dirty && !ImGui::IsAnyItemActive()) {
            hubui::save_settings(A.settings_path, A.st);
            A.dirty = false;
        }

        if (SDL_GetWindowFlags(A.win) & SDL_WINDOW_MINIMIZED) { SDL_Delay(50); continue; }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGui::PushFont(A.f_body);
        draw_frame();
        ImGui::PopFont();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 19, 20, 24, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);
    }

    // clean shutdown
    if (A.dirty) hubui::save_settings(A.settings_path, A.st);
    A.lobby.stop();
    if (A.scan_th.joinable()) A.scan_th.join();
    if (A.proc) CloseHandle(A.proc);
    for (auto* p : A.pads) SDL_GameControllerClose(p);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(A.win);
    SDL_Quit();
    CoUninitialize();
    return 0;
}
