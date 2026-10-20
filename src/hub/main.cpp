/* ClassicHub.exe - the launcher front door.
 *
 * Layout: full-window custom UI (no floating windows): left sidebar nav + content pages
 * (Library, Play Online, Saves, Controllers, Settings, About). Drawn with Dear ImGui on SDL_Renderer.
 *
 * Launch contracts:
 *  - N64 (emulated):  n64run.exe --rom <path> --game <id|unknown> --save-dir <battery dir>
 *                     [--net <server> --room <CODE>-<launch> --slot <lobby slot> --players <n>
 *                      --ports <p0,p1,p2,p3> --delay <d>]
 *  - GBA/GB/GBC:      retrorun.exe --core <x_libretro.dll> --rom <path> --system <gba|gb|gbc>
 *                     --save-dir <battery dir> --state-dir <states dir> --resume [--fullscreen]
 *                     [--net ... same flags as n64run; online uses a fresh temp save dir, no --resume]
 *  - GameCube:        Dolphin.exe -b -u <data>\dolphin-user -e <game>  (INI keys written before launch)
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
#include <winhttp.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "games.h"
#include "library.h"
#include "logic.h"
#include "savestore.h"
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

/* IFileSaveDialog. Returns "" on cancel. */
static std::string pick_save_path(HWND owner, const wchar_t* default_name, const wchar_t* filter_name,
                                  const wchar_t* filter_spec, const wchar_t* default_ext) {
    std::string result;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    COMDLG_FILTERSPEC f[1] = {{filter_name, filter_spec}};
    dlg->SetFileTypes(1, f);
    dlg->SetDefaultExtension(default_ext);
    dlg->SetFileName(default_name);
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

static bool dir_exists(const std::string& p) {
    if (p.empty()) return false;
    DWORD a = GetFileAttributesW(W(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::string read_text_file(const std::string& p) {
    std::ifstream f(W(p), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static bool write_text_file(const std::string& p, const std::string& text) {
    std::ofstream f(W(p), std::ios::binary | std::ios::trunc);
    f << text;
    return f.good();
}
static void make_dirs(const std::string& p) {
    std::wstring w = W(p);
    for (size_t i = 3; i < w.size(); ++i)
        if (w[i] == L'\\' || w[i] == L'/') CreateDirectoryW(w.substr(0, i).c_str(), nullptr);
    CreateDirectoryW(w.c_str(), nullptr);
}
static std::string with_slash(std::string p) {
    if (!p.empty() && p.back() != '\\' && p.back() != '/') p += '\\';
    return p;
}
/* Look for <rel> under exe_dir, then under up to 6 parent directories (dev builds: Hub\deps\...). */
static std::string find_upward(const std::string& rel) {
    std::string base = hub::exe_dir();
    for (int i = 0; i < 7; ++i) {
        std::string c = base + rel;
        if (GetFileAttributesW(W(c).c_str()) != INVALID_FILE_ATTRIBUTES) return c;
        std::string t = base;
        while (!t.empty() && (t.back() == '\\' || t.back() == '/')) t.pop_back();
        size_t k = t.find_last_of("/\\");
        if (k == std::string::npos) break;
        base = t.substr(0, k + 1);
    }
    return "";
}
static std::string env_var(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return (n > 0 && n < MAX_PATH * 2) ? U(buf) : "";
}
/* Number of files below dir (recursive) and their total size. */
static void count_files(const std::string& dir, int* files, uint64_t* bytes) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(with_slash(dir) + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = U(fd.cFileName);
        if (n == "." || n == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) count_files(with_slash(dir) + n, files, bytes);
        else { ++*files; *bytes += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void remove_tree(const std::string& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(with_slash(dir) + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = U(fd.cFileName);
        if (n == "." || n == "..") continue;
        std::string p = with_slash(dir) + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { remove_tree(p); RemoveDirectoryW(W(p).c_str()); }
        else DeleteFileW(W(p).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static std::string fmt_time(uint64_t unix_s) {
    if (!unix_s) return "never";
    time_t t = (time_t)unix_s;
    tm lt{};
    localtime_s(&lt, &t);
    time_t now = time(nullptr);
    tm nt{};
    localtime_s(&nt, &now);
    char b[48];
    if (lt.tm_year == nt.tm_year && lt.tm_yday == nt.tm_yday) std::strftime(b, sizeof b, "Today %H:%M", &lt);
    else std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &lt);
    return b;
}
static uint64_t unix_now() { return (uint64_t)time(nullptr); }

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
/* Per-system default scheme. */
static void system_colors(hub::System s, ImU32* a, ImU32* b) {
    switch (s) {
        case hub::System::N64: *a = IM_COL32(200, 50, 45, 255); *b = IM_COL32(245, 140, 50, 255); break;
        case hub::System::GC: *a = IM_COL32(70, 50, 170, 255); *b = IM_COL32(160, 95, 220, 255); break;
        case hub::System::GBA: *a = IM_COL32(40, 70, 190, 255); *b = IM_COL32(130, 100, 235, 255); break;
        case hub::System::GBC: *a = IM_COL32(0, 120, 125, 255); *b = IM_COL32(60, 200, 180, 255); break;
        case hub::System::GB: *a = IM_COL32(85, 100, 40, 255); *b = IM_COL32(150, 175, 70, 255); break;
        default: *a = IM_COL32(70, 78, 100, 255); *b = IM_COL32(120, 130, 160, 255); break;
    }
}
static const char* sys_badge(hub::System s) {
    switch (s) {
        case hub::System::N64: return "N64";
        case hub::System::GC: return "GAMECUBE";
        case hub::System::GBA: return "GBA";
        case hub::System::GBC: return "GBC";
        case hub::System::GB: return "GAME BOY";
        default: return "?";
    }
}
static void entry_colors(const hub::GameEntry& e, ImU32* a, ImU32* b) {
    if (e.system == hub::System::N64 && !e.catalog_id.empty() && hub::find_game(e.catalog_id)) game_colors(e.catalog_id, a, b);
    else system_colors(e.system, a, b);
}

/* ============================ app state ============================ */

enum class Page { Library, Online, Saves, Controllers, Settings, About };

struct ChatLine { std::string who, text; };

/* The game currently running as a child process. */
struct RunInfo {
    std::string key, system_short, title;
    uint64_t start_unix = 0;
    ULONGLONG start_tick = 0;
    bool track = true;      // record play time / touch the save vault (false for the native Smash port)
    bool online = false;
};

/* Background save job (sync / export / import). */
struct SaveJob {
    enum Kind { None, Sync, Export, Import } kind = None;
    std::thread th;
    std::atomic<bool> running{false}, done{false};
    hub::saves::SyncReport report;
    bool export_ok = false;
    std::string export_err, path;
    bool include_states = false, manual = false;
};

struct VaultRow { hubui::VaultMeta m; };

struct App {
    SDL_Window* win = nullptr;
    HWND hwnd = nullptr;
    hubui::Settings st;
    std::string settings_path;
    bool dirty = false;

    Page page = Page::Library;
    ImFont *f_body = nullptr, *f_title = nullptr, *f_h2 = nullptr, *f_big = nullptr;

    // library scan
    std::thread scan_th;
    std::atomic<bool> scanning{false}, scan_ready{false};
    std::atomic<int> scan_done{0}, scan_total{0};
    std::vector<hub::GameEntry> scan_result;
    std::vector<hub::GameEntry> lib;              // scanned games (+ native Smash entry first)
    std::map<std::string, int> lib_index;         // key -> index in lib
    std::map<std::string, hub::GameState> lib_state;   // cached game_state per key
    std::string sys_filter;                       // "" = all, else system short
    char search_buf[64] = {0};
    std::string vis_filter, vis_query;            // last inputs used to build `vis`
    std::vector<int> vis;                         // filtered + sorted indices into lib
    std::vector<int> vis_recent, vis_fav;
    bool vis_dirty = true;
    bool scanned_once = false;

    // save info for the selected game
    std::string info_key;
    int info_battery_files = 0, info_state_files = 0;
    uint64_t info_battery_bytes = 0;
    bool info_auto_state = false;
    double info_at = -100;

    // saves
    SaveJob job;
    std::function<void()> after_job;              // run once the current job finishes (e.g. a queued launch)
    bool sync_pending = false;                    // a sync was requested while a job was running
    std::string sync_result;                      // last result summary (Saves page)
    bool sync_result_ok = true;
    std::string export_result, import_result;
    bool export_ok = true, import_ok = true;
    std::vector<VaultRow> vault_rows;
    bool vault_dirty = true;

    // toast
    std::string toast;
    ImU32 toast_col = 0;
    double toast_until = 0;

    // running game
    RunInfo run;

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
    std::string selected_game;   // room's chosen game (room id: catalog id or library key)
    std::string pending_room_game;   // preselect when hosting from the library
    int launch_counter = 0;
    std::vector<ChatLine> chat;
    char chat_buf[256] = {0};
    bool chat_scroll = false;

    // child process
    HANDLE proc = nullptr;
    std::string proc_title;

    // ui
    std::string sel_game;        // library selection (game key)
    bool focus_search = false;
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
    /* The released package ships the hub inside the Smash Classic folder. */
    std::string here = hub::exe_dir() + "BattleShip.exe";
    if (file_exists(here)) return here;
    return "";
}
static std::string n64run_path() { return hub::exe_dir() + "n64run.exe"; }

/* ---------- library / ROM scanning ---------- */

static const char* kNativeKey = "native:smash64";

static bool is_native(const hub::GameEntry& e) { return e.key == kNativeKey; }
static std::string sys_short(const hub::GameEntry& e) { return hub::system_short(e.system); }
static std::string no_slash(std::string p) {
    while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    return p;
}

static void start_scan() {
    if (A.scanning) return;
    if (A.scan_th.joinable()) A.scan_th.join();
    A.scanning = true;
    A.scan_done = 0;
    A.scan_total = 0;
    std::vector<std::string> folders = A.st.rom_folders;
    A.scan_th = std::thread([folders]() {
        A.scan_result = hub::scan_library(folders, [](int d, int t) { A.scan_done = d; A.scan_total = t; });
        A.scan_ready = true;
    });
}

static const hub::GameState& gstate(const std::string& key) {
    auto it = A.lib_state.find(key);
    if (it == A.lib_state.end()) it = A.lib_state.emplace(key, hub::game_state(key)).first;
    return it->second;
}
static void refresh_state(const std::string& key) {
    A.lib_state[key] = hub::game_state(key);
    A.vis_dirty = true;
}
static const hub::GameEntry* find_entry(const std::string& key) {
    auto it = A.lib_index.find(key);
    return it == A.lib_index.end() ? nullptr : &A.lib[it->second];
}

static void apply_scan() {
    A.lib.clear();
    if (!smash_exe_path().empty()) {
        if (const GameInfo* g = hub::find_game("smash64")) {
            hub::GameEntry n;
            n.key = kNativeKey;
            n.system = hub::System::N64;
            n.title = g->title;
            n.serial = "";
            n.path = smash_exe_path();
            n.catalog_id = "smash64";
            A.lib.push_back(n);
        }
    }
    for (auto& e : A.scan_result) A.lib.push_back(e);
    A.lib_index.clear();
    A.lib_state.clear();
    for (size_t i = 0; i < A.lib.size(); ++i) A.lib_index.emplace(A.lib[i].key, (int)i);   // first wins on duplicates
    A.vis_dirty = true;
    A.hello_dirty = true;
    A.info_at = -100;
    A.scanned_once = true;
}

static void rebuild_vis() {
    std::vector<hubui::ListItem> items;
    items.reserve(A.lib.size());
    for (auto& e : A.lib) items.push_back({e.key, e.title, sys_short(e), is_native(e)});
    std::string q = A.search_buf;
    A.vis = hubui::filter_sort(items, A.sys_filter, q);
    std::vector<unsigned long long> lp(A.lib.size(), 0);
    for (size_t i = 0; i < A.lib.size(); ++i)
        if (A.sys_filter.empty() || A.sys_filter == sys_short(A.lib[i])) lp[i] = gstate(A.lib[i].key).last_played;
    A.vis_recent = hubui::pick_recent(lp, 6);
    A.vis_fav.clear();
    for (int i : A.vis)
        if (gstate(A.lib[i].key).favorite) A.vis_fav.push_back(i);
    A.vis_filter = A.sys_filter;
    A.vis_query = q;
    A.vis_dirty = false;
}

/* ---------- online capability ---------- */

static bool supports_online(const hub::GameEntry& e) {
    return e.system == hub::System::N64 || e.system == hub::System::GBA || e.system == hub::System::GB ||
           e.system == hub::System::GBC;
}
static int max_players_for(const hub::GameEntry& e) {
    if (const GameInfo* g = hub::find_game(e.catalog_id)) return g->max_players;
    return (e.system == hub::System::GB || e.system == hub::System::GBC) ? 2 : 4;
}
/* How a game is named in lobby messages: the N64 catalog id (back-compat) or the library key. */
static std::string room_id(const hub::GameEntry& e) { return e.catalog_id.empty() ? e.key : e.catalog_id; }
static const hub::GameEntry* find_room_entry(const std::string& id) {
    for (auto& e : A.lib)
        if (supports_online(e) && room_id(e) == id) return &e;
    return nullptr;
}
static std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
static void my_games(std::map<std::string, std::string>* games, std::vector<std::string>* keys) {
    for (auto& e : A.lib) {
        if (!supports_online(e)) continue;
        if (is_native(e)) { (*games)["smash64"] = "native"; continue; }
        if (!e.catalog_id.empty() && e.key.compare(0, 4, "n64:") == 0 && !games->count(e.catalog_id))
            (*games)[e.catalog_id] = upper(e.key.substr(4));
        if (keys) keys->push_back(e.key);
    }
}
static std::map<std::string, std::string> my_games_map() {
    std::map<std::string, std::string> m;
    my_games(&m, nullptr);
    return m;
}

/* ---------- save jobs (sync / export / import) ---------- */

static void set_toast(const std::string& s, bool good = true, double secs = 6.0) {
    A.toast = s;
    A.toast_col = good ? COL_GOOD : COL_BAD;
    A.toast_until = ImGui::GetTime() + secs;
}

static bool job_busy() { return A.job.running; }

static void start_job(SaveJob::Kind k, const std::string& path, bool manual) {
    if (A.job.running) return;
    if (A.job.th.joinable()) A.job.th.join();
    A.job.kind = k;
    A.job.running = true;
    A.job.done = false;
    A.job.path = path;
    A.job.include_states = A.st.sync_states;
    A.job.manual = manual;
    A.job.report = hub::saves::SyncReport();
    A.job.export_ok = false;
    A.job.export_err.clear();
    A.job.th = std::thread([]() {
        SaveJob& j = A.job;
        if (j.kind == SaveJob::Sync) j.report = hub::saves::sync_folder(j.path, j.include_states);
        else if (j.kind == SaveJob::Export) j.export_ok = hub::saves::export_zip(j.path, j.include_states, &j.export_err);
        else if (j.kind == SaveJob::Import) j.report = hub::saves::import_zip(j.path);
        j.done = true;
    });
}

static void start_sync(bool manual) {
    if (A.st.sync_folder.empty()) return;
    if (A.job.running) { A.sync_pending = true; return; }
    start_job(SaveJob::Sync, A.st.sync_folder, manual);
}
static void auto_sync() {
    if (!A.st.sync_folder.empty() && A.st.sync_auto) start_sync(false);
}

static std::string report_summary(const hub::saves::SyncReport& r, const char* sent_word, const char* got_word) {
    if (!r.ok) return "Failed: " + (r.error.empty() ? std::string("unknown error") : r.error);
    if (r.pushed == 0 && r.pulled == 0 && r.conflicts == 0) return "Everything is already up to date.";
    std::string s;
    if (r.pushed) s += std::to_string(r.pushed) + " file(s) " + sent_word;
    if (r.pulled) s += (s.empty() ? "" : ", ") + std::to_string(r.pulled) + " file(s) " + got_word;
    if (r.conflicts) s += (s.empty() ? "" : ", ") + std::to_string(r.conflicts) + " conflict(s) (older copy kept in _conflicts)";
    return s + ".";
}

static void poll_job() {
    if (!A.job.running || !A.job.done) return;
    if (A.job.th.joinable()) A.job.th.join();
    SaveJob& j = A.job;
    if (j.kind == SaveJob::Sync) {
        A.sync_result = report_summary(j.report, "sent", "received");
        A.sync_result_ok = j.report.ok;
        bool quiet = j.report.ok && j.report.pushed == 0 && j.report.pulled == 0 && j.report.conflicts == 0 && !j.manual;
        if (!quiet) set_toast(j.report.ok ? "Saves synced: " + A.sync_result : "Save sync " + A.sync_result, j.report.ok);
    } else if (j.kind == SaveJob::Export) {
        A.export_ok = j.export_ok;
        A.export_result = j.export_ok ? "Exported to " + j.path : "Export failed: " + j.export_err;
        set_toast(j.export_ok ? "Saves exported." : A.export_result, j.export_ok);
    } else if (j.kind == SaveJob::Import) {
        A.import_ok = j.report.ok;
        A.import_result = report_summary(j.report, "restored", "restored");
        if (j.report.ok && (j.report.pushed + j.report.pulled) > 0)
            A.import_result = std::to_string(j.report.pushed + j.report.pulled) + " file(s) restored" +
                              (j.report.conflicts ? ", " + std::to_string(j.report.conflicts) + " conflict(s) (older copy kept)" : "") + ".";
        set_toast(j.report.ok ? "Saves imported." : A.import_result, j.report.ok);
    }
    A.vault_dirty = true;
    A.info_at = -100;
    j.kind = SaveJob::None;
    j.running = false;
    j.done = false;
    std::function<void()> next;
    next.swap(A.after_job);
    if (A.sync_pending) { A.sync_pending = false; start_sync(false); }
    if (next) next();
}

/* ---------- process launching ---------- */

static bool spawn(const std::string& exe, const std::string& args, const std::string& cwd,
                  const std::vector<std::pair<std::string, std::string>>& env, const RunInfo& ri) {
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
    A.proc_title = ri.title;
    A.run = ri;
    A.run.start_unix = unix_now();
    A.run.start_tick = GetTickCount64();
    return true;
}

static void poll_process() {
    if (!A.proc) return;
    if (WaitForSingleObject(A.proc, 0) != WAIT_OBJECT_0) return;
    DWORD code = 0;
    GetExitCodeProcess(A.proc, &code);
    CloseHandle(A.proc);
    A.proc = nullptr;
    uint64_t secs = (GetTickCount64() - A.run.start_tick) / 1000;
    if (!A.run.key.empty()) {
        if (secs >= 1) hub::record_play(A.run.key, A.run.start_unix, secs);
        if (A.run.track && !A.run.online && !A.run.system_short.empty())
            hub::saves::touch_game(A.run.system_short, A.run.key, A.run.title);
        hub::flush_library_state();
        refresh_state(A.run.key);
    }
    A.vault_dirty = true;
    A.info_at = -100;
    set_notice(A.proc_title + ": " + (code == 0 ? std::string("closed.") : hubui::exit_code_text(code)), 20.0);
    auto_sync();
}

/* Core DLL: <exe>\cores\ (packaged) or <Hub>\deps\cores\ (dev). */
static std::string find_core(const std::string& name) {
    std::string f = name + "_libretro.dll";
    std::string p = hub::exe_dir() + "cores\\" + f;
    if (file_exists(p)) return p;
    return find_upward("deps\\cores\\" + f);
}
static std::string find_dolphin() {
    std::string p = hub::exe_dir() + "dolphin\\Dolphin.exe";
    if (file_exists(p)) return p;
    return find_upward("deps\\dolphin\\Dolphin.exe");
}

static std::string quote(const std::string& s) { return "\"" + s + "\""; }

/* Pre-write the Dolphin INI keys the hub relies on (merged into whatever is already there).
 * Per-launch settings (fullscreen, memory-card folder) go on the command line with -C. */
static void prepare_dolphin(const std::string& userdir) {
    make_dirs(userdir + "\\Config");
    std::string path = userdir + "\\Config\\Dolphin.ini";
    std::string t = read_text_file(path);
    t = hubui::ini_set(t, "Analytics", "Enabled", "False");
    t = hubui::ini_set(t, "Analytics", "PermissionAsked", "True");
    t = hubui::ini_set(t, "AutoUpdate", "UpdateTrack", "");
    t = hubui::ini_set(t, "Interface", "ConfirmStop", "False");
    t = hubui::ini_set(t, "Interface", "UsePanicHandlers", "False");
    write_text_file(path, t);

    std::string pad_path = userdir + "\\Config\\GCPadNew.ini";
    std::string p = read_text_file(pad_path);
    if (hubui::ini_get(p, "GCPad1", "Device").empty()) {
        static const char* kv[][2] = {
            {"Device", "XInput/0/Gamepad"},
            {"Buttons/A", "`Button A`"}, {"Buttons/B", "`Button X`"}, {"Buttons/X", "`Button B`"},
            {"Buttons/Y", "`Button Y`"}, {"Buttons/Z", "`Shoulder R`"}, {"Buttons/Start", "Start"},
            {"Main Stick/Up", "`Left Y+`"}, {"Main Stick/Down", "`Left Y-`"},
            {"Main Stick/Left", "`Left X-`"}, {"Main Stick/Right", "`Left X+`"},
            {"C-Stick/Up", "`Right Y+`"}, {"C-Stick/Down", "`Right Y-`"},
            {"C-Stick/Left", "`Right X-`"}, {"C-Stick/Right", "`Right X+`"},
            {"Triggers/L", "`Trigger L`"}, {"Triggers/R", "`Trigger R`"},
            {"D-Pad/Up", "`Pad N`"}, {"D-Pad/Down", "`Pad S`"}, {"D-Pad/Left", "`Pad W`"}, {"D-Pad/Right", "`Pad E`"},
        };
        for (auto& e2 : kv) p = hubui::ini_set(p, "GCPad1", e2[0], e2[1]);
        write_text_file(pad_path, p);
    }
}

/* ---------- Dolphin download (not shipped: ~18 MB archive, GPL) ---------- */

struct DolphinDl {
    enum State { Idle, Asking, Working, Failed } state = Idle;
    std::thread th;
    std::atomic<bool> done{false}, ok{false};
    std::atomic<uint64_t> got{0}, total{0};
    std::atomic<int> phase{0};   // 0 download, 1 extract, 2 install
    std::string error, key;      // key: the game to start afterwards
    std::mutex mu;
} DL;

static bool http_download(const std::wstring& host, const std::wstring& path, const std::string& out_file, std::string* err) {
    HINTERNET ses = WinHttpOpen(L"ClassicHub", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!ses) { *err = "network unavailable"; return false; }
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
        wchar_t lenb[32] = {0};
        sz = sizeof lenb;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, lenb, &sz, nullptr)) DL.total = _wtoi64(lenb);
        if (status != 200) *err = "server answered " + std::to_string(status);
        else {
            HANDLE f = CreateFileW(W(out_file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) *err = "cannot write " + out_file;
            else {
                std::vector<char> buf(64 * 1024);
                ok = true;
                for (;;) {
                    DWORD n = 0;
                    if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &n)) { ok = false; *err = "download interrupted"; break; }
                    if (n == 0) break;
                    DWORD w = 0;
                    if (!WriteFile(f, buf.data(), n, &w, nullptr) || w != n) { ok = false; *err = "disk write failed"; break; }
                    DL.got += n;
                }
                CloseHandle(f);
            }
        }
    } else *err = "could not reach " + U(host);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

/* Directory (searched to `depth` levels) that contains a file called `name`. */
static bool find_file_below(const std::string& dir, const wchar_t* name, int depth, std::string* found_dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(with_slash(dir) + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    std::vector<std::string> subs;
    do {
        std::string n = U(fd.cFileName);
        if (n == "." || n == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) subs.push_back(n);
        else if (_wcsicmp(fd.cFileName, name) == 0) { *found_dir = dir; ok = true; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (ok || depth <= 0) return ok;
    for (auto& s2 : subs)
        if (find_file_below(with_slash(dir) + s2, name, depth - 1, found_dir)) return true;
    return false;
}

/* Mirrors scripts/fetch-dolphin.ps1: download, tar -xf into a staging folder, move the folder that
 * holds Dolphin.exe up into <exe>\dolphin\, create portable.txt. */
static void dolphin_install_thread() {
    std::string dir = no_slash(hub::exe_dir()) + "\\dolphin";
    std::string stage = dir + "\\_extract";
    std::string archive = dir + "\\dolphin-2509-x64.7z";
    std::string err;
    make_dirs(dir);
    remove_tree(stage);
    make_dirs(stage);
    DL.phase = 0;
    bool ok = http_download(L"dl.dolphin-emu.org", L"/releases/2509/dolphin-2509-x64.7z", archive + ".part", &err);
    if (ok) MoveFileExW(W(archive + ".part").c_str(), W(archive).c_str(), MOVEFILE_REPLACE_EXISTING);
    if (ok) {
        DL.phase = 1;
        wchar_t sysdir[MAX_PATH];
        GetSystemDirectoryW(sysdir, MAX_PATH);
        std::wstring tar = std::wstring(sysdir) + L"\\tar.exe";
        std::wstring cmd = L"\"" + tar + L"\" -xf \"" + W(archive) + L"\" -C \"" + W(stage) + L"\"";
        std::vector<wchar_t> cb(cmd.begin(), cmd.end());
        cb.push_back(0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(tar.c_str(), cb.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            if (code != 0) { ok = false; err = "extraction failed (tar exit " + std::to_string(code) + ")"; }
        } else { ok = false; err = "tar.exe not found (needs Windows 10 or later)"; }
    }
    if (ok) {
        DL.phase = 2;
        std::string inner;
        if (!find_file_below(stage, L"Dolphin.exe", 3, &inner)) { ok = false; err = "Dolphin.exe not found in the download"; }
        else {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(W(with_slash(inner) + "*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    std::string n = U(fd.cFileName);
                    if (n == "." || n == "..") continue;
                    if (!MoveFileExW(W(with_slash(inner) + n).c_str(), W(dir + "\\" + n).c_str(), MOVEFILE_REPLACE_EXISTING) &&
                        !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { ok = false; err = "could not install " + n; }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            write_text_file(dir + "\\portable.txt", "");
        }
    }
    remove_tree(stage);
    RemoveDirectoryW(W(stage).c_str());
    DeleteFileW(W(archive).c_str());
    DeleteFileW(W(archive + ".part").c_str());
    if (!ok) { std::lock_guard<std::mutex> lk(DL.mu); DL.error = err; }
    DL.ok = ok;
    DL.done = true;
}

static void start_dolphin_install() {
    if (DL.th.joinable()) DL.th.join();
    DL.done = false;
    DL.ok = false;
    DL.got = 0;
    DL.total = 0;
    DL.phase = 0;
    DL.error.clear();
    DL.state = DolphinDl::Working;
    DL.th = std::thread(dolphin_install_thread);
}

static RunInfo run_info_for(const hub::GameEntry& e, bool online) {
    RunInfo ri;
    ri.key = e.key;
    ri.system_short = is_native(e) ? "" : sys_short(e);
    ri.title = e.title;
    ri.track = !is_native(e);
    ri.online = online;
    return ri;
}

static void play_entry(const hub::GameEntry& e);
static void play_entry(const hub::GameEntry& e) {
    if (A.proc) { set_notice("A game is already running."); return; }
    if (A.job.running) {
        std::string key = e.key;
        A.after_job = [key]() { if (const hub::GameEntry* g = find_entry(key)) { A.sel_game = key; hub::GameEntry copy = *g; play_entry(copy); } };
        set_notice("Syncing your saves first, then starting " + e.title + "...", 10.0);
        return;
    }
    A.st.last_game = room_id(e);
    mark_dirty();
    RunInfo ri = run_info_for(e, false);
    std::string sys = sys_short(e);
    if (is_native(e)) {
        std::string exe = smash_exe_path();
        spawn(exe, "--from-hub", dir_of(exe), {}, ri);
        return;
    }
    std::string battery = no_slash(hub::saves::battery_dir(sys, e.key));
    std::string states = no_slash(hub::saves::states_dir(sys, e.key));
    switch (e.system) {
        case hub::System::N64: {
            std::string args = "--rom " + quote(e.path) + " --game " + (e.catalog_id.empty() ? "unknown" : e.catalog_id) +
                               " --save-dir " + quote(battery);
            if (!file_exists(hub::exe_dir() + "emu\\mupen64plus.dll")) {
                std::string emu = find_upward("deps\\emu\\mupen64plus.dll");
                if (!emu.empty()) args += " --emu-dir " + quote(no_slash(dir_of(emu)));
            }
            spawn(n64run_path(), args, hub::exe_dir(), {}, ri);
            break;
        }
        case hub::System::GBA:
        case hub::System::GB:
        case hub::System::GBC: {
            std::string core = find_core(e.system == hub::System::GBA ? "mgba" : A.st.gb_core);
            if (core.empty()) { set_notice("Emulator core for " + std::string(hub::system_name(e.system)) + " not found (cores folder)."); return; }
            std::string args = "--core " + quote(core) + " --rom " + quote(e.path) + " --system " + sys + " --save-dir " + quote(battery) +
                               " --state-dir " + quote(states) + " --resume";
            if (A.st.fullscreen) args += " --fullscreen";
            spawn(hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
            break;
        }
        case hub::System::GC: {
            std::string dol = find_dolphin();
            if (dol.empty()) {   // not shipped: ask, download, then start this game
                DL.key = e.key;
                DL.state = DolphinDl::Asking;
                return;
            }
            std::string userdir = no_slash(hub::data_dir()) + "\\dolphin-user";
            prepare_dolphin(userdir);
            std::string gci = battery + "\\GCI";
            make_dirs(gci);
            std::string args = "-b -u " + quote(userdir) + " -C Dolphin.Core.SlotA=8 -C \"Dolphin.Core.GCIFolderAPathOverride=" + gci +
                               "\" -C Dolphin.Display.Fullscreen=" + (A.st.fullscreen ? "True" : "False") + " -e " + quote(e.path);
            spawn(dol, args, dir_of(dol), {}, ri);
            break;
        }
        default: set_notice("Unsupported system."); break;
    }
}

static void launch_online(const hub::GameEntry& e, int launch_no, int delay, bool is_host, const std::string& core_in) {
    A.st.last_game = room_id(e);
    mark_dirty();
    RunInfo ri = run_info_for(e, true);
    if (is_native(e)) {
        std::string exe = smash_exe_path();
        std::vector<std::pair<std::string, std::string>> env;
        env.push_back({is_host ? "SSB64_LOBBY_HOST" : "SSB64_LOBBY_JOIN", server_url()});
        env.push_back({"SSB64_LOBBY_CODE", A.code});
        spawn(exe, "--from-hub", dir_of(exe), env, ri);
        return;
    }
    std::vector<int> slots;
    for (auto& m : A.lobby.members()) slots.push_back(m.slot);
    std::string net = " --net " + quote(server_url()) + " --room " + A.code + "-" + std::to_string(launch_no) + " --slot " +
                      std::to_string(A.lobby.my_slot()) + " --players " + std::to_string(slots.size()) + " --ports " +
                      hubui::ports_arg(slots) + " --delay " + std::to_string(delay);
    if (e.system == hub::System::N64) {
        std::string args = "--rom " + quote(e.path) + " --game " + (e.catalog_id.empty() ? "unknown" : e.catalog_id) + net;
        if (!file_exists(hub::exe_dir() + "emu\\mupen64plus.dll")) {
            std::string emu = find_upward("deps\\emu\\mupen64plus.dll");
            if (!emu.empty()) args += " --emu-dir " + quote(no_slash(dir_of(emu)));
        }
        spawn(n64run_path(), args, hub::exe_dir(), {}, ri);
        return;
    }
    if (e.system == hub::System::GBA || e.system == hub::System::GB || e.system == hub::System::GBC) {
        std::string corename = e.system == hub::System::GBA ? "mgba" : (core_in.empty() ? A.st.gb_core : core_in);
        std::string core = find_core(corename);
        if (core.empty()) { set_notice("Emulator core '" + corename + "' not found (cores folder)."); return; }
        /* Netplay starts from an empty, identical save on every PC: use a throwaway dir. */
        std::string tmp = no_slash(hub::data_dir()) + "\\netplay\\" + sys_short(e);
        remove_tree(tmp);
        make_dirs(tmp);
        std::string args = "--core " + quote(core) + " --rom " + quote(e.path) + " --system " + sys_short(e) + " --save-dir " + quote(tmp) +
                           " --state-dir " + quote(tmp) + net;
        if (A.st.fullscreen) args += " --fullscreen";
        spawn(hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
        return;
    }
    set_notice("Online play is not available for this system yet.");
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
    if (host && !A.pending_room_game.empty() && find_room_entry(A.pending_room_game)) A.selected_game = A.pending_room_game;
    else if (host && !A.st.last_game.empty() && find_room_entry(A.st.last_game)) A.selected_game = A.st.last_game;
    A.pending_room_game.clear();
}

static bool is_host() { return A.lobby.connected() && A.lobby.my_slot() == 0; }

static std::vector<hubui::PeerInfo> peer_list() {
    std::vector<hubui::PeerInfo> v;
    int me = A.lobby.my_slot();
    for (auto& m : A.lobby.members()) {
        hubui::PeerInfo p;
        auto it = A.peers.find(m.slot);
        if (it != A.peers.end()) p = it->second;
        if (m.slot == me) { p.hello = true; p.games.clear(); p.keys.clear(); my_games(&p.games, &p.keys); }
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
        {
            std::map<std::string, std::string> games;
            std::vector<std::string> keys;
            my_games(&games, &keys);
            for (auto& m : hubui::hello_messages("2", games, keys)) A.lobby.send_msg(m);
        }
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
            const hub::GameEntry* g = find_room_entry(j.str("game"));
            if (!g) { set_notice("The host started a game you don't have."); continue; }
            if (A.proc) continue;
            launch_online(*g, (int)j.num("launch", 1), (int)j.num("delay", 2), false, j.str("core"));
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

static void add_rom_folder() {
    std::string p = pick_path(A.hwnd, true, nullptr, nullptr);
    if (p.empty()) return;
    if (std::find(A.st.rom_folders.begin(), A.st.rom_folders.end(), p) == A.st.rom_folders.end()) A.st.rom_folders.push_back(p);
    mark_dirty();
    start_scan();
}
static void locate_smash() {
    std::string p = pick_path(A.hwnd, false, L"BattleShip.exe", L"*.exe");
    if (p.empty()) return;
    A.st.smash_exe = p;
    mark_dirty();
    A.hello_dirty = true;
    start_scan();
}

/* ---------- Library ---------- */

static void toggle_favorite(const std::string& key) {
    hub::set_favorite(key, !gstate(key).favorite);
    hub::flush_library_state();
    refresh_state(key);
}

static void star_shape(ImDrawList* dl, ImVec2 c, float r, bool filled, ImU32 col) {
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        float a = -3.14159265f / 2 + i * 3.14159265f / 5;
        float rr = (i & 1) ? r * 0.42f : r;
        pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
    }
    if (filled) dl->AddConvexPolyFilled(pts, 10, col);
    else dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.8f * S);
}

static void game_card(const hub::GameEntry& e, ImVec2 size, const char* section) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::PushID(section);
    ImGui::PushID(e.key.c_str());
    ImGui::InvisibleButton("card", size);
    bool item_hov = ImGui::IsItemHovered();
    bool focused = ImGui::IsItemFocused();
    bool hov = item_hov || focused;
    ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    float pad = 16 * S;
    float sr = 12 * S;
    ImVec2 sc(r1.x - pad - sr * 0.6f, r0.y + pad * 0.8f + sr + 2 * S);
    bool star_hov = item_hov && ImGui::IsMouseHoveringRect(sc - ImVec2(sr + 6 * S, sr + 6 * S), sc + ImVec2(sr + 6 * S, sr + 6 * S));
    bool play = false;
    if (ImGui::IsItemClicked()) {
        A.sel_game = e.key;
        if (star_hov) toggle_favorite(e.key);
    }
    if (item_hov && !star_hov && ImGui::IsMouseDoubleClicked(0)) play = true;
    if (focused) {
        if (A.sel_game != e.key && io.NavActive) A.sel_game = e.key;
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) play = true;
            if (ImGui::IsKeyPressed(ImGuiKey_F, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) toggle_favorite(e.key);
        }
    }
    float& an = A.card_anim[e.key];
    an += ((hov ? 1.f : 0.f) - an) * std::min(1.f, io.DeltaTime * 14.f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float grow = an * 5 * S;
    ImVec2 p0 = r0 - ImVec2(grow, grow), p1 = r1 + ImVec2(grow, grow);
    float rad = 18 * S;
    ImU32 c0, c1;
    entry_colors(e, &c0, &c1);

    dl->AddRectFilled(p0 + ImVec2(0, 6 * S), p1 + ImVec2(0, 10 * S), IM_COL32(0, 0, 0, (int)(60 + 60 * an)), rad + 4 * S);
    gradient_round_rect(dl, p0, p1, c0, c1, rad);
    dl->PushClipRect(p0, p1, true);
    dl->AddCircleFilled(ImVec2(p1.x - 40 * S, p0.y + 50 * S), 70 * S, IM_COL32(255, 255, 255, 28), 48);
    dl->AddCircleFilled(ImVec2(p1.x - 10 * S, p1.y - 10 * S), 50 * S, IM_COL32(0, 0, 0, 30), 48);
    if (hov) dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 14), rad);
    // system badge
    std::string badge = sys_badge(e.system);
    if (is_native(e)) badge += "  NATIVE";
    pill_at(dl, p0 + ImVec2(pad, pad * 0.8f), badge.c_str(), IM_COL32(0, 0, 0, 110));
    // title (wraps inside the card)
    dl->PushClipRect(p0 + ImVec2(0, pad * 0.8f + 34 * S), p1 - ImVec2(0, 34 * S), true);
    dl->AddText(A.f_h2, A.f_h2->FontSize, p0 + ImVec2(pad, pad * 0.8f + 38 * S), IM_COL32_WHITE, e.title.c_str(), nullptr,
                size.x - 2 * pad);
    dl->PopClipRect();
    // bottom line: play time
    const hub::GameState& gs = gstate(e.key);
    std::string pt = hubui::format_play_time(gs.play_seconds);
    dl->AddText(ImVec2(p0.x + pad, p1.y - pad - ImGui::GetFontSize() + 2 * S), IM_COL32(255, 255, 255, 215), pt.c_str());
    dl->PopClipRect();
    // favorite star
    ImVec2 sc2 = sc + ImVec2(0, 0);
    dl->AddCircleFilled(sc2, sr + 5 * S, IM_COL32(0, 0, 0, star_hov ? 120 : 70), 24);
    star_shape(dl, sc2, sr, gs.favorite, gs.favorite ? IM_COL32(255, 214, 70, 255) : IM_COL32(255, 255, 255, star_hov ? 255 : 190));
    if (A.sel_game == e.key) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 235), rad, 0, 3 * S);

    ImGui::PopID();
    ImGui::PopID();
    if (play) {
        A.sel_game = e.key;
        play_entry(e);
    }
}

static void card_strip(const char* id, const std::vector<int>& idx, ImVec2 cs) {
    float h = cs.y + 34 * S;
    ImGui::BeginChild(id, ImVec2(0, h), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::SetCursorPos(ImVec2(8 * S, 10 * S));
    for (size_t i = 0; i < idx.size(); ++i) {
        if (i) ImGui::SameLine();
        game_card(A.lib[idx[i]], cs, id);
    }
    ImGui::EndChild();
}

static bool chip(const char* label, bool on, ImU32 col) {
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 sz(ts.x + 30 * S, ts.y + 14 * S);
    bool pressed = ImGui::InvisibleButton(label, sz);
    bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    dl->AddRectFilled(p0, p1, on ? col : (hov ? mix(COL_PANEL2, IM_COL32_WHITE, 0.08f) : COL_PANEL), sz.y * 0.5f);
    if (ImGui::IsItemFocused()) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 200), sz.y * 0.5f, 0, 2 * S);
    dl->AddText(p0 + ImVec2(15 * S, 7 * S), on ? IM_COL32_WHITE : IM_COL32(190, 194, 210, 255), label);
    return pressed;
}

static void section_title(const std::string& t) {
    ImGui::PushFont(A.f_h2);
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopFont();
}

static void refresh_save_info(const hub::GameEntry& e) {
    A.info_key = e.key;
    A.info_at = ImGui::GetTime();
    A.info_battery_files = A.info_state_files = 0;
    A.info_battery_bytes = 0;
    A.info_auto_state = false;
    if (is_native(e)) return;
    std::string sys = sys_short(e);
    uint64_t sb = 0;
    count_files(hub::saves::battery_dir(sys, e.key), &A.info_battery_files, &A.info_battery_bytes);
    std::string sd = hub::saves::states_dir(sys, e.key);
    count_files(sd, &A.info_state_files, &sb);
    A.info_auto_state = file_exists(with_slash(sd) + "auto.state");
    if (A.info_auto_state && A.info_state_files > 0) --A.info_state_files;
}

static std::string size_text(uint64_t b) {
    char s[32];
    if (b < 1024) std::snprintf(s, sizeof s, "%llu B", (unsigned long long)b);
    else if (b < 1024 * 1024) std::snprintf(s, sizeof s, "%.1f KB", b / 1024.0);
    else std::snprintf(s, sizeof s, "%.1f MB", b / 1048576.0);
    return s;
}

static void open_in_explorer(const std::string& dir) {
    ShellExecuteW(nullptr, L"open", W(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void library_details(const hub::GameEntry& e, float height) {
    ImU32 c0, c1;
    entry_colors(e, &c0, &c1);
    const hub::GameState& gs = gstate(e.key);
    if (A.info_key != e.key || ImGui::GetTime() - A.info_at > 4.0) refresh_save_info(e);
    bool idle = A.proc == nullptr;
    bool native = is_native(e);

    begin_panel("details", ImVec2(0, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddRectFilled(wp + ImVec2(0, 18 * S), wp + ImVec2(5 * S, ImGui::GetWindowHeight() - 18 * S), c1, 3 * S);

    ImGui::PushFont(A.f_h2);
    ImGui::TextUnformatted(e.title.c_str());
    ImGui::PopFont();
    pill(hub::system_name(e.system), mix(c0, IM_COL32_BLACK, 0.2f));
    ImGui::SameLine();
    pill(native ? "NATIVE PORT" : "EMULATED", COL_PANEL2);
    if (supports_online(e)) {
        ImGui::SameLine();
        char pl[32];
        std::snprintf(pl, sizeof pl, "1-%d players", max_players_for(e));
        pill(pl, COL_PANEL2);
    }
    ImGui::TextDisabled("%s", e.path.c_str());
    std::string line = "Play time: " + hubui::format_play_time(gs.play_seconds) + "     Last played: " +
                       (gs.last_played ? fmt_time(gs.last_played) : std::string("never"));
    ImGui::TextUnformatted(line.c_str());
    if (native) {
        ImGui::TextDisabled("Smash Classic keeps its own saves.");
    } else {
        std::string sv = A.info_battery_files > 0
                             ? "Save data: " + std::to_string(A.info_battery_files) + " file(s), " + size_text(A.info_battery_bytes)
                             : std::string("Save data: none yet");
        sv += "     Save states: " + std::to_string(A.info_state_files);
        if (A.info_auto_state) sv += "  (+ resume point)";
        ImGui::TextUnformatted(sv.c_str());
    }
    ImGui::Dummy(ImVec2(0, 2 * S));

    if (button("Play", ImVec2(170 * S, 46 * S), true, idle)) play_entry(e);
    ImGui::SameLine();
    if (e.system == hub::System::GC) {
        button("Online coming soon", ImVec2(0, 46 * S), false, false);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Online via Dolphin's own netplay - coming soon");
    } else if (supports_online(e)) {
        if (button("Play Online", ImVec2(0, 46 * S), false, idle)) {
            A.pending_room_game = room_id(e);
            enter_room(true, hubnet::new_room_code());
        }
    }
    ImGui::SameLine();
    if (button("Open save folder", ImVec2(0, 46 * S), false, !native))
        open_in_explorer(hub::saves::game_dir(sys_short(e), e.key));
    ImGui::SameLine();
    if (button(gs.favorite ? "Remove favorite" : "Add to favorites", ImVec2(0, 46 * S))) toggle_favorite(e.key);
    if (native) {
        ImGui::SameLine();
        if (button("Locate BattleShip.exe", ImVec2(0, 46 * S))) locate_smash();
    }
    end_panel();
}

static void page_library() {
    page_header("Library", "Pick a game and press Play. Everything resumes where you left off.");

    // ---- search + scan controls
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) A.focus_search = true;
    if (A.focus_search) { ImGui::SetKeyboardFocusHere(); A.focus_search = false; }
    ImGui::SetNextItemWidth(std::min(380 * S, ImGui::GetContentRegionAvail().x * 0.5f));
    ImGui::InputTextWithHint("##search", "Search games...  (Ctrl+F)", A.search_buf, sizeof A.search_buf);
    ImGui::SameLine();
    if (button("Rescan", ImVec2(0, 0), false, !A.scanning)) start_scan();
    ImGui::SameLine();
    if (button("Add ROM folder", ImVec2(0, 0), A.lib.size() <= 1 && A.st.rom_folders.empty())) add_rom_folder();
    if (A.scanning) {
        int d = A.scan_done, t = A.scan_total;
        char ov[64];
        std::snprintf(ov, sizeof ov, t > 0 ? "Scanning games...  %d / %d" : "Scanning games...", d, t);
        ImGui::ProgressBar(t > 0 ? (float)d / (float)t : 0.f, ImVec2(std::min(520 * S, ImGui::GetContentRegionAvail().x), 22 * S), ov);
    }

    // ---- system chips
    struct Chip { const char* label; const char* sys; };
    static const Chip chips[] = {{"All", ""}, {"N64", "n64"}, {"GameCube", "gc"}, {"GBA", "gba"}, {"GBC", "gbc"}, {"GB", "gb"}};
    std::map<std::string, int> counts;
    for (auto& e : A.lib) ++counts[sys_short(e)];
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * S, 8 * S));
    bool first = true;
    for (auto& c : chips) {
        if (!first) ImGui::SameLine();
        first = false;
        char lab[48];
        int n = c.sys[0] ? counts[c.sys] : (int)A.lib.size();
        std::snprintf(lab, sizeof lab, "%s  %d", c.label, n);
        ImU32 a = COL_ACCENT, b2;
        if (c.sys[0]) system_colors(hub::system_from_short(c.sys), &a, &b2);
        if (chip(lab, A.sys_filter == c.sys, a)) { A.sys_filter = c.sys; A.vis_dirty = true; }
    }
    ImGui::PopStyleVar();

    if (A.vis_dirty || A.vis_filter != A.sys_filter || A.vis_query != A.search_buf) rebuild_vis();

    // selection must exist
    if (!find_entry(A.sel_game)) {
        A.sel_game.clear();
        if (!A.vis_recent.empty()) A.sel_game = A.lib[A.vis_recent[0]].key;
        else if (!A.vis.empty()) A.sel_game = A.lib[A.vis[0]].key;
    }
    const hub::GameEntry* sel = find_entry(A.sel_game);
    float det_h = sel ? 218 * S : 0;

    ImGui::BeginChild("libscroll", ImVec2(0, -(det_h + (sel ? 12 * S : 0))), ImGuiChildFlags_NavFlattened);
    if (A.lib.empty() && !A.scanning) {
        begin_panel("empty");
        section_title("No games yet");
        text_wrapped_disabled("Add the folder(s) that hold your own game files. Classic Hub recognises N64 (.z64 .v64 .n64), "
                              "GameCube (.iso .gcm .rvz .wia .ciso .gcz), Game Boy Advance (.gba), Game Boy (.gb) and "
                              "Game Boy Color (.gbc) by their contents, not their names.");
        if (button("Add ROM folder", ImVec2(0, 48 * S), true)) add_rom_folder();
        end_panel();
    } else {
        bool searching = A.search_buf[0] != 0;
        if (!searching && !A.vis_recent.empty()) {
            section_title("Continue playing");
            card_strip("recent", A.vis_recent, ImVec2(300 * S, 170 * S));
        }
        if (!searching && !A.vis_fav.empty()) {
            section_title("Favorites");
            card_strip("favs", A.vis_fav, ImVec2(230 * S, 130 * S));
        }
        char head[64];
        std::snprintf(head, sizeof head, "%s  (%d)", A.sys_filter.empty() ? "All games" : "Games", (int)A.vis.size());
        section_title(head);
        if (A.vis.empty()) {
            ImGui::TextDisabled("No games match.");
        } else {
            float avail = ImGui::GetContentRegionAvail().x - 24 * S;
            float gap = 18 * S, min_w = 230 * S;
            int cols = std::max(1, (int)((avail + gap) / (min_w + gap)));
            float cw = std::min(330 * S, (avail - gap * (cols - 1)) / cols);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
            ImGui::Dummy(ImVec2(0, 2 * S));
            ImGui::Indent(8 * S);
            for (size_t i = 0; i < A.vis.size(); ++i) {
                if (i % cols) ImGui::SameLine();
                game_card(A.lib[A.vis[i]], ImVec2(cw, 140 * S), "grid");
            }
            ImGui::Unindent(8 * S);
            ImGui::PopStyleVar();
            ImGui::Dummy(ImVec2(0, 10 * S));
        }
    }
    ImGui::EndChild();

    if (sel) {
        ImGui::Dummy(ImVec2(0, 0));
        library_details(*sel, det_h);
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
        if (!p->keys.empty()) {   // v2 peers: games per system
            std::map<std::string, int> per;
            for (auto& k : p->keys) ++per[k.substr(0, k.find(':'))];
            for (auto& kv : per) {
                ImU32 a, b;
                system_colors(hub::system_from_short(kv.first), &a, &b);
                char lab[48];
                std::snprintf(lab, sizeof lab, "%s %d", sys_badge(hub::system_from_short(kv.first)), kv.second);
                if (!first) ImGui::SameLine(0, 6 * S);
                first = false;
                pill(lab, mix(a, IM_COL32_BLACK, 0.15f));
            }
        } else {
            for (auto& g : hub::catalog()) {
                if (!p->games.count(g.id)) continue;
                ImU32 a, b;
                game_colors(g.id, &a, &b);
                if (!first) ImGui::SameLine(0, 6 * S);
                first = false;
                pill(short_name(g.id), mix(a, IM_COL32_BLACK, 0.15f));
            }
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

    // game picker: games every member owns (plus the current pick); the rest are summarised
    {
        std::vector<const hub::GameEntry*> cands;
        std::set<std::string> seen;
        for (auto& e : A.lib)
            if (supports_online(e) && seen.insert(room_id(e)).second) cands.push_back(&e);
        std::sort(cands.begin(), cands.end(), [](const hub::GameEntry* a, const hub::GameEntry* b) {
            if (is_native(*a) != is_native(*b)) return is_native(*a);
            std::string x = a->title, y = b->title;
            for (auto& c : x) c = (char)std::tolower((unsigned char)c);
            for (auto& c : y) c = (char)std::tolower((unsigned char)c);
            return x != y ? x < y : a->key < b->key;
        });
        int hidden = 0;
        for (auto* ep : cands) {
            const hub::GameEntry& g = *ep;
            std::string rid = room_id(g);
            auto sel = hubui::game_selectable(rid, max_players_for(g), peers, 0);
            bool chosen = A.selected_game == rid;
            if (!sel.ok && !chosen) { ++hidden; continue; }
            if (!host && !chosen) continue;
            ImGui::PushID(rid.c_str());
            ImVec2 sz(left_w, 54 * S);
            bool can_click = host && sel.ok;
            if (!can_click) ImGui::BeginDisabled();
            bool clicked = ImGui::InvisibleButton("row", sz);
            bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            if (!can_click) ImGui::EndDisabled();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
            ImU32 a, b;
            entry_colors(g, &a, &b);
            dl->AddRectFilled(p0, p1, chosen ? with_alpha(a, 0.35f) : (hov ? COL_PANEL2 : COL_PANEL), 12 * S);
            if (chosen) dl->AddRect(p0, p1, b, 12 * S, 0, 2 * S);
            dl->AddRectFilled(p0 + ImVec2(10 * S, 10 * S), p0 + ImVec2(16 * S, 44 * S), sel.ok ? b : IM_COL32(90, 92, 105, 255), 3 * S);
            std::string label = g.title + "   " + sys_badge(g.system);
            dl->AddText(p0 + ImVec2(30 * S, 13 * S), sel.ok ? IM_COL32_WHITE : IM_COL32(130, 132, 145, 255), label.c_str());
            if (!sel.ok) {
                ImVec2 ts = ImGui::CalcTextSize(sel.reason.c_str());
                dl->AddText(ImVec2(p1.x - ts.x - 16 * S, p0.y + 14 * S), mix(COL_BAD, IM_COL32_WHITE, 0.25f), sel.reason.c_str());
            } else if (chosen) {
                dl->AddText(ImVec2(p1.x - ImGui::CalcTextSize("Selected").x - 16 * S, p0.y + 14 * S), IM_COL32(255, 255, 255, 220), "Selected");
            }
            if (clicked && can_click) {
                A.selected_game = rid;
                A.st.last_game = rid;
                mark_dirty();
                A.lobby.send_msg("{\"kind\":\"select\",\"game\":\"" + hubui::json_escape(rid) + "\"}");
            }
            ImGui::PopID();
        }
        if (host && hidden > 0) ImGui::TextDisabled("%d more game(s) are hidden because not everyone in the room has them.", hidden);
        if (host && cands.empty()) ImGui::TextDisabled("No online-capable games in your library yet.");
    }

    ImGui::Dummy(ImVec2(0, 6 * S));
    const hub::GameEntry* rg = find_room_entry(A.selected_game);
    if (host) {
        bool ok = false;
        std::string why;
        if (!rg) why = "Choose a game to start";
        else {
            auto s = hubui::game_selectable(A.selected_game, max_players_for(*rg), peers, 0);
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
            A.lobby.send_msg(hubui::launch_json(A.selected_game, A.launch_counter, delay, seed, A.st.gb_core));
            launch_online(*rg, A.launch_counter, delay, true, A.st.gb_core);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", ok ? "Everyone is ready." : why.c_str());
    } else {
        ImGui::TextDisabled("%s", rg ? ("Waiting for the host to start " + rg->title + "...").c_str() : "Waiting for the host to pick a game...");
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

/* ---------- Saves ---------- */

static void refresh_vault() {
    A.vault_rows.clear();
    std::string root = with_slash(hub::saves::vault_dir());
    WIN32_FIND_DATAW sf;
    HANDLE hs = FindFirstFileW(W(root + "*").c_str(), &sf);
    if (hs != INVALID_HANDLE_VALUE) {
        do {
            std::string sys = U(sf.cFileName);
            if (!(sf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || sys.empty() || sys[0] == '.' || sys[0] == '_') continue;
            WIN32_FIND_DATAW gf;
            HANDLE hg = FindFirstFileW(W(root + sys + "\\*").c_str(), &gf);
            if (hg == INVALID_HANDLE_VALUE) continue;
            do {
                std::string g = U(gf.cFileName);
                if (!(gf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || g == "." || g == "..") continue;
                VaultRow r;
                if (hubui::parse_vault_meta(read_text_file(root + sys + "\\" + g + "\\meta.json"), &r.m)) {
                    if (r.m.system.empty()) r.m.system = sys;
                    A.vault_rows.push_back(r);
                }
            } while (FindNextFileW(hg, &gf));
            FindClose(hg);
        } while (FindNextFileW(hs, &sf));
        FindClose(hs);
    }
    std::sort(A.vault_rows.begin(), A.vault_rows.end(), [](const VaultRow& a, const VaultRow& b) { return a.m.updated > b.m.updated; });
    A.vault_dirty = false;
}

struct SyncSuggestion { std::string label, path; };
static const std::vector<SyncSuggestion>& sync_suggestions() {
    static std::vector<SyncSuggestion> v;
    static bool done = false;
    if (done) return v;
    done = true;
    std::set<std::string> seen;
    auto add = [&](const std::string& label, const std::string& base) {
        if (base.empty() || !dir_exists(base) || !seen.insert(base).second) return;
        v.push_back({label, no_slash(base) + "\\ClassicHub"});
    };
    add("OneDrive", env_var(L"OneDriveConsumer"));
    add("OneDrive", env_var(L"OneDrive"));
    add("OneDrive (work)", env_var(L"OneDriveCommercial"));
    std::string up = env_var(L"USERPROFILE");
    if (!up.empty()) {
        add("Dropbox", up + "\\Dropbox");
        add("Google Drive", up + "\\Google Drive");
        add("Google Drive", up + "\\My Drive");
        add("iCloud Drive", up + "\\iCloudDrive");
    }
    for (char d = 'D'; d <= 'Z'; ++d) {
        std::string gd = std::string(1, d) + ":\\My Drive";
        if (dir_exists(gd)) add("Google Drive", gd);
    }
    return v;
}

static void result_line(const std::string& s, bool ok) {
    if (s.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ok ? COL_GOOD : COL_BAD));
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::PopStyleColor();
}

static void page_saves() {
    page_header("Saves", "One portable save vault. Sync it between PCs or back it up; saves follow you across installs.");
    if (A.vault_dirty) refresh_vault();
    bool busy = A.job.running;

    begin_panel("vault");
    section_title("Save vault");
    text_wrapped_disabled(hub::saves::vault_dir());
    text_wrapped_disabled("Every game's in-game saves and save states live here. Copy this folder to move everything by hand.");
    if (button("Open vault folder", ImVec2(0, 0))) open_in_explorer(hub::saves::vault_dir());
    end_panel();
    ImGui::Dummy(ImVec2(0, 4 * S));

    begin_panel("sync");
    section_title("Sync between PCs");
    text_wrapped_disabled("Pick a folder both PCs can see (OneDrive, Dropbox, Google Drive, a USB stick or a NAS). Classic Hub keeps "
                          "it in step with the vault in both directions and never deletes a save; if both sides changed, the older copy is kept.");
    ImGui::TextUnformatted("Sync folder");
    text_wrapped_disabled(A.st.sync_folder.empty() ? "(not set)" : A.st.sync_folder);
    if (button("Browse...", ImVec2(0, 0), A.st.sync_folder.empty())) {
        std::string p = pick_path(A.hwnd, true, nullptr, nullptr);
        if (!p.empty()) { A.st.sync_folder = p; mark_dirty(); }
    }
    if (!A.st.sync_folder.empty()) {
        ImGui::SameLine();
        if (button("Stop syncing", ImVec2(0, 0))) { A.st.sync_folder.clear(); mark_dirty(); A.sync_result.clear(); }
    }
    for (auto& sg : sync_suggestions()) {
        if (sg.path == A.st.sync_folder) continue;
        ImGui::SameLine();
        std::string lab = "Use " + sg.label + "##" + sg.path;
        if (button(lab.c_str(), ImVec2(0, 0))) {
            make_dirs(sg.path);
            A.st.sync_folder = sg.path;
            mark_dirty();
            start_sync(true);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sg.path.c_str());
    }
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (ImGui::Checkbox("Sync automatically after playing", &A.st.sync_auto)) mark_dirty();
    if (ImGui::Checkbox("Include save states (bigger)", &A.st.sync_states)) mark_dirty();
    ImGui::Dummy(ImVec2(0, 2 * S));
    bool can_sync = !A.st.sync_folder.empty() && !busy;
    if (button(busy && A.job.kind == SaveJob::Sync ? "Syncing..." : "Sync now", ImVec2(180 * S, 46 * S), true, can_sync)) start_sync(true);
    if (!A.sync_result.empty()) {
        ImGui::SameLine();
        ImGui::BeginGroup();
        result_line(A.sync_result, A.sync_result_ok);
        ImGui::EndGroup();
    }
    end_panel();
    ImGui::Dummy(ImVec2(0, 4 * S));

    begin_panel("backup");
    section_title("Backup and restore");
    text_wrapped_disabled("Export writes every save to one .zip file. Import merges a zip back in (the newer file wins; the other is kept).");
    if (button("Export all saves...", ImVec2(0, 0), false, !busy)) {
        time_t t = time(nullptr);
        tm lt{};
        localtime_s(&lt, &t);
        wchar_t nm[64];
        std::wcsftime(nm, 64, L"ClassicHub-saves-%Y%m%d.zip", &lt);
        std::string p = pick_save_path(A.hwnd, nm, L"Zip archive", L"*.zip", L"zip");
        if (!p.empty()) start_job(SaveJob::Export, p, true);
    }
    ImGui::SameLine();
    if (button("Import saves...", ImVec2(0, 0), false, !busy)) {
        std::string p = pick_path(A.hwnd, false, L"Zip archive", L"*.zip");
        if (!p.empty()) start_job(SaveJob::Import, p, true);
    }
    if (busy && A.job.kind != SaveJob::Sync) { ImGui::SameLine(); ImGui::TextDisabled("Working..."); }
    result_line(A.export_result, A.export_ok);
    result_line(A.import_result, A.import_ok);
    end_panel();
    ImGui::Dummy(ImVec2(0, 4 * S));

    section_title("In the vault (" + std::to_string(A.vault_rows.size()) + ")");
    if (A.vault_rows.empty()) {
        ImGui::TextDisabled("Nothing yet. Play a game and its saves show up here.");
    } else if (ImGui::BeginTable("vaulttbl", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Last updated", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();
        for (auto& r : A.vault_rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.m.title.empty() ? r.m.key.c_str() : r.m.title.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(hub::system_name(hub::system_from_short(r.m.system)));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmt_time(r.m.updated).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.m.device.c_str());
        }
        ImGui::EndTable();
    }
}

/* ---------- Settings ---------- */

static void page_settings() {
    page_header("Settings", "");
    begin_panel("set1");
    ImGui::TextUnformatted("Player name");
    char nb[40];
    std::snprintf(nb, sizeof nb, "%s", A.st.name.c_str());
    ImGui::SetNextItemWidth(320 * S);
    if (ImGui::InputText("##name", nb, 25)) { A.st.name = nb; mark_dirty(); }
    ImGui::Dummy(ImVec2(0, 8 * S));

    ImGui::TextUnformatted("ROM folders");
    if (A.st.rom_folders.empty()) text_wrapped_disabled("(none yet)");
    int remove_at = -1;
    for (size_t i = 0; i < A.st.rom_folders.size(); ++i) {
        ImGui::PushID((int)i);
        if (button("Remove", ImVec2(0, 0))) remove_at = (int)i;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", A.st.rom_folders[i].c_str());
        ImGui::PopID();
    }
    if (remove_at >= 0) {
        A.st.rom_folders.erase(A.st.rom_folders.begin() + remove_at);
        mark_dirty();
        start_scan();
    }
    if (button("Add folder...", ImVec2(0, 0))) add_rom_folder();
    ImGui::SameLine();
    if (button("Rescan", ImVec2(0, 0), false, !A.scanning)) start_scan();
    ImGui::Dummy(ImVec2(0, 8 * S));

    ImGui::TextUnformatted("Game Boy / Game Boy Color emulator");
    int core = A.st.gb_core == "gambatte" ? 1 : 0;
    if (ImGui::RadioButton("SameBoy (most accurate)", core == 0)) { A.st.gb_core = "sameboy"; mark_dirty(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Gambatte (lighter)", core == 1)) { A.st.gb_core = "gambatte"; mark_dirty(); }
    ImGui::Dummy(ImVec2(0, 8 * S));

    if (ImGui::Checkbox("Start games fullscreen", &A.st.fullscreen)) mark_dirty();
    ImGui::TextDisabled("Applies to Game Boy and GameCube games. N64 games: press Alt+Enter.");
    ImGui::Dummy(ImVec2(0, 8 * S));

    ImGui::TextUnformatted("Smash Classic location");
    std::string se = smash_exe_path();
    text_wrapped_disabled(se.empty() ? "BattleShip.exe not found" : se);
    if (button("Browse for BattleShip.exe...", ImVec2(0, 0))) locate_smash();
    if (!A.st.smash_exe.empty()) {
        ImGui::SameLine();
        if (button("Use default", ImVec2(0, 0))) { A.st.smash_exe.clear(); mark_dirty(); A.hello_dirty = true; start_scan(); }
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

/* ---------- Dolphin download dialog + toast ---------- */

static void poll_dolphin() {
    if (DL.state == DolphinDl::Working && DL.done) {
        if (DL.th.joinable()) DL.th.join();
        if (DL.ok) {
            DL.state = DolphinDl::Idle;
            set_toast("Dolphin installed.");
            std::string key = DL.key;
            if (const hub::GameEntry* g = find_entry(key)) { hub::GameEntry copy = *g; play_entry(copy); }
        } else {
            DL.state = DolphinDl::Failed;
        }
    }
}

static void draw_overlays() {
    // Dolphin modal
    if (DL.state != DolphinDl::Idle) {
        ImGui::OpenPopup("Dolphin emulator");
        ImVec2 c = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(520 * S, 0));
        if (ImGui::BeginPopupModal("Dolphin emulator", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            if (DL.state == DolphinDl::Asking) {
                ImGui::TextWrapped("GameCube needs the Dolphin emulator (about 18 MB download, GPL). Download now?");
                ImGui::Dummy(ImVec2(0, 8 * S));
                if (button("Download", ImVec2(160 * S, 44 * S), true)) start_dolphin_install();
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (button("Cancel", ImVec2(160 * S, 44 * S))) DL.state = DolphinDl::Idle;
            } else if (DL.state == DolphinDl::Working) {
                static const char* phases[] = {"Downloading Dolphin...", "Unpacking...", "Installing..."};
                ImGui::TextUnformatted(phases[std::min(2, (int)DL.phase)]);
                uint64_t got = DL.got, tot = DL.total;
                char ov[48];
                std::snprintf(ov, sizeof ov, "%.1f / %.1f MB", got / 1048576.0, tot / 1048576.0);
                ImGui::ProgressBar(DL.phase > 0 ? 1.f : (tot ? (float)got / (float)tot : 0.f), ImVec2(-1, 24 * S), DL.phase > 0 ? "" : ov);
                ImGui::TextDisabled("Your game starts as soon as it is ready.");
            } else {
                std::string err;
                { std::lock_guard<std::mutex> lk(DL.mu); err = DL.error; }
                ImGui::TextWrapped("Could not install Dolphin: %s", err.c_str());
                ImGui::Dummy(ImVec2(0, 8 * S));
                if (button("Close", ImVec2(160 * S, 44 * S), true)) DL.state = DolphinDl::Idle;
                ImGui::SetItemDefaultFocus();
            }
            if (DL.state == DolphinDl::Idle) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    // toast
    if (!A.toast.empty() && ImGui::GetTime() < A.toast_until) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        float maxw = std::min(460 * S, ds.x * 0.6f);
        ImVec2 ts = ImGui::CalcTextSize(A.toast.c_str(), nullptr, false, maxw);
        ImVec2 p1(ds.x - 24 * S, ds.y - 52 * S);
        ImVec2 p0(p1.x - ts.x - 36 * S, p1.y - ts.y - 24 * S);
        dl->AddRectFilled(p0 + ImVec2(0, 4 * S), p1 + ImVec2(0, 6 * S), IM_COL32(0, 0, 0, 80), 12 * S);
        dl->AddRectFilled(p0, p1, COL_PANEL2, 12 * S);
        dl->AddRectFilled(p0, ImVec2(p0.x + 5 * S, p1.y), A.toast_col, 3 * S);
        dl->AddText(nullptr, 0, p0 + ImVec2(20 * S, 12 * S), IM_COL32_WHITE, A.toast.c_str(), nullptr, maxw);
    }
}

static void page_about() {
    page_header("About", "");
    begin_panel("about");
    ImGui::PushFont(A.f_h2);
    ImGui::TextUnformatted("Classic Hub");
    ImGui::PopFont();
    ImGui::TextDisabled("Version %s", HUB_VERSION_STR);
    ImGui::Dummy(ImVec2(0, 6 * S));
    ImGui::TextWrapped("One library for N64, GameCube, Game Boy Advance, Game Boy and Game Boy Color: one-click resume, "
                       "room codes, online play, and saves that follow you between PCs.");
    ImGui::Dummy(ImVec2(0, 6 * S));
    text_wrapped_disabled("Smash Classic runs as a native port with rollback netplay. N64 games run in the built-in "
                          "mupen64plus runner and Game Boy games in the built-in libretro runner, both with lockstep input netplay. "
                          "GameCube games use Dolphin. You supply your own game files; none are included.");
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
    if (nav_item(Page::Saves, A.job.running ? "Saves  (syncing...)" : "Saves", iw)) A.page = Page::Saves;
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
        case Page::Saves: page_saves(); break;
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
    draw_overlays();
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
    if (const char* pg = std::getenv("HUB_START_PAGE")) {   // screenshot/testing aid: 0 Library, 1 Online, 2 Saves, 3 Controllers, 4 Settings, 5 About
        int n = std::atoi(pg);
        if (n >= 0 && n <= 5) A.page = static_cast<Page>(n);
    }
    if (A.st.name.empty() || A.st.name == "Player") {   // default to the Windows user name
        wchar_t un[128];
        DWORD ul = 128;
        std::string u = GetUserNameW(un, &ul) ? U(un) : "";
        A.st.name = hubui::sanitize_player_name(u);
    }
    if (A.st.rom_folders.empty()) {
        // Default ROM folder: %USERPROFILE%\Desktop\Everything\N64Games if it exists.
        std::string d = env_var(L"USERPROFILE") + "\\Desktop\\Everything\\N64Games";
        if (dir_exists(d)) A.st.rom_folders.push_back(d);
    }
    if (!A.st.vault_dir.empty()) hub::saves::set_vault_dir(A.st.vault_dir);

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
    auto_sync();   // pull in saves from other PCs at start

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
        poll_job();
        poll_dolphin();
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
    if (A.job.th.joinable()) A.job.th.join();
    if (DL.th.joinable()) DL.th.join();
    hub::flush_library_state();
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
