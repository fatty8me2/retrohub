/* RetroHub.exe - the launcher front door.
 *
 * Layout: full-window custom UI (no floating windows): left sidebar nav + content pages
 * (Library, Play Online, Saves, Controllers, Settings, About). Drawn with Dear ImGui on SDL_Renderer.
 *
 * Launch contracts:
 *  - N64 (emulated):  n64run.exe --rom <path> --game <id|unknown> --save-dir <battery dir>
 *                     [--net <server> --room <CODE>-<launch> --slot <lobby slot> --players <n>
 *                      --ports <p0,p1,p2,p3> --delay <d>]
 *  - GBA/GB/GBC/DS:   retrorun.exe --core <x_libretro.dll> --rom <path> --system <gba|gb|gbc|nds>
 *                     --save-dir <battery dir> --state-dir <states dir> --resume [--fullscreen]
 *                     [--net ... same flags as n64run; online uses a fresh temp save dir, no --resume]
 *  - GameCube:        Dolphin.exe -b -u <data>\dolphin-user -e <game>  (INI keys written before launch)
 *  - 3DS:             <data>\emulators\azahar\azahar.exe -f <game>   (src/hub/emulators.cpp; downloaded on demand)
 *  - Wii U:           Cemu.exe -g <game> -f  (an existing install, else <data>\emulators\cemu with a portable folder)
 *  - Smash (native):  BattleShip.exe --from-hub --no-update-check
 *                     (ALWAYS passed: skips handing back to the hub, and RetroHub's updater is the only one)
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
#include "imgui_internal.h"

#include <windows.h>
#include <shobjidl.h>
#include <winhttp.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
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

#include "art.h"
#include "updater.h"
#include "games.h"
#include "emulators.h"
#include "library.h"
#include "logic.h"
#include "savestore.h"
#include "shell_integration.h"
#include "session.h"
#include "ui_kit.h"

#ifndef HUB_VERSION_STR
#define HUB_VERSION_STR "0.0.0"
#endif

using hub::GameInfo;
using hub::RomFile;

/* ============================ helpers ============================ */

static float& S = ui::S;  // DPI scale (lives in ui_kit)

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
/* Short relative form for tiles: "Today 08:29", "Yesterday", "3 days ago", else the date. */
static std::string fmt_ago(uint64_t unix_s) {
    if (!unix_s) return "never";
    time_t t = (time_t)unix_s, now = time(nullptr);
    tm lt{}, nt{};
    localtime_s(&lt, &t);
    localtime_s(&nt, &now);
    int days = (int)((now - t) / 86400);
    if (lt.tm_year == nt.tm_year && lt.tm_yday == nt.tm_yday) return fmt_time(unix_s);
    if (days <= 1) return "Yesterday";
    if (days < 14) return std::to_string(days) + " days ago";
    char b[32];
    std::strftime(b, sizeof b, "%b %d, %Y", &lt);
    return b;
}

/* ============================ drawing helpers (palette, widgets, icons live in ui_kit) ============================ */

using ui::button;
using ui::chip;
using ui::gradient_round_rect;
using ui::mix;
using ui::pill;
using ui::pill_at;
using ui::sp;
using ui::with_alpha;

static const ImU32 COL_ACCENT = ui::ACCENT;
static const ImU32 COL_GOOD = ui::GOOD;
static const ImU32 COL_BAD = ui::BAD;
static const ImU32 COL_WARN = ui::WARN;
static const ImU32 COL_PANEL = ui::PANEL;
static const ImU32 COL_PANEL2 = ui::PANEL2;

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
        case hub::System::N3DS: *a = IM_COL32(170, 25, 55, 255); *b = IM_COL32(235, 90, 105, 255); break;
        case hub::System::WiiU: *a = IM_COL32(0, 100, 170, 255); *b = IM_COL32(60, 190, 230, 255); break;
        case hub::System::NDS: *a = IM_COL32(130, 130, 145, 255); *b = IM_COL32(205, 210, 225, 255); break;
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
        case hub::System::N3DS: return "3DS";
        case hub::System::WiiU: return "WII U";
        case hub::System::NDS: return "NINTENDO DS";
        default: return "?";
    }
}
static void entry_colors(const hub::GameEntry& e, ImU32* a, ImU32* b) {
    if (e.system == hub::System::N64 && !e.catalog_id.empty() && hub::find_game(e.catalog_id)) game_colors(e.catalog_id, a, b);
    else system_colors(e.system, a, b);
}

/* ============================ app state ============================ */

enum class Page { Library, Online, Saves, Controllers, Settings, About };
enum class LibView { Home, All };

/* A transient message (bottom right). */
struct Toast {
    std::string text;
    ImU32 col = 0;
    double t0 = 0, until = 0;
};
/* Smooth horizontal scroll state of one shelf. */
struct ShelfState {
    float scroll = 0, target = 0;
    int focus_idx = -1;
    bool pressed = false, dragging = false;     // click-and-drag panning
    float press_x = 0, press_y = 0, press_scroll = 0;
    hubui::FRect rect;                          // visible screen rect of the row (for wheel routing)
    float max_scroll = 0, tw = 0, gap = 0;
    int frame = -100;                           // frame this row was last drawn
};

struct ChatLine { std::string who, text; };

/* The game currently running as a child process. */
struct RunInfo {
    std::string key, system_short, title;
    uint64_t start_unix = 0;
    ULONGLONG start_tick = 0;
    bool track = true;      // record play time / touch the save vault (false for the native Smash port)
    bool online = false;
    std::function<void()> after_exit;   // runs once when the process ended (3DS/Wii U: copy the emulator save folder into the vault)
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

    bool update_prompt_dismissed = false;   // "Later" / "Skip" pressed this run
    bool update_check_requested = false;    // the player pressed "Check for updates"
    Page page = Page::Library;

    // library scan
    std::thread scan_th;
    std::atomic<bool> scanning{false}, scan_ready{false};
    std::atomic<int> scan_done{0}, scan_total{0};
    std::vector<hub::GameEntry> scan_result;
    std::vector<hub::GameEntry> lib;              // scanned games (+ native Smash entry first)
    std::map<std::string, int> lib_index;         // key -> index in lib
    std::map<std::string, hub::GameState> lib_state;   // cached game_state per key
    std::string sys_filter;                       // "" = all, else system short
    bool online_only = false;                     // All Games: only games that are fun online
    int group_mode = 0;                           // All Games grouping: 0 genre, 1 system, 2 A-Z
    char search_buf[64] = {0};
    std::string vis_filter, vis_query;            // last inputs used to build `vis`
    std::vector<int> vis;                         // filtered + sorted indices into lib
    std::vector<hubui::ShelfGame> sg;             // parallel to `lib`, input of the shelf composition
    std::vector<hubui::Shelf> shelves;            // Home screen shelves (indices into lib)
    std::vector<int> pick_idx;                    // "Top Picks you don't have": indices into hub::catalog()
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
    hubui::SyncGate sync_gate;                    // a sync was requested while a job or a game was running
    bool share_host_save = true;                  // room option (host-controlled): all PCs use the host's N64 save
    int my_locals = 1;                            // local controllers I play with online (1-2)
    std::map<int, int> locals;                    // other members' local counts by lobby slot
    std::map<int, hubui::MemberPick> picks;       // other members' fighter picks by lobby slot (Smash match picker)
    hubui::MemberPick my_pick;                    // my fighter/costume (+ 2nd local player's)
    hubui::MatchRules rules;                      // stage + stocks: the host's choice, mirrored on guests
    int pick_edit = 0;                            // which local player the picker edits (0/1)
    std::string run_fighter;                      // my fighter in the running match ("" = none), for friends presence
    std::string sync_result;                      // last result summary (Saves page)
    bool sync_result_ok = true;
    std::string export_result, import_result;
    bool export_ok = true, import_ok = true;
    std::vector<VaultRow> vault_rows;
    bool vault_dirty = true;

    std::vector<Toast> toasts;

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
    bool room_update_pending = false;   // guest pressed "Update now": waiting for the release check
    bool room_update_none = false;      // that check finished with no newer release
    bool my_ready = false;
    std::string selected_game;   // room's chosen game (room id: catalog id or library key)
    std::string pending_room_game;   // preselect when hosting from the library
    int launch_counter = 0;
    std::vector<ChatLine> chat;
    char chat_buf[256] = {0};
    bool chat_scroll = false;

    // child process
    HANDLE proc = nullptr;
    HANDLE job_h = nullptr;     // job object holding an external emulator and anything it starts (3DS / Wii U)
    std::string proc_title;

    // ui
    std::string sel_game;        // library selection (game key)
    bool focus_search = false;
    LibView view = LibView::Home;
    std::string env_select;      // HUB_SELECT_GAME
    std::string hero_key;        // game the hero header currently shows (for the fade-in)
    float hero_t = 1.f;
    bool show_details = false;   // request to open the details modal
    std::map<std::string, ShelfState> shelf_state;
    std::map<std::string, double> art_seen;   // first time art was requested per game (for the loading shimmer)
    bool page_scrollable = false;             // the Home shelves overflow vertically (plain wheel then scrolls the page)
    bool focused = true;                      // hub window has input focus
    bool pad_mode = false;                    // last input came from a gamepad (footer shows controller glyphs)
    float side_w = 0;                         // animated sidebar width
    bool nav_seed = true;                     // put the (invisible) keyboard/gamepad focus on the hero Play / selected tile
    std::vector<ImGuiID> side_ids, side_ids_prev;   // sidebar item ids (this / last frame): tells which zone has the D-pad focus
    int zone_req = 0;                         // 1 = move focus to the sidebar, 2 = back to the content (set when a D-pad move found nothing)
    bool seed_side = false;
    Page last_page = Page::Library;
    LibView last_view = LibView::Home;

    // controllers
    std::vector<SDL_GameController*> pads;
    int pad_sel = 0;
} A;

static bool g_headless = false;   // HUB_SCREENSHOT run: never launch games or touch the lobby

static void push_toast(const std::string& s, ImU32 col, double secs = 6.0) {
    double now = ImGui::GetTime();
    if (!A.toasts.empty() && A.toasts.back().text == s && A.toasts.back().until > now) {   // same message again: just extend it
        A.toasts.back().until = now + secs;
        return;
    }
    A.toasts.push_back({s, col, now, now + std::min(secs, 14.0)});
    if (A.toasts.size() > 4) A.toasts.erase(A.toasts.begin());
}
static void set_notice(const std::string& s, double secs = 8.0) { push_toast(s, ui::WARN, secs); }
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

/* Curated catalog entry for a scanned game (id/MD5 first, then serial + system). */
static const GameInfo* catalog_for(const hub::GameEntry& e) {
    if (!e.catalog_id.empty())
        if (const GameInfo* g = hub::find_game(e.catalog_id)) return g;
    std::string md5;
    if (e.key.compare(0, 4, "n64:") == 0)
        for (char c : e.key.substr(4)) md5 += (char)std::toupper((unsigned char)c);
    return hub::find_game_for(sys_short(e), e.serial, md5);
}

static void rebuild_vis() {
    std::vector<hubui::ListItem> items;
    items.reserve(A.lib.size());
    for (auto& e : A.lib) items.push_back({e.key, e.title, sys_short(e), is_native(e)});
    std::string q = A.search_buf;
    A.vis = hubui::filter_sort(items, A.sys_filter, q);
    if (A.online_only) {
        std::vector<int> keep;
        for (int i : A.vis) {
            const hub::GameEntry& e = A.lib[i];
            const GameInfo* c = catalog_for(e);
            if (is_native(e) || (c && c->online)) keep.push_back(i);
        }
        A.vis.swap(keep);
    }

    // Home composition: shelves over the whole library, plus catalog games the player doesn't own.
    A.sg.clear();
    std::set<std::string> owned_ids;
    for (auto& e : A.lib) {
        const GameInfo* cat = catalog_for(e);
        if (cat) owned_ids.insert(cat->id);
        const hub::GameState& gs = gstate(e.key);
        hubui::ShelfGame g;
        g.key = e.key;
        g.title = e.title;
        g.system = sys_short(e);
        g.favorite = gs.favorite;
        g.last_played = gs.last_played;
        g.online = is_native(e) || (cat && cat->online);
        g.pinned = is_native(e);
        g.genre = cat ? cat->genre : std::string();
        A.sg.push_back(g);
    }
    A.shelves = hubui::compose_shelves(A.sg);
    const std::vector<GameInfo>& cat = hub::catalog();
    std::vector<bool> owned(cat.size()), online(cat.size());
    for (size_t i = 0; i < cat.size(); ++i) {
        owned[i] = cat[i].engine == hub::Engine::Native || owned_ids.count(cat[i].id) > 0;
        online[i] = cat[i].online;
    }
    A.pick_idx = hubui::pick_unowned(owned, online, 16);
    A.vis_filter = A.sys_filter;
    A.vis_query = q;
    A.vis_dirty = false;
}

/* ---------- online capability ---------- */

static bool supports_online(const hub::GameEntry& e) {
    return e.system == hub::System::N64 || e.system == hub::System::GBA || e.system == hub::System::GB ||
           e.system == hub::System::GBC;
}
/* Online player cap for a room (online_max_for; 0 online_max = max_players). Every online limit uses this. */
static int max_players_for(const hub::GameEntry& e) {
    if (const GameInfo* g = hub::find_game(e.catalog_id)) return hub::online_max_for(*g);
    return (e.system == hub::System::GB || e.system == hub::System::GBC) ? 2 : 4;
}
/* Local player count shown on cards (not the online cap). */
static int local_players_for(const hub::GameEntry& e) {
    if (const GameInfo* g = hub::find_game(e.catalog_id)) return g->max_players;
    return max_players_for(e);
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
    push_toast(s, good ? COL_GOOD : COL_BAD, secs);
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

struct SyncSuggestion { std::string label, path; };
static const std::vector<SyncSuggestion>& sync_suggestions() {
    static std::vector<SyncSuggestion> v;
    static bool done = false;
    if (done) return v;
    done = true;
    std::set<std::string> seen;
    auto add = [&](const std::string& label, const std::string& base) {
        if (base.empty() || !dir_exists(base) || !seen.insert(base).second) return;
        v.push_back({label, no_slash(base) + "\\RetroHub"});
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

static void start_sync(bool manual) {
    if (A.st.sync_folder.empty()) return;
    /* never touch battery files under a running emulator: queue until the game exits */
    if (hubui::sync_request(A.sync_gate, manual, A.proc != nullptr, A.job.running) == hubui::SyncAction::Queue) return;
    start_job(SaveJob::Sync, A.st.sync_folder, manual);
}
static void run_pending_sync() {
    bool manual = false;
    if (hubui::sync_take_pending(A.sync_gate, A.proc != nullptr, A.job.running, &manual)) start_job(SaveJob::Sync, A.st.sync_folder, manual);
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

static void fr_adopt_identity_after_vault_change();   // friends_ui.inc
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
    if ((j.kind == SaveJob::Sync || j.kind == SaveJob::Import) && j.report.ok) fr_adopt_identity_after_vault_change();
    A.vault_dirty = true;
    A.info_at = -100;
    j.kind = SaveJob::None;
    j.running = false;
    j.done = false;
    std::function<void()> next;
    next.swap(A.after_job);
    if (next) next();
}

/* ---------- process launching ---------- */

static bool spawn(const std::string& exe, const std::string& args, const std::string& cwd,
                  const std::vector<std::pair<std::string, std::string>>& env, const RunInfo& ri) {
    if (g_headless) { set_notice("Headless test run: launch skipped."); return false; }
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
    if (ri.after_exit) {   // external emulator: follow every process it starts, so a relaunch / hand-off keeps the session open
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job && !AssignProcessToJobObject(job, pi.hProcess)) { CloseHandle(job); job = nullptr; }
        A.job_h = job;
    }
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
    if (A.job_h) {   // the emulator may have handed the game to a child: wait until the whole job is gone
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ji{};
        if (QueryInformationJobObject(A.job_h, JobObjectBasicAccountingInformation, &ji, sizeof ji, nullptr) && ji.ActiveProcesses > 0) return;
        CloseHandle(A.job_h);
        A.job_h = nullptr;
    }
    DWORD code = 0;
    GetExitCodeProcess(A.proc, &code);
    CloseHandle(A.proc);
    A.proc = nullptr;
    if (A.run.after_exit) { auto fn = std::move(A.run.after_exit); A.run.after_exit = nullptr; fn(); }
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

/* Game window caption: "<title> — RetroHub" (UTF-8 em dash), quoted for the command line. */
static std::string title_arg(const std::string& game_title) {
    std::string t = game_title;
    t.erase(std::remove(t.begin(), t.end(), '"'), t.end());   // a quote would break the argument
    return " --title " + quote(t + " \xE2\x80\x94 RetroHub");
}

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
    HINTERNET ses = WinHttpOpen(L"RetroHub", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
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

/* Env for every Smash Classic launch: ROM hand-off + vault save path. Migrates the legacy save once (copy, never delete). */
static std::vector<std::pair<std::string, std::string>> smash_env(const std::string& exe) {
    std::vector<hubui::RomRef> refs;
    for (auto& g : A.lib) refs.push_back({g.catalog_id, g.serial, g.path});
    std::string rom = hubui::find_smash_rom(refs);
    std::string save = hubui::smash_save_file(hub::saves::battery_dir(hubui::kSmashSaveSystem, hubui::kSmashSaveKey));
    std::string legacy = dir_of(exe) + "ssb64_save.bin";
    bool vault_has = file_exists(save), legacy_has = file_exists(legacy);
    if (hubui::smash_should_migrate_save(vault_has, legacy_has)) {
        CopyFileW(W(legacy).c_str(), W(save).c_str(), TRUE);
    } else if (!A.st.smash_save_notice_shown &&
               hubui::smash_save_conflict(vault_has, legacy_has, read_text_file(save), read_text_file(legacy))) {
        // both exist and differ: the vault wins; tell the player once where the old local save is
        A.st.smash_save_notice_shown = true;
        mark_dirty();
        push_toast(hubui::smash_save_conflict_text(legacy), ui::WARN, 12.0);
    }
    if (!file_exists(dir_of(exe) + "BattleShip.o2r")) push_toast("Preparing Smash Classic for first use (about 30 s)...", ui::WARN, 12.0);
    return hubui::smash_launch_env(rom, save);
}

static RunInfo run_info_for(const hub::GameEntry& e, bool online) {
    RunInfo ri;
    ri.key = e.key;
    ri.system_short = is_native(e) ? std::string(hubui::kSmashSaveSystem) : sys_short(e);
    ri.title = e.title;
    ri.track = true;   // the native port is tracked too: its save lives in the vault (n64 / native:smash64)
    ri.online = online;
    return ri;
}

static void play_entry(const hub::GameEntry& e);

/* ---------- 3DS (Azahar) and Wii U (Cemu): external emulators, see docs/EMULATORS.md ---------- */

static hubemu::Installer EMU;   // download-on-demand job + its dialog state

/* Explicit, confirmed unpacking of a zipped 3DS image (Azahar cannot open .zip files). */
struct UnzipJob {
    enum State { Idle, Asking, Working, Failed } state = Idle;
    std::thread th;
    std::atomic<bool> done{false}, ok{false}, cancel{false};
    std::atomic<uint64_t> got{0}, total{0};
    std::string key, error;
    std::mutex mu;
} UNZ;

/* Size of the image inside a zipped 3DS game (read once from the zip's directory). */
static uint64_t unpacked_size(const hub::GameEntry& e) {
    static std::map<std::string, uint64_t> cache;
    auto it = cache.find(e.path);
    if (it == cache.end()) it = cache.emplace(e.path, hub::zip_3ds_unpacked_size(e)).first;
    return it->second ? it->second : e.size;
}

static std::string launch_path_for(const hub::GameEntry& e) {
    if (e.issue == "extract") return hub::extracted_3ds_path(e);
    return e.path;
}

static void launch_external(const hub::GameEntry& e, const RunInfo& ri, const std::string& battery) {
    hubemu::Kind kind = hubemu::kind_for(e.system);
    if (e.system == hub::System::N3DS && e.issue == "extract" && hub::extracted_3ds_path(e).empty()) {
        UNZ.key = e.key;
        UNZ.state = UnzipJob::Asking;
        return;
    }
    hubemu::Install inst = hubemu::find(kind, A.st.cemu_exe, e.path);
    if (inst.exe.empty()) {   // not installed: ask, download, then start this game
        EMU.kind = kind;
        EMU.game_key = e.key;
        EMU.state = hubemu::Installer::Asking;
        return;
    }
    if (g_headless) { set_notice("Headless test run: launch skipped."); return; }
    if (hubemu::is_running(inst.exe)) {   // a second launch would hand over to the running instance and end the session early
        set_notice(std::string(hubemu::name(kind)) + " is already running. Close it first, then start the game from RetroHub.", 10.0);
        return;
    }
    if (kind == hubemu::Kind::Azahar) {
        if (inst.managed) hubemu::prepare_azahar(inst.dir);
    } else if (inst.managed) {
        hubemu::prepare_cemu_managed(inst.dir);
    }
    std::string game = launch_path_for(e);
    std::string args = kind == hubemu::Kind::Azahar ? hubemu::azahar_args(game, A.st.fullscreen) : hubemu::cemu_args(game, A.st.fullscreen);
    if (e.issue == "encrypted") set_notice("This 3DS image is encrypted: Azahar needs your own AES keys (aes_keys.txt in its user\\sysdata folder).", 12.0);
    if (e.issue == "keys") set_notice("Disc images (.wud/.wux) need your disc key in Cemu's keys.txt. A .wua needs no keys.", 12.0);
    auto sess = std::make_shared<hubemu::SaveSession>(hubemu::begin_session(e, inst, battery, ""));
    RunInfo r = ri;
    r.after_exit = [sess]() {   // emulator save folder -> vault, before the sync runs
        hubemu::end_session(*sess);
        if (!sess->error.empty()) push_toast("Could not copy the save into the vault: " + sess->error + " (your emulator's copy is untouched)", ui::WARN, 15.0);
    };
    spawn(inst.exe, args, inst.dir, {}, r);
}

static void start_unzip(const hub::GameEntry& e) {
    if (UNZ.th.joinable()) UNZ.th.join();
    UNZ.done = false;
    UNZ.ok = false;
    UNZ.cancel = false;
    UNZ.got = 0;
    UNZ.total = unpacked_size(e);
    UNZ.error.clear();
    UNZ.state = UnzipJob::Working;
    hub::GameEntry copy = e;
    UNZ.th = std::thread([copy]() {
        std::string out, err;
        bool ok = hub::extract_3ds_zip(copy, [](uint64_t done, uint64_t total) { UNZ.got = done; UNZ.total = total; return !UNZ.cancel.load(); }, &out, &err);
        if (!ok) { std::lock_guard<std::mutex> lk(UNZ.mu); UNZ.error = err; }
        UNZ.ok = ok;
        UNZ.done = true;
    });
}
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
        spawn(exe, "--from-hub --no-update-check", dir_of(exe), smash_env(exe), ri);   // RetroHub's updater is the only one
        return;
    }
    std::string battery = no_slash(hub::saves::battery_dir(sys, e.key));
    std::string states = no_slash(hub::saves::states_dir(sys, e.key));
    switch (e.system) {
        case hub::System::N64: {
            std::string args = "--rom " + quote(e.path) + " --game " + (e.catalog_id.empty() ? "unknown" : e.catalog_id) +
                               " --save-dir " + quote(battery) + title_arg(e.title);
            if (A.st.fullscreen) args += " --fullscreen";
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
                               " --state-dir " + quote(states) + " --resume" + title_arg(e.title);
            if (A.st.fullscreen) args += " --fullscreen";
            spawn(hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
            break;
        }
        case hub::System::NDS: {   // melonDS DS core; no --resume (a stale auto state could overwrite a save synced from another PC)
            std::string core = find_core("melondsds");
            if (core.empty()) { set_notice("Emulator core for Nintendo DS not found (cores folder)."); return; }
            std::string args = "--core " + quote(core) + " --rom " + quote(e.path) + " --system nds --save-dir " + quote(battery) +
                               " --state-dir " + quote(states) + title_arg(e.title);
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
        case hub::System::N3DS:
        case hub::System::WiiU:
            launch_external(e, ri, battery);
            break;
        default: set_notice("Unsupported system."); break;
    }
}

static std::vector<hubui::MemberPads> member_pads();
static void launch_online(const hub::GameEntry& e, int launch_no, int delay, bool is_host, const std::string& core_in, bool share_save,
                          const hubui::PortPlan* host_plan = nullptr, const hubui::Json* launch_msg = nullptr,
                          const hubui::MatchSpec* host_match = nullptr) {
    A.st.last_game = room_id(e);
    mark_dirty();
    RunInfo ri = run_info_for(e, true);
    if (is_native(e)) {
        std::string exe = smash_exe_path();
        std::vector<std::pair<std::string, std::string>> env;
        env.push_back({is_host ? "SSB64_LOBBY_HOST" : "SSB64_LOBBY_JOIN", server_url()});
        env.push_back({"SSB64_LOBBY_CODE", A.code});
        for (auto& kv : smash_env(exe)) env.push_back(kv);   // online uses the same vault save as offline
        spawn(exe, "--from-hub --no-update-check", dir_of(exe), env, ri);   // RetroHub's updater is the only one
        return;
    }
    /* port i -> i-th member in slot order (slots may have gaps); --slot stays the real lobby slot */
    /* The host builds the plan once and ships it; guests use it verbatim (fallback: derive locally for older hosts). */
    hubui::PortPlan plan;
    if (host_plan) plan = *host_plan;
    else if (launch_msg && hubui::parse_launch_plan(*launch_msg, &plan.ports, &plan.players)) {}
    else plan = hubui::build_ports(member_pads());
    {
        bool mine = false;   // this PC's slot must own a port, else the launch cannot work
        std::string tok;
        for (size_t i = 0; i <= plan.ports.size(); ++i) {
            if (i == plan.ports.size() || plan.ports[i] == ',') { if (tok == std::to_string(A.lobby.my_slot())) mine = true; tok.clear(); }
            else tok += plan.ports[i];
        }
        if (!mine) { set_notice("The host's port plan doesn't include this PC."); return; }
    }
    /* Smash match picker: the host's resolved spec rides in the launch message; every PC passes the identical --match. */
    std::string match_args;
    A.run_fighter.clear();
    if (e.system == hub::System::N64 && hubui::match_game_supported(e.catalog_id)) {
        hubui::MatchSpec ms;
        bool have = host_match != nullptr;
        if (have) ms = *host_match;
        else if (launch_msg && launch_msg->get("match")) {
            have = hubui::parse_match(*launch_msg, plan.ports, e.catalog_id == "remix", &ms);
            if (!have) { set_notice("The host's match setup is invalid - ask them to update RetroHub."); return; }
        }
        if (have) {
            match_args = " --match " + quote(hubui::match_arg(ms));
            int port = 0, my = -1;
            std::string tok;
            for (size_t i = 0; i <= plan.ports.size() && my < 0; ++i) {
                if (i == plan.ports.size() || plan.ports[i] == ',') {
                    if (tok == std::to_string(A.lobby.my_slot())) my = port;
                    if (!tok.empty() && tok != "-1") ++port;
                    tok.clear();
                } else tok += plan.ports[i];
            }
            if (my >= 0 && my < (int)ms.players.size()) A.run_fighter = hubui::fighter_name(ms.players[(size_t)my].fighter);
        }
    }
    std::string net = " --net " + quote(server_url()) + " --room " + A.code + "-" + std::to_string(launch_no) + " --slot " +
                      std::to_string(A.lobby.my_slot()) + " --players " + std::to_string(plan.players) + " --ports " +
                      plan.ports + hubui::delay_arg(delay);
    if (e.system == hub::System::N64) {
        std::string args = "--rom " + quote(e.path) + " --game " + (e.catalog_id.empty() ? "unknown" : e.catalog_id) + net + title_arg(e.title);
        if (A.st.fullscreen) args += " --fullscreen";
        args += match_args;
        if (share_save) {   // every PC must agree; only the host (slot 0) supplies its battery dir
            args += " --share-host-save";
            if (A.lobby.my_slot() == 0)
                args += " --host-save-dir " + quote(no_slash(hub::saves::battery_dir(sys_short(e), e.key)));
        }
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
                           " --state-dir " + quote(tmp) + net + title_arg(e.title);
        if (A.st.fullscreen) args += " --fullscreen";
        spawn(hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
        return;
    }
    set_notice("Online play is not available for this system yet.");
}

/* ---------- lobby ---------- */

/* Room accessors: screenshots (HUB_SHOT_FAKE_ROOM) fake a connected room without touching the network. */
static bool g_fake_room = false;
static bool g_fake_other_host = false;   // HUB_SHOT_HOSTCHANGE=other: Mia (slot 0) is the host, we are slot 1
static bool room_connected() { return g_fake_room || A.lobby.connected(); }
static std::vector<hubnet::Member> room_members() {
    if (g_fake_room && g_fake_other_host) return {{0, "Mia"}, {1, A.st.name}, {2, "Jake"}};
    if (g_fake_room) return {{0, A.st.name}, {1, "Mia"}, {2, "Jake"}};
    return A.lobby.members();
}
static int room_my_slot() { return g_fake_room ? (g_fake_other_host ? 1 : 0) : A.lobby.my_slot(); }
static int locals_of(int slot) {
    if (slot == room_my_slot()) return A.my_locals;
    auto it = A.locals.find(slot);
    return it == A.locals.end() ? 1 : std::max(1, std::min(2, it->second));
}
static std::vector<hubui::MemberPads> member_pads() {
    std::vector<hubui::MemberPads> v;
    for (auto& m : room_members()) v.push_back({m.slot, locals_of(m.slot)});
    return v;
}

static std::string member_name(int slot) {
    for (auto& m : room_members()) if (m.slot == slot) return m.name;
    return "Player " + std::to_string(slot + 1);
}

/* ---------- Smash match picker (room state; the panel itself is drawn by match_panel) ---------- */
static bool match_active() { return hubui::match_game_supported(A.selected_game); }
static bool match_remix_roster() { return A.selected_game == "remix"; }   // "ssb64rom" will use the 12-fighter vanilla roster
static std::map<int, hubui::MemberPick> all_picks() {
    std::map<int, hubui::MemberPick> m = A.picks;
    m[room_my_slot()] = A.my_pick;
    return m;
}
static void send_my_pick() { A.lobby.send_msg(hubui::pick_json(A.my_pick)); }
static void send_rules() { A.lobby.send_msg(hubui::rules_json(A.rules)); }

static void leave_room() {
    A.lobby.stop();
    A.in_room = false;
    A.peers.clear();
    A.ready.clear();
    A.last_slots.clear();
    A.chat.clear();
    A.selected_game.clear();
    A.my_ready = false;
    A.locals.clear();
    A.my_locals = 1;
    A.picks.clear();
    A.my_pick = hubui::MemberPick();
    A.rules = hubui::MatchRules();
    A.pick_edit = 0;
    A.share_host_save = true;
    A.was_connected = false;
    A.code.clear();
}

static void enter_room(bool host, const std::string& code) {
    if (A.st.name.empty()) { set_notice("Pick a player name in Settings first."); return; }
    if (g_headless) { set_notice("Headless test run: online skipped."); return; }
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

static bool is_host() { return room_connected() && room_my_slot() == 0; }

static bool allow_version_mismatch() {   // dev override
    const char* e = std::getenv("RETROHUB_ALLOW_VERSION_MISMATCH");
    return e && *e && std::string(e) != "0";
}

static std::vector<hubui::PeerInfo> peer_list() {
    std::vector<hubui::PeerInfo> v;
    int me = room_my_slot();
    for (auto& m : room_members()) {
        hubui::PeerInfo p;
        auto it = A.peers.find(m.slot);
        if (it != A.peers.end()) p = it->second;
        if (m.slot == me || g_fake_room) {
            p.hello = true; p.games.clear(); p.keys.clear(); my_games(&p.games, &p.keys);
            p.app = HUB_VERSION_STR;
            if (g_fake_room && m.slot == 1 && std::getenv("HUB_SHOT_FAKE_APP")) p.app = std::getenv("HUB_SHOT_FAKE_APP");   // screenshot aid: fake version mismatch (e.g. 1.9.1)
        }
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

    /* Lobby events are handled strictly in arrival order. A HostChange moves per-slot state from the promoted
     * member's old slot to 0 and drops the departed host's, so every message queued before it (old numbering)
     * is applied first and nothing from before the change can recreate state under a stale slot. A `launch`
     * that arrived in the same batch before the HostChange came from the departed host: it is dropped (its
     * port plan uses the old slots); the remaining players just stay in the room under the new host. */
    std::vector<hubnet::LobbyEvent> evs;
    {
        hubnet::LobbyEvent e;
        while (A.lobby.poll_event(&e)) evs.push_back(std::move(e));   // poll applies the slot change when it pops a HostChange
    }
    size_t last_hc = evs.size();
    for (size_t k = 0; k < evs.size(); ++k) if (evs[k].type == hubnet::LobbyEvent::HostChange) last_hc = k;
    for (size_t k = 0; k < evs.size(); ++k) {
        const hubnet::LobbyEvent& ev = evs[k];
        if (ev.type == hubnet::LobbyEvent::HostChange) {
            hubui::remap_slot_key(A.peers, ev.from_slot, 0);
            hubui::remap_slot_key(A.ready, ev.from_slot, 0);
            hubui::remap_slot_key(A.locals, ev.from_slot, 0);
            hubui::remap_slot_key(A.picks, ev.from_slot, 0);
            A.hello_dirty = true;
            bool me = A.lobby.my_slot() == 0;
            A.chat.push_back({"", hubui::host_changed_text(ev.name, me)});
            A.chat_scroll = true;
            push_toast(hubui::host_changed_text(ev.name, me), ui::GOOD, 8.0);
            continue;
        }
        int from = ev.from_slot;
        const std::string& body = ev.body;
        bool before_hc = last_hc != evs.size() && k < last_hc;
        hubui::Json j;
        if (!hubui::json_parse(body, &j) || j.type != hubui::Json::Obj) continue;
        if (before_hc && j.str("kind") == "launch") continue;
        std::string kind = j.str("kind");
        if (kind == "hello") {
            hubui::PeerInfo& p = A.peers[from];
            hubui::parse_hello(j, &p);
        } else if (kind == "select") {
            if (from == 0) A.selected_game = j.str("game");
        } else if (kind == "ready") {
            const hubui::Json* r = j.get("ready");
            A.ready[from] = r && r->type == hubui::Json::Bool && r->b;
        } else if (kind == "locals") {
            A.locals[from] = std::max(1, std::min(2, (int)j.num("n", 1)));
        } else if (kind == "opts") {
            const hubui::Json* r = j.get("share_save");
            if (from == 0 && r && r->type == hubui::Json::Bool) A.share_host_save = r->b;
        } else if (kind == "pick") {
            hubui::MemberPick mp;
            if (from != room_my_slot() && hubui::parse_pick(j, &mp)) A.picks[from] = mp;
        } else if (kind == "rules") {
            hubui::MatchRules mr;
            if (from == 0 && !is_host() && hubui::parse_rules(j, &mr)) A.rules = mr;
        } else if (kind == "chat") {
            A.chat.push_back({member_name(from), j.str("text")});
            if (A.chat.size() > 200) A.chat.erase(A.chat.begin());
            A.chat_scroll = true;
        } else if (kind == "launch") {
            if (from != 0 || is_host()) continue;
            const hub::GameEntry* g = find_room_entry(j.str("game"));
            if (!g) { set_notice("The host started a game you don't have."); continue; }
            if (A.proc) continue;
            const hubui::Json* ss = j.get("share_save");
            launch_online(*g, (int)j.num("launch", 1), (int)j.num("delay", 0), false, j.str("core"),
                          ss && ss->type == hubui::Json::Bool && ss->b, nullptr, &j);   // --match comes from j["match"]
        }

    }

    // Membership changes -> forget departed peers, re-announce ourselves.
    std::vector<int> slots;
    for (auto& m : A.lobby.members()) slots.push_back(m.slot);
    std::sort(slots.begin(), slots.end());
    bool changed = !A.was_connected || slots != A.last_slots;
    if (changed) {
        std::set<int> alive(slots.begin(), slots.end());
        for (auto it = A.peers.begin(); it != A.peers.end();) it = alive.count(it->first) ? std::next(it) : A.peers.erase(it);
        for (auto it = A.ready.begin(); it != A.ready.end();) it = alive.count(it->first) ? std::next(it) : A.ready.erase(it);
        for (auto it = A.locals.begin(); it != A.locals.end();) it = alive.count(it->first) ? std::next(it) : A.locals.erase(it);
        for (auto it = A.picks.begin(); it != A.picks.end();) it = alive.count(it->first) ? std::next(it) : A.picks.erase(it);
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
            for (auto& m : hubui::hello_messages("2", games, keys, 3000, HUB_VERSION_STR)) A.lobby.send_msg(m);
        }
        A.lobby.send_msg(std::string("{\"kind\":\"ready\",\"ready\":") + (A.my_ready ? "true" : "false") + "}");
        A.lobby.send_msg("{\"kind\":\"locals\",\"n\":" + std::to_string(A.my_locals) + "}");
        if (is_host()) A.lobby.send_msg(std::string("{\"kind\":\"opts\",\"share_save\":") + (A.share_host_save ? "true" : "false") + "}");
        A.lobby.send_msg(hubui::pick_json(A.my_pick));
        if (is_host()) A.lobby.send_msg(hubui::rules_json(A.rules));
        if (is_host() && !A.selected_game.empty())
            A.lobby.send_msg("{\"kind\":\"select\",\"game\":\"" + hubui::json_escape(A.selected_game) + "\"}");
    }

}

/* ============================ widgets / pages ============================ */

static void page_header(const char* title, const char* sub) {
    ImGui::PushFont(ui::F.title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (sub && *sub) ImGui::TextDisabled("%s", sub);
    ImGui::Dummy(ImVec2(0, sp(1.f)));
}

static void begin_panel(const char* id, ImVec2 size = ImVec2(0, 0), ImU32 bg = COL_PANEL) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleColor(ImGuiCol_Border, ui::HAIRLINE);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, sp(2.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.5f), sp(2.25f)));
    ImGuiChildFlags fl = ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders;
    if (size.y == 0) fl |= ImGuiChildFlags_AutoResizeY;
    ImGui::BeginChild(id, size, fl);
}
static void end_panel() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

static void text_wrapped_disabled(const std::string& s) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::PopStyleColor();
}

static void add_rom_folder_path(const std::string& p) {
    if (std::find(A.st.rom_folders.begin(), A.st.rom_folders.end(), p) == A.st.rom_folders.end()) A.st.rom_folders.push_back(p);
    mark_dirty();
    start_scan();
}
static void add_rom_folder() {
    std::string p = pick_path(A.hwnd, true, nullptr, nullptr);
    if (p.empty()) return;
    add_rom_folder_path(p);
}
static void locate_smash() {
    std::string p = pick_path(A.hwnd, false, L"BattleShip.exe", L"*.exe");
    if (p.empty()) return;
    A.st.smash_exe = p;
    mark_dirty();
    A.hello_dirty = true;
    start_scan();
}

/* ---------- Library: Home (hero + shelves) and All Games ---------- */

static const float kTileAspect = 1.3f;   // tile width / height, shared by shelves, grid and hero art

static void toggle_favorite(const std::string& key) {
    hub::set_favorite(key, !gstate(key).favorite);
    hub::flush_library_state();
    refresh_state(key);
}

static void section_title(const std::string& t) {
    ImGui::PushFont(ui::F.h2);
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

/* ---------- cover art lookups ---------- */

struct ArtRef {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    bool loading = false;   // show a gentle shimmer for a few seconds while the fetch is in flight
};
static ArtRef art_lookup(const std::string& seen_key, bool have_name, SDL_Texture* tex, int w, int h) {
    ArtRef a;
    a.tex = tex;
    a.w = w;
    a.h = h;
    if (!tex && have_name && hubart::enabled()) {
        double now = ImGui::GetTime();
        auto it = A.art_seen.find(seen_key);
        if (it == A.art_seen.end()) it = A.art_seen.emplace(seen_key, now).first;
        a.loading = now - it->second < 10.0;
    }
    return a;
}
static ArtRef art_for_entry(const hub::GameEntry& e) {
    int w = 0, h = 0;
    SDL_Texture* t = hubart::get(e, hubart::Kind::Boxart, &w, &h);
    return art_lookup(e.key, true, t, w, h);
}
static ArtRef art_for_pick(const GameInfo& g) {
    if (g.nointro.empty()) return ArtRef();
    int w = 0, h = 0;
    SDL_Texture* t = hubart::get_by_name(g.system, g.nointro, hubart::Kind::Boxart, &w, &h);
    return art_lookup("pick:" + g.id, true, t, w, h);
}

static std::string initials(const std::string& t) {
    std::string o;
    bool start = true;
    for (unsigned char c : t) {
        if (c == ' ' || c == '-' || c == ':' || c == '.') { start = true; continue; }
        if (start && std::isalnum(c)) {
            o += (char)std::toupper(c);
            if (o.size() >= 3) break;
        }
        start = false;
    }
    return o;
}

/* ---------- tiles ---------- */

static void seed_nav_here();
static bool g_suppress_click = false;   // set by shelf_strip while a drag-pan is in progress / just ended

struct TileSpec {
    std::string id, title, subtitle, badge;
    ImU32 c0 = 0, c1 = 0;
    ArtRef art;
    bool favorite = false, online = false, selected = false, owned = true, show_star = true;
    const char* cta = nullptr;   // hover caption for catalog tiles ("Add ROM")
};
struct TileResult {
    bool visible = false, hovered = false, focused = false, clicked = false, activated = false, star = false, details = false;
};

static void draw_no_art(ImDrawList* dl, const TileSpec& t, ImVec2 p0, ImVec2 p1, float rad, float dim) {
    ImU32 a = t.c0, b = t.c1;
    if (dim < 1.f) { a = mix(ui::PANEL, a, dim); b = mix(ui::PANEL, b, dim); }
    gradient_round_rect(dl, p0, p1, a, b, rad);
    std::string ini = initials(t.title);
    if (!ini.empty()) {
        ImFont* f = ui::F.hero;
        ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, ini.c_str());
        dl->AddText(f, f->FontSize, ImVec2((p0.x + p1.x - ts.x) * 0.5f, p0.y + (p1.y - p0.y) * 0.16f), IM_COL32(255, 255, 255, 52),
                    ini.c_str());
    }
}

static TileResult draw_tile(const TileSpec& t, ImVec2 size) {
    TileResult r;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::PushID(t.id.c_str());
    ImGui::InvisibleButton("tile", size, ImGuiButtonFlags_EnableNav);
    if (t.selected && A.view == LibView::All) seed_nav_here();   // All Games: initial focus = the selected game
    r.visible = ImGui::IsItemVisible();
    bool item_hov = ImGui::IsItemHovered();
    bool focused = ImGui::IsItemFocused();
    r.hovered = item_hov;
    r.focused = focused && io.NavVisible;
    if (!r.visible && !focused && !item_hov) {
        ImGui::PopID();
        return r;
    }
    ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    float sr = sp(1.5f);
    ImVec2 sc(r1.x - sp(2.25f), r0.y + sp(2.25f));
    bool star_hov = item_hov && t.show_star && t.owned && std::fabs(io.MousePos.x - sc.x) < sr + sp(0.75f) && std::fabs(io.MousePos.y - sc.y) < sr + sp(0.75f);
    /* A click fires on release (and not after a drag-pan), so dragging a shelf never selects or plays a tile. */
    if (ImGui::IsItemDeactivated() && item_hov && !g_suppress_click && ImGui::IsMouseReleased(0)) {
        if (star_hov) r.star = true;
        else r.clicked = true;
    }
    if (ImGui::IsItemClicked(1)) { r.clicked = true; r.details = t.owned; }
    if (item_hov && !star_hov && ImGui::IsMouseDoubleClicked(0)) r.activated = true;
    if (focused && A.focused && !io.WantTextInput && !A.proc) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false))
            r.activated = true;
    }

    float* an = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), 0.f);
    ui::anim(*an, (item_hov || r.focused) ? 1.f : 0.f, 14.f);
    float e = *an;
    float lift = e * sp(0.5f), grow = e * sp(0.375f);
    ImVec2 p0(r0.x - grow, r0.y - grow - lift), p1(r1.x + grow, r1.y + grow - lift);
    float rad = sp(1.75f), h = p1.y - p0.y, w = p1.x - p0.x;
    float dim = t.owned ? 1.f : 0.52f + 0.3f * e;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ui::shadow(dl, p0, p1, rad, 0.8f + 0.7f * e, e * sp(0.5f));
    ui::glow(dl, p0, p1, rad, t.c1, e);
    if (t.art.tex) ui::draw_art(dl, t.art.tex, t.art.w, t.art.h, p0, p1, rad, dim);
    else draw_no_art(dl, t, p0, p1, rad, dim);
    if (t.art.loading) ui::shimmer(dl, p0, p1, rad, IM_COL32(0, 0, 0, 0));
    // scrims keep the badge and title readable on any artwork
    ui::vfade_round_rect(dl, ImVec2(p0.x, p0.y + h * 0.30f), p1, IM_COL32(6, 7, 12, 255), rad, ImDrawFlags_RoundCornersBottom, 0.f, 0.96f);
    ui::vfade_round_rect(dl, ImVec2(p0.x, p0.y + h * 0.58f), p1, IM_COL32(6, 7, 12, 255), rad, ImDrawFlags_RoundCornersBottom, 0.f, 0.55f);
    if (t.art.tex) ui::vfade_round_rect(dl, p0, ImVec2(p1.x, p0.y + h * 0.28f), IM_COL32(6, 7, 12, 255), rad, ImDrawFlags_RoundCornersTop, 0.5f, 0.f);
    if (e > 0.01f) dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, (int)(12 * e)), rad);

    float pad = sp(1.5f);
    ui::pill_at(dl, p0 + ImVec2(sp(1.25f), sp(1.25f)), t.badge.c_str(), IM_COL32(0, 0, 0, 150));
    if (t.show_star && t.owned && (t.favorite || e > 0.04f || star_hov)) {
        float sa = t.favorite ? 1.f : std::max(e, star_hov ? 1.f : 0.f);
        dl->AddCircleFilled(sc, sr + sp(0.5f), IM_COL32(0, 0, 0, (int)((star_hov ? 150 : 100) * sa)), 24);
        ui::icon(dl, t.favorite ? ui::Icon::StarFilled : ui::Icon::Star, sc, sr * 1.7f,
                 t.favorite ? IM_COL32(255, 214, 70, 255) : IM_COL32(255, 255, 255, (int)(235 * sa)));
    }

    // title (max 2 lines) + subtitle, anchored to the bottom
    ImFont* tf = ui::F.bold;
    float tfs = tf->FontSize, cap = ui::F.cap->FontSize;
    float maxw = w - pad * 2 - (t.online && t.owned ? sp(2.5f) : 0);
    std::vector<std::string> lines = ui::wrap_lines(tf, tfs, t.title, w - pad * 2, 2);
    float sub_y = p1.y - sp(1.25f) - cap;
    float ty = sub_y - sp(0.375f) - (float)lines.size() * tfs * 1.12f;
    for (size_t i = 0; i < lines.size(); ++i)
        ui::text_shadowed(dl, tf, tfs, ImVec2(p0.x + pad, ty + i * tfs * 1.12f), IM_COL32_WHITE, lines[i].c_str());
    std::string sub = ui::ellipsize(ui::F.cap, cap, t.subtitle, maxw);
    dl->AddText(ui::F.cap, cap, ImVec2(p0.x + pad, sub_y), IM_COL32(255, 255, 255, 190), sub.c_str());
    if (t.online && t.owned) ui::icon(dl, ui::Icon::Globe, ImVec2(p1.x - pad - sp(1), sub_y + cap * 0.5f), sp(2), ui::GOOD);
    if (!t.owned && t.cta && e > 0.02f) {
        const char* c = t.cta;
        ImFont* f = ui::F.bold;
        float cw = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, c).x + sp(5.f);
        ImVec2 cp((p0.x + p1.x - cw) * 0.5f, p0.y + h * 0.40f);
        float ch = f->FontSize + sp(1.25f);
        dl->AddRectFilled(cp, cp + ImVec2(cw, ch), ui::scale_alpha(ui::ACCENT, e), ch * 0.5f);
        ui::icon(dl, ui::Icon::Plus, cp + ImVec2(sp(1.75f), ch * 0.5f), sp(1.75f), IM_COL32(255, 255, 255, (int)(255 * e)));
        dl->AddText(f, f->FontSize, cp + ImVec2(sp(3.f), sp(0.625f)), IM_COL32(255, 255, 255, (int)(255 * e)), c);
    }
    dl->AddRect(p0, p1, ui::HAIRLINE, rad, 0, 1.f);
    if (r.focused) ui::focus_ring(dl, p0, p1, rad);
    else if (t.selected) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 120), rad, 0, sp(0.25f));
    ImGui::PopID();
    return r;
}

static TileSpec spec_for_entry(const hub::GameEntry& e, const char* shelf, bool recent_sub) {
    TileSpec t;
    t.id = std::string(shelf) + "|" + e.key;
    t.title = e.title;
    t.badge = sys_badge(e.system);
    if (is_native(e)) t.badge += " NATIVE";
    if (e.issue == "extract") t.badge += " ZIPPED";
    else if (e.issue == "encrypted") t.badge += " ENCRYPTED";
    else if (e.issue == "keys") t.badge += " NEEDS KEYS";
    entry_colors(e, &t.c0, &t.c1);
    t.art = art_for_entry(e);
    const hub::GameState& gs = gstate(e.key);
    t.favorite = gs.favorite;
    t.selected = A.sel_game == e.key;
    const GameInfo* cat = catalog_for(e);
    t.online = is_native(e) || (cat && cat->online);
    if (recent_sub && gs.last_played) t.subtitle = fmt_ago(gs.last_played);
    else if (gs.play_seconds) t.subtitle = hubui::format_play_time(gs.play_seconds) + " played";
    else t.subtitle = cat && cat->year ? std::to_string(cat->year) : std::string("Not played yet");
    return t;
}

static void handle_entry_result(const hub::GameEntry& e, const TileSpec& spec, const TileResult& r) {
    if (r.focused && A.sel_game != e.key) A.sel_game = e.key;
    bool play = r.activated;
    if (r.clicked) {
        if (spec.selected) play = true;   // second click on the selected game plays it
        A.sel_game = e.key;
    }
    if (r.star) { A.sel_game = e.key; toggle_favorite(e.key); }
    if (r.details) { A.sel_game = e.key; A.show_details = true; }
    if (play) {
        A.sel_game = e.key;
        hub::GameEntry copy = e;
        play_entry(copy);
    }
}

/* ---------- shelves ---------- */

static void shelf_header(const char* title, const std::string& right, float bleed, int count = -1) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* f = ui::F.h2;
    dl->AddText(f, f->FontSize, ImVec2(p.x + bleed, p.y), ui::TEXT, title);
    if (count >= 0) {
        float tw = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, title).x;
        char c[16];
        std::snprintf(c, sizeof c, "%d", count);
        ImFont* cf = ui::F.cap;
        dl->AddText(cf, cf->FontSize, ImVec2(p.x + bleed + tw + sp(1.25f), p.y + f->FontSize - cf->FontSize - sp(0.25f)), ui::TEXT2, c);
    }
    if (!right.empty()) {
        ImVec2 ts = ui::F.cap->CalcTextSizeA(ui::F.cap->FontSize, FLT_MAX, 0.f, right.c_str());
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(p.x + w - bleed - ts.x, p.y + f->FontSize - ts.y - sp(0.25f)), ui::TEXT3, right.c_str());
    }
    ImGui::Dummy(ImVec2(w, f->FontSize + sp(1.25f)));
}

/* Mouse wheel over a shelf row that overflows scrolls THAT row horizontally (called from the event loop before
 * ImGui sees the event, so the page does not scroll too). Returns false when the pointer is not over such a row. */
static bool route_wheel(float px, float py, float wx, float wy) {
    std::vector<hubui::FRect> rects;
    std::vector<ShelfState*> rows;
    int fc = ImGui::GetFrameCount();
    for (auto& kv : A.shelf_state)
        if (kv.second.frame >= fc - 2 && kv.second.max_scroll > 0) {
            rects.push_back(kv.second.rect);
            rows.push_back(&kv.second);
        }
    int i = hubui::rect_hit(rects, px, py);
    if (i < 0) return false;
    ShelfState& s = *rows[i];
    s.target = hubui::shelf_clamp(s.target + hubui::shelf_wheel_delta(wx, wy, s.tw, s.gap), s.max_scroll);
    return true;
}

/* A horizontally scrolling row of tiles. Scrolling is managed here (smooth, time based) instead of by a child
 * window, so every tile stays a real ImGui item and the D-pad can walk onto tiles that are off-screen.
 * Wheel (see route_wheel), arrow buttons, click-and-drag and keyboard/gamepad focus all move the same target. */
static void shelf_strip(const std::string& id, int count, float tw, float th, const std::function<TileResult(int, ImVec2)>& draw_one) {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ShelfState& ss = A.shelf_state[id];
    float gap = sp(2.f), bleed = sp(1.5f);
    float view_w = ImGui::GetContentRegionAvail().x;
    float strip_h = th + sp(4.5f);
    float max_scroll = hubui::shelf_max_scroll(count, tw, gap, bleed, view_w);
    ImVec2 org = ImGui::GetCursorScreenPos();
    ImVec2 v0 = org, v1(org.x + view_w, org.y + strip_h);

    ImVec2 c0(std::max(v0.x, win->InnerClipRect.Min.x), std::max(v0.y, win->InnerClipRect.Min.y));
    ImVec2 c1(std::min(v1.x, win->InnerClipRect.Max.x), std::min(v1.y, win->InnerClipRect.Max.y));
    ss.rect = {c0.x, c0.y, c1.x, c1.y};
    ss.max_scroll = max_scroll;
    ss.tw = tw;
    ss.gap = gap;
    ss.frame = ImGui::GetFrameCount();

    // arrow buttons: always shown while the row overflows
    float bs = sp(4.5f), by = org.y + sp(1.5f) + th * 0.5f - bs * 0.5f;
    ImVec2 la(org.x + sp(0.5f), by), ra(v1.x - bs - sp(0.5f), by);
    auto in_box = [&](ImVec2 p, ImVec2 q) { return io.MousePos.x >= p.x && io.MousePos.x < p.x + q.x && io.MousePos.y >= p.y && io.MousePos.y < p.y + q.y; };
    bool over_arrow = max_scroll > 0 && (in_box(la, ImVec2(bs, bs)) || in_box(ra, ImVec2(bs, bs)));

    bool hov = ImGui::IsMouseHoveringRect(c0, c1) && ImGui::IsWindowHovered();
    // click-and-drag panning (a press that moves past the threshold becomes a drag; a plain click is untouched)
    if (max_scroll > 0) {
        if (hov && !over_arrow && ImGui::IsMouseClicked(0)) {
            ss.pressed = true;
            ss.dragging = false;
            ss.press_x = io.MousePos.x;
            ss.press_y = io.MousePos.y;
            ss.press_scroll = ss.scroll;
        }
        if (ss.pressed && ImGui::IsMouseDown(0)) {
            if (!ss.dragging && hubui::drag_exceeds(io.MousePos.x - ss.press_x, io.MousePos.y - ss.press_y, sp(0.75f))) ss.dragging = true;
            if (ss.dragging) {
                ss.target = hubui::shelf_clamp(ss.press_scroll - (io.MousePos.x - ss.press_x), max_scroll);
                ss.scroll = ss.target;
            }
        }
    }
    ss.target = hubui::shelf_clamp(ss.target, max_scroll);
    if (!ss.dragging) ss.scroll = hubui::approach(ss.scroll, ss.target, io.DeltaTime, 16.f);
    if (std::fabs(ss.scroll - ss.target) < 0.4f) ss.scroll = ss.target;
    g_suppress_click = ss.dragging;

    ImGui::PushClipRect(v0, v1, true);
    bool any_focus = false;
    for (int i = 0; i < count; ++i) {
        float x = org.x + bleed + i * (tw + gap) - ss.scroll;
        ImGui::SetCursorScreenPos(ImVec2(x, org.y + sp(1.5f)));
        TileResult r = draw_one(i, ImVec2(tw, th));
        if (r.focused) {
            any_focus = true;
            if (ss.focus_idx != i) {   // gamepad/keyboard moved onto this tile: scroll it into view
                ss.focus_idx = i;
                float left = bleed + i * (tw + gap);
                ss.target = hubui::shelf_reveal(left, left + tw, ss.target, view_w, sp(5.f), max_scroll);
            }
        }
    }
    if (!any_focus) ss.focus_idx = -1;
    ImGui::PopClipRect();
    g_suppress_click = false;
    if (!io.MouseDown[0]) { ss.pressed = false; ss.dragging = false; }

    // edge fades hint that the row scrolls
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float fw = sp(5.f);
    ImU32 solid = ui::BG, clear = ui::with_alpha(ui::BG, 0.f);
    dl->PushClipRect(v0, v1, true);
    if (ss.scroll > 1.f) dl->AddRectFilledMultiColor(v0, ImVec2(v0.x + fw, v1.y), solid, clear, clear, solid);
    if (ss.scroll < max_scroll - 1.f) dl->AddRectFilledMultiColor(ImVec2(v1.x - fw, v0.y), v1, clear, solid, solid, clear);
    dl->PopClipRect();
    if (max_scroll > 0) {
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        float page = hubui::shelf_page_step(view_w, tw, gap, bleed);
        bool can_l = ss.target > 1.f, can_r = ss.target < max_scroll - 1.f;
        ImGui::SetCursorScreenPos(la);
        ImGui::BeginDisabled(!can_l);
        if (ui::icon_button((id + "<").c_str(), ui::Icon::ChevronL, bs, false, "Scroll left")) ss.target = hubui::shelf_clamp(ss.target - page, max_scroll);
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(ra);
        ImGui::BeginDisabled(!can_r);
        if (ui::icon_button((id + ">").c_str(), ui::Icon::ChevronR, bs, false, "Scroll right")) ss.target = hubui::shelf_clamp(ss.target + page, max_scroll);
        ImGui::EndDisabled();
        ImGui::PopItemFlag();
    }

    ImGui::SetCursorScreenPos(org);
    ImGui::Dummy(ImVec2(view_w, strip_h));
    win->DC.CursorMaxPos.x = std::min(win->DC.CursorMaxPos.x, win->WorkRect.Max.x);   // off-screen tiles must not widen the page
}

static void go_add_rom(const GameInfo& g) {
    A.page = Page::Settings;
    push_toast("Put your " + g.title + " ROM in a ROM folder, then press Rescan.", ui::ACCENT, 7.0);
}

static void home_shelves() {
    float avail = ImGui::GetContentRegionAvail().x;
    float gap = sp(2.f), bleed = sp(1.5f);
    float max_w = std::max(sp(25.f), std::min(sp(37.5f), avail * 0.2f));
    float tw = hubui::shelf_tile_w(avail, sp(24.f), max_w, gap);
    float th = tw / kTileAspect;
    for (size_t si = 0; si < A.shelves.size(); ++si) {
        const hubui::Shelf& sh = A.shelves[si];
        shelf_header(sh.title.c_str(), "", bleed, (int)sh.items.size());
        bool recent = sh.id == "recent";
        shelf_strip("shelf_" + sh.id, (int)sh.items.size(), tw, th, [&](int i, ImVec2 sz) {
            const hub::GameEntry& e = A.lib[sh.items[i]];
            TileSpec spec = spec_for_entry(e, sh.id.c_str(), recent);
            TileResult r = draw_tile(spec, sz);
            handle_entry_result(e, spec, r);
            return r;
        });
        ImGui::Dummy(ImVec2(0, sp(1.f)));
    }
    if (!A.pick_idx.empty()) {
        const std::vector<GameInfo>& cat = hub::catalog();
        shelf_header("Top Picks You Don't Have Yet", "add the ROM to a ROM folder", bleed, (int)A.pick_idx.size());
        shelf_strip("shelf_picks", (int)A.pick_idx.size(), tw, th, [&](int i, ImVec2 sz) {
            const GameInfo& g = cat[A.pick_idx[i]];
            TileSpec t;
            t.id = "pick|" + g.id;
            t.title = g.title;
            hub::System sy = hub::system_from_short(g.system);
            t.badge = sys_badge(sy);
            system_colors(sy, &t.c0, &t.c1);
            t.art = art_for_pick(g);
            t.owned = false;
            t.cta = "Add ROM";
            t.online = g.online;
            t.subtitle = g.year ? std::to_string(g.year) + " - not in your library" : std::string("Not in your library");
            TileResult r = draw_tile(t, sz);
            if (r.clicked || r.activated) go_add_rom(g);
            return r;
        });
        ImGui::Dummy(ImVec2(0, sp(1.f)));
    }
    ImGui::Dummy(ImVec2(0, sp(3.f)));
}

/* ---------- hero ---------- */

/* Gives the item just submitted the (not yet visible) nav focus. ImGui's own first-press behaviour would otherwise
 * land on whatever is closest to the window corner (the search box). Called once per page/view change. */
static void set_nav_here() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (!g.NavWindow) return;
    ImVec2 off = g.NavWindow->DC.CursorStartPos;
    ImRect rel(ImGui::GetItemRectMin() - off, ImGui::GetItemRectMax() - off);
    ImGui::SetNavID(ImGui::GetItemID(), ImGuiNavLayer_Main, g.CurrentFocusScopeId, rel);
}
static void seed_nav_here() {
    if (!A.nav_seed || ImGui::GetIO().WantTextInput) return;
    A.nav_seed = false;
    set_nav_here();
}

static float action_row(const hub::GameEntry& e, float bh, bool draw, ImVec2 at, bool compact, bool seed_nav = false) {
    bool idle = A.proc == nullptr;
    const hub::GameState& gs = gstate(e.key);
    bool gc = e.system == hub::System::GC;
    bool online = supports_online(e) || gc;
    float gap = sp(1.5f), play_w = sp(compact ? 15.f : 18.f), on_w = compact ? bh : sp(21.f);
    float total = play_w;
    if (online) total += gap + on_w;
    total += (gap + bh) * 2;
    if (!draw) return total;
    ImGui::SetCursorScreenPos(at);
    if (button("Play", ImVec2(play_w, bh), true, idle, ui::Icon::Play)) { hub::GameEntry c = e; play_entry(c); }
    if (seed_nav) seed_nav_here();
    if (online) {
        ImGui::SameLine(0, gap);
        if (gc) {
            button(compact ? "##onsoon" : "Online soon", ImVec2(on_w, bh), false, false, ui::Icon::Globe);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Online via Dolphin's own netplay - coming soon");
        } else if (button(compact ? "##online" : "Play Online", ImVec2(on_w, bh), false, idle, ui::Icon::Globe)) {
            A.pending_room_game = room_id(e);
            enter_room(true, hubnet::new_room_code());
        }
        if (compact && ImGui::IsItemHovered()) ImGui::SetTooltip("Play online with friends");
    }
    ImGui::SameLine(0, gap);
    if (ui::icon_button("fav", gs.favorite ? ui::Icon::StarFilled : ui::Icon::Star, bh, gs.favorite, gs.favorite ? "Remove from favorites" : "Add to favorites", IM_COL32(236, 180, 40, 255)))
        toggle_favorite(e.key);
    ImGui::SameLine(0, gap);
    if (ui::icon_button("more", ui::Icon::Dots, bh, false, "Details")) A.show_details = true;
    return total;
}

static void draw_hero(const hub::GameEntry& e, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
    float rad = sp(2.5f), ha = A.hero_t;
    const hub::GameState& gs = gstate(e.key);
    const GameInfo* cat = catalog_for(e);
    ImU32 c0, c1;
    entry_colors(e, &c0, &c1);
    ArtRef art = art_for_entry(e);
    bool compact = h < sp(29.f);

    ui::shadow(dl, p0, p1, rad, 1.3f);
    gradient_round_rect(dl, p0, p1, mix(c0, ui::BG, 0.68f), mix(c1, ui::BG, 0.78f), rad);
    if (art.tex) ui::draw_art_bg(dl, art.tex, art.w, art.h, p0, p1, rad, 0.27f, ha);
    ui::hfade_round_rect(dl, p0, p1, IM_COL32(14, 15, 20, 255), rad, 0, 0.94f, 0.30f);
    ui::vfade_round_rect(dl, ImVec2(p0.x, p0.y + h * 0.5f), p1, IM_COL32(14, 15, 20, 255), rad, ImDrawFlags_RoundCornersBottom, 0.f, 0.55f);
    dl->AddRect(p0, p1, ui::HAIRLINE, rad, 0, 1.f);

    float pad = compact ? sp(2.5f) : sp(3.f), bh = compact ? sp(5.f) : sp(6.f);
    float inner_h = h - pad * 2;
    float art_h = inner_h, art_w = art_h * kTileAspect;
    if (art_w > w * 0.32f) { art_w = w * 0.32f; art_h = art_w / kTileAspect; }
    ImVec2 a0(p0.x + pad, p0.y + pad + (inner_h - art_h) * 0.5f), a1(a0.x + art_w, a0.y + art_h);
    float arad = sp(1.75f);
    ui::shadow(dl, a0, a1, arad, 1.4f);
    TileSpec ts;
    ts.title = e.title;
    ts.c0 = c0;
    ts.c1 = c1;
    if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, arad, 1.f, ha);
    else draw_no_art(dl, ts, a0, a1, arad, 1.f);
    if (art.loading) ui::shimmer(dl, a0, a1, arad, IM_COL32(0, 0, 0, 0));
    dl->AddRect(a0, a1, ui::HAIRLINE, arad, 0, 1.f);

    float tx = a1.x + pad, tw = p1.x - pad - tx, ty = p0.y + pad;
    ImFont* cf = ui::F.cap;
    std::string eyebrow = gs.last_played ? "LAST PLAYED  " + fmt_ago(gs.last_played) : std::string("READY TO PLAY");
    for (auto& ch : eyebrow) ch = (char)std::toupper((unsigned char)ch);
    dl->AddText(cf, cf->FontSize, ImVec2(tx, ty), ui::scale_alpha(mix(c1, IM_COL32_WHITE, 0.45f), ha), eyebrow.c_str());
    ty += cf->FontSize + sp(0.75f);

    ImFont* tf = compact ? ui::F.title : ui::F.hero;
    std::vector<std::string> lines = ui::wrap_lines(tf, tf->FontSize, e.title, tw, compact ? 1 : 2);
    if (lines.size() == 1 && tf == ui::F.hero && ui::F.title != ui::F.hero &&
        tf->CalcTextSizeA(tf->FontSize, FLT_MAX, 0.f, e.title.c_str()).x > tw) {
        tf = ui::F.title;
        lines = ui::wrap_lines(tf, tf->FontSize, e.title, tw, 2);
    }
    for (auto& ln : lines) {
        ui::text_shadowed(dl, tf, tf->FontSize, ImVec2(tx, ty), ui::scale_alpha(IM_COL32_WHITE, ha), ln.c_str());
        ty += tf->FontSize * 1.08f;
    }
    ty += sp(0.5f);

    // meta pills
    {
        float x = tx, ph = cf->FontSize + sp(1.f);
        auto put = [&](const std::string& s, ImU32 bg) {
            float pw = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, s.c_str()).x + sp(2.5f);
            if (x + pw > tx + tw) return;
            pill_at(dl, ImVec2(x, ty), s.c_str(), ui::scale_alpha(bg, ha), ui::scale_alpha(IM_COL32_WHITE, ha));
            x += pw + sp(0.75f);
        };
        put(hub::system_name(e.system), mix(c0, IM_COL32_BLACK, 0.3f));
        if (is_native(e)) put("Native port", IM_COL32(255, 255, 255, 30));
        if (cat && cat->year) put(std::to_string(cat->year), IM_COL32(255, 255, 255, 30));
        if (supports_online(e)) put("1-" + std::to_string(local_players_for(e)) + " players", IM_COL32(255, 255, 255, 30));
        if (gs.play_seconds) put(hubui::format_play_time(gs.play_seconds) + " played", IM_COL32(255, 255, 255, 30));
        ty += ph + sp(1.f);
    }

    float by = p1.y - pad - bh;
    if (!compact && cat && !cat->blurb.empty()) {
        float room = by - ty - sp(1.5f);
        int nl = (int)(room / (ui::F.body->FontSize * 1.25f));
        if (nl > 0) {
            auto bl = ui::wrap_lines(ui::F.body, ui::F.body->FontSize, cat->blurb, tw, std::min(2, nl));
            for (auto& ln : bl) {
                dl->AddText(ui::F.body, ui::F.body->FontSize, ImVec2(tx, ty), ui::scale_alpha(ui::TEXT2, ha), ln.c_str());
                ty += ui::F.body->FontSize * 1.25f;
            }
        }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.3f + 0.7f * ha);
    action_row(e, bh, true, ImVec2(tx, by), compact || tw < sp(70.f), true);
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(w, sp(2.f)));
}

/* ---------- skeleton / empty states ---------- */

static void skeleton_home() {
    float w = ImGui::GetContentRegionAvail().x, h = std::max(sp(24.f), std::min(sp(38.f), ImGui::GetContentRegionAvail().y * 0.4f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ui::shimmer(dl, p0, ImVec2(p0.x + w, p0.y + h), sp(2.5f), ui::PANEL);
    float aw = std::min((h - sp(6.f)) * kTileAspect, w * 0.3f);
    ui::shimmer(dl, ImVec2(p0.x + sp(3.f), p0.y + sp(3.f)), ImVec2(p0.x + sp(3.f) + aw, p0.y + h - sp(3.f)), sp(1.75f), ui::PANEL2);
    float tx = p0.x + sp(6.f) + aw;
    ui::shimmer(dl, ImVec2(tx, p0.y + sp(3.f)), ImVec2(tx + sp(12.f), p0.y + sp(4.f)), sp(0.5f), ui::PANEL2);
    ui::shimmer(dl, ImVec2(tx, p0.y + sp(5.f)), ImVec2(tx + sp(34.f), p0.y + sp(8.f)), sp(1.f), ui::PANEL2);
    ui::shimmer(dl, ImVec2(tx, p0.y + h - sp(9.f)), ImVec2(tx + sp(18.f), p0.y + h - sp(3.f)), sp(1.5f), ui::PANEL3);
    ImGui::Dummy(ImVec2(w, h + sp(2.f)));
    float tw = hubui::shelf_tile_w(w, sp(24.f), std::max(sp(25.f), std::min(sp(37.5f), w * 0.2f)), sp(2.f)), th = tw / kTileAspect;
    for (int row = 0; row < 2; ++row) {
        ImVec2 o = ImGui::GetCursorScreenPos();
        ui::shimmer(dl, o + ImVec2(sp(1.5f), 0), o + ImVec2(sp(1.5f) + sp(18.f), sp(2.5f)), sp(0.75f), ui::PANEL);
        float x = o.x + sp(1.5f), y = o.y + sp(4.f);
        for (int i = 0; x < o.x + w && i < 12; ++i, x += tw + sp(2.f))
            ui::shimmer(dl, ImVec2(x, y), ImVec2(x + tw, y + th), sp(1.75f), ui::PANEL);
        ImGui::Dummy(ImVec2(w, th + sp(7.f)));
    }
}

static void centered_text(ImDrawList* dl, ImFont* f, float cx, float y, ImU32 col, const char* s) {
    ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, s);
    dl->AddText(f, f->FontSize, ImVec2(cx - ts.x * 0.5f, y), col, s);
}

static void empty_library_state() {
    static const std::vector<std::string> suggestions = hub::suggest_rom_folders(3);   // scanned once, on first empty view
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x;
    float h = std::min(ImGui::GetContentRegionAvail().y,
                       sp(46.f) + (suggestions.empty() ? 0.f : sp(10.f) + sp(6.f) * (float)suggestions.size()));
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, ui::PANEL, sp(2.5f));
    dl->AddRect(p0, p1, ui::HAIRLINE, sp(2.5f), 0, 1.f);
    float cx = (p0.x + p1.x) * 0.5f, y = p0.y + sp(3.f);
    float ir = sp(5.f);
    dl->AddCircleFilled(ImVec2(cx, y + ir), ir + sp(1.5f), ui::with_alpha(ui::ACCENT, 0.10f), 48);
    dl->AddCircleFilled(ImVec2(cx, y + ir), ir, ui::with_alpha(ui::ACCENT, 0.22f), 48);
    ui::icon(dl, ui::Icon::Gamepad, ImVec2(cx, y + ir), ir * 1.2f, ui::mix(ui::ACCENT, IM_COL32_WHITE, 0.5f));
    y += ir * 2 + sp(3.f);
    centered_text(dl, ui::F.title, cx, y, ui::TEXT, "Let's find your games");
    y += ui::F.title->FontSize + sp(1.f);
    float tw = std::min(w - sp(6.f), sp(62.f));
    auto lines = ui::wrap_lines(ui::F.body, ui::F.body->FontSize,
                                "RetroHub doesn't include any games. Point it at the folder(s) that hold your own game files and "
                                "everything is recognised by its contents, not its name.",
                                tw, 3);
    for (auto& ln : lines) {
        centered_text(dl, ui::F.body, cx, y, ui::TEXT2, ln.c_str());
        y += ui::F.body->FontSize * 1.3f;
    }
    y += sp(1.5f);
    // supported systems
    {
        struct Sy { const char* label; hub::System s; };
        static const Sy sys[] = {{"N64", hub::System::N64}, {"GameCube", hub::System::GC}, {"GBA", hub::System::GBA}, {"GBC", hub::System::GBC}, {"Game Boy", hub::System::GB},
                                {"Nintendo DS", hub::System::NDS},
                                {"3DS", hub::System::N3DS}, {"Wii U", hub::System::WiiU}};
        float tot = 0;
        for (auto& s2 : sys) tot += ui::F.cap->CalcTextSizeA(ui::F.cap->FontSize, FLT_MAX, 0.f, s2.label).x + sp(2.5f) + sp(0.75f);
        float x = cx - (tot - sp(0.75f)) * 0.5f;
        for (auto& s2 : sys) {
            ImU32 a, b;
            system_colors(s2.s, &a, &b);
            x += pill_at(dl, ImVec2(x, y), s2.label, mix(a, IM_COL32_BLACK, 0.2f)) + sp(0.75f);
        }
        y += ui::F.cap->FontSize + sp(4.f);
    }
    float bw = sp(24.f);
    ImGui::SetCursorScreenPos(ImVec2(cx - bw * 0.5f, y));
    if (button("Add ROM folder", ImVec2(bw, sp(6.f)), true, true, ui::Icon::Folder)) add_rom_folder();
    if (!suggestions.empty()) {
        y += sp(6.f) + sp(3.f);
        centered_text(dl, ui::F.cap, cx, y, ui::TEXT2, "Found game files in these folders:");
        y += ui::F.cap->FontSize + sp(1.5f);
        for (size_t i = 0; i < suggestions.size(); ++i) {
            const std::string& s = suggestions[i];
            // show the last two path parts, e.g. "Desktop\Roms"
            size_t cut = s.find_last_of('\\');
            size_t cut2 = cut == std::string::npos || cut == 0 ? std::string::npos : s.find_last_of('\\', cut - 1);
            std::string label = "Add  " + (cut2 == std::string::npos ? s : s.substr(cut2 + 1));
            ImGui::PushID((int)i);
            ImGui::SetCursorScreenPos(ImVec2(cx - bw * 0.5f, y));
            if (button(label.c_str(), ImVec2(bw, sp(4.5f)), false, true, ui::Icon::Plus)) add_rom_folder_path(s);
            ImGui::PopID();
            y += sp(6.f);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(w, sp(1.f)));
}

/* ---------- top bar ---------- */

static void library_top_bar() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);   // D-pad focus starts on the hero / tiles, not here
    static const char* tabs[] = {"Home", "All Games"};
    int cur = A.view == LibView::Home ? 0 : 1;
    int n = ui::segmented("view", tabs, 2, cur);
    if (n != cur) {
        A.view = n == 0 ? LibView::Home : LibView::All;
        if (n == 0) A.search_buf[0] = 0;
    }
    ImGui::SameLine(0, sp(2.f));

    float bh = sp(5.f);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) A.focus_search = true;
    if (A.focus_search) { ImGui::SetKeyboardFocusHere(); A.focus_search = false; }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(sp(5.f), (bh - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bh * 0.5f);
    ImGui::SetNextItemWidth(std::max(sp(20.f), std::min(sp(46.f), ImGui::GetContentRegionAvail().x * 0.45f)));
    ImGui::InputTextWithHint("##search", "Search games   (Ctrl+F)", A.search_buf, sizeof A.search_buf);
    ImVec2 s0 = ImGui::GetItemRectMin(), s1 = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(s0, s1, ImGui::IsItemActive() ? ui::ACCENT : ui::HAIRLINE, bh * 0.5f, 0, 1.f);
    ui::icon(ImGui::GetWindowDrawList(), ui::Icon::Search, ImVec2(s0.x + sp(2.5f), (s0.y + s1.y) * 0.5f), sp(2.f), ui::TEXT2);
    ImGui::PopStyleVar(2);
    if (A.search_buf[0] && A.view == LibView::Home) A.view = LibView::All;

    float total = bh * 2 + sp(1.f);
    ImGui::SameLine(0, 0);
    float ax = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ax - total));
    ImGui::BeginDisabled(A.scanning);
    if (ui::icon_button("rescan", ui::Icon::Refresh, bh, false, A.scanning ? "Scanning..." : "Rescan ROM folders")) start_scan();
    ImGui::EndDisabled();
    ImGui::SameLine(0, sp(1.f));
    if (ui::icon_button("addfolder", ui::Icon::Plus, bh, false, "Add ROM folder")) add_rom_folder();
    ImGui::PopItemFlag();

    // scan progress: thin animated line under the bar
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 o = ImGui::GetCursorScreenPos();
    float lh = sp(0.25f);
    if (A.scanning) {
        int d = A.scan_done, t2 = A.scan_total;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(o.x, o.y + sp(0.5f)), ImVec2(o.x + w, o.y + sp(0.5f) + lh), ui::PANEL2, lh);
        if (t2 > 0) dl->AddRectFilled(ImVec2(o.x, o.y + sp(0.5f)), ImVec2(o.x + w * d / (float)t2, o.y + sp(0.5f) + lh), ui::ACCENT, lh);
        else {
            float ph = std::fmod((float)ImGui::GetTime() * 0.8f, 1.f);
            dl->AddRectFilled(ImVec2(o.x + w * ph * 0.8f, o.y + sp(0.5f)), ImVec2(o.x + w * (ph * 0.8f + 0.2f), o.y + sp(0.5f) + lh), ui::ACCENT, lh);
        }
        char buf[64];
        std::snprintf(buf, sizeof buf, t2 > 0 ? "Scanning your games...  %d / %d" : "Scanning your games...", d, t2);
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(o.x + sp(0.25f), o.y + sp(1.25f)), ui::TEXT3, buf);
    }
    ImGui::Dummy(ImVec2(w, sp(A.scanning ? 3.25f : 1.5f)));
}

/* ---------- All Games ---------- */

static void selected_bar(const hub::GameEntry& e) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x, h = sp(8.f);
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, ui::PANEL, sp(2.f));
    dl->AddRect(p0, p1, ui::HAIRLINE, sp(2.f), 0, 1.f);
    ImU32 c0, c1;
    entry_colors(e, &c0, &c1);
    ArtRef art = art_for_entry(e);
    float th = h - sp(2.f), tw = th * kTileAspect;
    ImVec2 a0(p0.x + sp(1.f), p0.y + sp(1.f)), a1(a0.x + tw, a0.y + th);
    TileSpec ts;
    ts.title = e.title;
    ts.c0 = c0;
    ts.c1 = c1;
    if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, sp(1.25f));
    else draw_no_art(dl, ts, a0, a1, sp(1.25f), 1.f);
    float bh = sp(5.f);
    float row_w = action_row(e, bh, false, ImVec2(0, 0), true);
    float tx = a1.x + sp(2.f);
    float text_w = std::max(sp(8.f), p1.x - sp(2.f) - row_w - tx - sp(2.f));
    const hub::GameState& gs = gstate(e.key);
    dl->AddText(ui::F.h2, ui::F.h2->FontSize, ImVec2(tx, p0.y + sp(1.25f)), ui::TEXT, ui::ellipsize(ui::F.h2, ui::F.h2->FontSize, e.title, text_w).c_str());
    std::string sub = std::string(hub::system_name(e.system)) + "  -  " + hubui::format_play_time(gs.play_seconds);
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p0.y + sp(1.25f) + ui::F.h2->FontSize + sp(0.5f)), ui::TEXT2,
                ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, sub, text_w).c_str());
    action_row(e, bh, true, ImVec2(p1.x - sp(2.f) - row_w, p0.y + (h - bh) * 0.5f), true);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(w, 1));
}

static void all_games_view() {
    // system chips
    struct Chip { const char* label; const char* sys; };
    static const Chip chips[] = {{"All", ""}, {"N64", "n64"}, {"GameCube", "gc"}, {"GBA", "gba"}, {"GBC", "gbc"}, {"Game Boy", "gb"},
                                {"Nintendo DS", "nds"},
                                {"3DS", "3ds"}, {"Wii U", "wiiu"}};
    std::map<std::string, int> counts;
    for (auto& e : A.lib) ++counts[sys_short(e)];
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp(1.f), sp(1.f)));
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
    bool first = true;
    for (auto& c : chips) {
        int n = c.sys[0] ? counts[c.sys] : (int)A.lib.size();
        if (c.sys[0] && n == 0 && A.sys_filter != c.sys) continue;
        if (!first) ImGui::SameLine();
        first = false;
        char lab[48];
        std::snprintf(lab, sizeof lab, "%s  %d", c.label, n);
        ImU32 a = COL_ACCENT, b2;
        if (c.sys[0]) system_colors(hub::system_from_short(c.sys), &a, &b2);
        if (chip(lab, A.sys_filter == c.sys, a)) { A.sys_filter = c.sys; A.vis_dirty = true; }
    }
    {   // Online filter chip + group-by toggle
        int n_online = 0;
        for (auto& e : A.lib) {
            const GameInfo* c = catalog_for(e);
            if (is_native(e) || (c && c->online)) ++n_online;
        }
        char lab[48];
        std::snprintf(lab, sizeof lab, "Online  %d", n_online);
        ImGui::SameLine(0, sp(2.f));
        if (chip(lab, A.online_only, ui::GOOD)) { A.online_only = !A.online_only; A.vis_dirty = true; }
    }
    ImGui::PopItemFlag();
    ImGui::PopStyleVar();
    {
        static const char* modes[] = {"Genre", "System", "A-Z"};
        float need = ImGui::CalcTextSize("Group by").x + sp(5.f) + sp(1.f);
        for (const char* m : modes) need += ImGui::CalcTextSize(m).x + sp(4.f);
        float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (ImGui::GetItemRectMax().x + sp(3.f) + need <= right) ImGui::SameLine(0, sp(3.f));   // else wrap under the chips
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::TEXT3));
        ImGui::TextUnformatted("Group by");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, sp(1.f));
        ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
        A.group_mode = ui::segmented("groupby", modes, 3, A.group_mode);
        ImGui::PopItemFlag();
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (A.vis_dirty || A.vis_filter != A.sys_filter || A.vis_query != A.search_buf) rebuild_vis();

    const hub::GameEntry* sel = find_entry(A.sel_game);
    float bar_h = sel ? sp(8.f) + ImGui::GetStyle().ItemSpacing.y + 2.f : 0;   // selected bar + the gap above it
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.5f), sp(1.5f)));
    ImGui::BeginChild("grid", ImVec2(0, -bar_h), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    A.page_scrollable = true;
    if (A.vis.empty()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetCursorScreenPos();
        float cx = o.x + ImGui::GetContentRegionAvail().x * 0.5f;
        ui::icon(dl, ui::Icon::Search, ImVec2(cx, o.y + sp(6.f)), sp(5.f), ui::TEXT3);
        std::string msg = A.search_buf[0] ? std::string("No games match \"") + A.search_buf + "\"" : std::string("No games in this system yet");
        centered_text(dl, ui::F.h2, cx, o.y + sp(10.f), ui::TEXT, msg.c_str());
        centered_text(dl, ui::F.body, cx, o.y + sp(10.f) + ui::F.h2->FontSize + sp(1.f), ui::TEXT2, "Try a different spelling, or pick another system.");
        ImGui::Dummy(ImVec2(0, sp(17.f)));
        if (A.search_buf[0]) {
            float bw = sp(18.f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw) * 0.5f);
            if (button("Clear search", ImVec2(bw, sp(5.f)), false, true, ui::Icon::Close)) A.search_buf[0] = 0;
        }
    } else {
        // sections according to the group-by mode (A.vis is already sorted: pinned first, then by title)
        struct Section { std::string title; std::vector<int> items; };
        std::vector<Section> secs;
        auto sec_for = [&](const std::string& t) -> Section& {
            for (auto& s : secs) if (s.title == t) return s;
            secs.push_back({t, {}});
            return secs.back();
        };
        if (A.group_mode == 0) {
            for (const std::string& gn : hubui::genre_order()) sec_for(gn);
            sec_for("Other");
            for (int i : A.vis) {
                const GameInfo* c = catalog_for(A.lib[i]);
                sec_for(hubui::effective_genre(c ? c->genre : std::string())).items.push_back(i);
            }
        } else if (A.group_mode == 1) {
            static const char* order[] = {"n64", "gc", "gba", "gbc", "gb", "nds", "3ds", "wiiu"};
            for (const char* s : order) sec_for(hubui::system_shelf_title(s));
            for (int i : A.vis) sec_for(hubui::system_shelf_title(sys_short(A.lib[i]))).items.push_back(i);
        } else {
            for (int i : A.vis) {
                unsigned char ch = A.lib[i].title.empty() ? '#' : (unsigned char)A.lib[i].title[0];
                std::string t(1, std::isalpha(ch) ? (char)std::toupper(ch) : '#');
                sec_for(t).items.push_back(i);
            }
            std::stable_sort(secs.begin(), secs.end(), [](const Section& x, const Section& y) {
                if ((x.title == "#") != (y.title == "#")) return x.title == "#";
                return x.title < y.title;
            });
        }
        float gap = sp(2.f);
        float avail = ImGui::GetContentRegionAvail().x;
        hubui::GridLayout gl = hubui::grid_layout(avail, sp(26.f), sp(37.5f), gap);
        ImVec2 sz(gl.tile_w, gl.tile_w / kTileAspect);
        for (auto& sec : secs) {
            if (sec.items.empty()) continue;
            shelf_header(sec.title.c_str(), "", 0.f, (int)sec.items.size());
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
            for (size_t k = 0; k < sec.items.size(); ++k) {
                if (k % gl.cols) ImGui::SameLine();
                const hub::GameEntry& e = A.lib[sec.items[k]];
                TileSpec spec = spec_for_entry(e, "grid", false);
                TileResult r = draw_tile(spec, sz);
                handle_entry_result(e, spec, r);
            }
            ImGui::PopStyleVar();
            ImGui::Dummy(ImVec2(0, sp(1.5f)));
        }
        ImGui::Dummy(ImVec2(0, sp(2.f)));
    }
    ImGui::EndChild();
    if (sel) selected_bar(*sel);
}

/* ---------- details modal (Y / right-click / "...") ---------- */

static void draw_details_modal() {
    if (A.show_details) {
        A.show_details = false;
        if (find_entry(A.sel_game)) ImGui::OpenPopup("##details");
    }
    const hub::GameEntry* ep = find_entry(A.sel_game);
    ImGuiIO& io = ImGui::GetIO();
    float w = std::min(sp(90.f), io.DisplaySize.x * 0.92f);
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal("##details", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!open) return;
    if (!ep) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    const hub::GameEntry& e = *ep;
    const hub::GameState& gs = gstate(e.key);
    const GameInfo* cat = catalog_for(e);
    if (A.info_key != e.key || ImGui::GetTime() - A.info_at > 4.0) refresh_save_info(e);
    bool idle = A.proc == nullptr, native = is_native(e);
    ImU32 c0, c1;
    entry_colors(e, &c0, &c1);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    bool with_art = avail > sp(62.f);
    float aw = with_art ? sp(28.f) : 0, ah = aw / kTileAspect;
    if (with_art) {
        ArtRef art = art_for_entry(e);
        TileSpec ts;
        ts.title = e.title;
        ts.c0 = c0;
        ts.c1 = c1;
        ImVec2 a0 = o, a1(o.x + aw, o.y + ah);
        ui::shadow(dl, a0, a1, sp(1.75f), 1.2f);
        if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, sp(1.75f));
        else draw_no_art(dl, ts, a0, a1, sp(1.75f), 1.f);
        dl->AddRect(a0, a1, ui::HAIRLINE, sp(1.75f), 0, 1.f);
    }
    float tx = with_art ? aw + sp(3.f) : 0;
    ImGui::SetCursorScreenPos(ImVec2(o.x + tx, o.y));
    ImGui::BeginGroup();
    float tw = avail - tx;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
    ImGui::PushFont(ui::F.title);
    ImGui::TextWrapped("%s", e.title.c_str());
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    pill(hub::system_name(e.system), mix(c0, IM_COL32_BLACK, 0.2f));
    ImGui::SameLine(0, sp(0.75f));
    pill(native ? "NATIVE PORT" : "EMULATED", ui::PANEL2);
    if (supports_online(e)) {
        char pl[32];
        std::snprintf(pl, sizeof pl, "1-%d players", local_players_for(e));
        ImGui::SameLine(0, sp(0.75f));
        pill(pl, ui::PANEL2);
    }
    if (cat && cat->year) {
        ImGui::SameLine(0, sp(0.75f));
        pill(std::to_string(cat->year).c_str(), ui::PANEL2);
    }
    if (cat && !cat->blurb.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        ImGui::TextWrapped("%s", cat->blurb.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
    ImGui::EndGroup();
    if (with_art) ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + std::max(ah, ImGui::GetItemRectSize().y) + sp(2.f)));
    ImGui::Dummy(ImVec2(0, sp(1.f)));

    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextWrapped("%s", e.path.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    if (e.issue == "extract") {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("Zipped: Azahar cannot open .zip files. Press Play to unpack it (about %.1f GB) into RetroHub's data folder - your zip stays untouched.", unpacked_size(e) / 1073741824.0);
        ImGui::PopStyleColor();
    } else if (e.issue == "encrypted") {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("This 3DS image is encrypted. Azahar needs your own AES keys (aes_keys.txt in its user\\sysdata folder) to run it.");
        ImGui::PopStyleColor();
    } else if (e.issue == "keys") {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("Disc images (.wud/.wux) need your disc key in Cemu's keys.txt. A .wua archive needs no keys.");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::Text("Play time: %s      Last played: %s", hubui::format_play_time(gs.play_seconds).c_str(),
                gs.last_played ? fmt_time(gs.last_played).c_str() : "never");
    if (native) {
        ImGui::TextDisabled("Smash Classic keeps its own saves.");
    } else {
        std::string sv = A.info_battery_files > 0 ? "Save data: " + std::to_string(A.info_battery_files) + " file(s), " + size_text(A.info_battery_bytes)
                                                  : std::string("Save data: none yet");
        sv += "      Save states: " + std::to_string(A.info_state_files);
        if (A.info_auto_state) sv += "  (+ resume point)";
        ImGui::TextUnformatted(sv.c_str());
    }
    ImGui::Dummy(ImVec2(0, sp(2.f)));

    float bh = sp(5.5f);
    if (button("Play", ImVec2(sp(17.f), bh), true, idle, ui::Icon::Play)) { ImGui::CloseCurrentPopup(); hub::GameEntry c = e; play_entry(c); }
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine(0, sp(1.5f));
    if (e.system == hub::System::GC) {
        button("Online soon", ImVec2(0, bh), false, false, ui::Icon::Globe);
    } else if (supports_online(e)) {
        if (button("Play Online", ImVec2(0, bh), false, idle, ui::Icon::Globe)) {
            ImGui::CloseCurrentPopup();
            A.pending_room_game = room_id(e);
            enter_room(true, hubnet::new_room_code());
        }
    }
    ImGui::SameLine(0, sp(1.5f));
    if (button(gs.favorite ? "Unfavorite" : "Favorite", ImVec2(0, bh), false, true, gs.favorite ? ui::Icon::StarFilled : ui::Icon::Star)) toggle_favorite(e.key);
    if (!native) {
        ImGui::SameLine(0, sp(1.5f));
        if (button("Save folder", ImVec2(0, bh), false, true, ui::Icon::Folder)) open_in_explorer(hub::saves::game_dir(sys_short(e), e.key));
    } else {
        ImGui::SameLine(0, sp(1.5f));
        if (button("Locate BattleShip.exe", ImVec2(0, bh))) locate_smash();
    }
    ImGui::SameLine(0, sp(1.5f));
    if (button("Close", ImVec2(0, bh), false, true, ui::Icon::Close)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

/* ---------- the page ---------- */

static void ensure_selection() {
    if (A.lib.empty()) { A.sel_game.clear(); return; }
    if (!A.env_select.empty() && find_entry(A.env_select)) { A.sel_game = A.env_select; A.env_select.clear(); }
    if (find_entry(A.sel_game)) return;
    int i = hubui::pick_hero(A.sg, "");
    A.sel_game = i >= 0 ? A.lib[i].key : std::string();
}

/* Home card: shown until a sync folder is set or the player dismisses it ("Not now", saved in hub.json). */
static void sync_card() {
    if (!A.st.sync_folder.empty() || A.st.sync_card_dismissed) return;
    const auto& sugg = sync_suggestions();
    bool busy = A.job.running;

    begin_panel("sync_card");
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Keep your saves and friends on all your PCs");
    ImGui::PopFont();
    text_wrapped_disabled("Sync your save vault through a folder both PCs can see. RetroHub never deletes a save.");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (!sugg.empty() && button(("Use " + sugg[0].label).c_str(), ImVec2(0, 0), true, !busy)) {
        make_dirs(sugg[0].path);
        A.st.sync_folder = sugg[0].path;
        A.st.sync_auto = true;
        mark_dirty();
        start_sync(true);
    }
    if (!sugg.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sugg[0].path.c_str());
    ImGui::SameLine(0, sp(1.f));
    if (button("Choose folder...", ImVec2(0, 0), false, !busy)) {
        std::string p = pick_path(A.hwnd, true, nullptr, nullptr);
        if (!p.empty()) {
            A.st.sync_folder = p;
            A.st.sync_auto = true;
            mark_dirty();
            start_sync(true);
        }
    }
    ImGui::SameLine(0, sp(1.f));
    if (button("Not now", ImVec2(0, 0))) {
        A.st.sync_card_dismissed = true;
        mark_dirty();
    }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
}

static void page_library() {
    if (A.vis_dirty || A.vis_filter != A.sys_filter || A.vis_query != A.search_buf) rebuild_vis();
    ensure_selection();
    library_top_bar();
    A.page_scrollable = false;

    if (A.lib.empty()) {
        if (A.scanning && !A.scanned_once) skeleton_home();
        else if (A.scanning) {}   // rescan of an empty library: top bar shows the progress line
        else empty_library_state();
        return;
    }
    if (A.view == LibView::All) {
        all_games_view();
        return;
    }
    if (A.st.rom_folders.empty() && !A.scanning) {   // only the bundled game: ask for game folders
        begin_panel("add_folders_card");
        ImGui::PushFont(ui::F.bold);
        ImGui::TextUnformatted("Add your game folders");
        ImGui::PopFont();
        text_wrapped_disabled("Only the bundled game is here. Point RetroHub at the folder(s) with your N64, GBA, GB, DS, 3DS and Wii U games and they all show up.");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (button("Add ROM folder", ImVec2(0, 0), true, true, ui::Icon::Folder)) add_rom_folder();
        static const std::vector<std::string> sugg2 = hub::suggest_rom_folders(2);
        for (const std::string& s : sugg2) {
            ImGui::SameLine(0, sp(1.f));
            size_t cut = s.find_last_of('\\');
            size_t cut2 = cut == std::string::npos || cut == 0 ? std::string::npos : s.find_last_of('\\', cut - 1);
            if (button(("Add  " + (cut2 == std::string::npos ? s : s.substr(cut2 + 1))).c_str(), ImVec2(0, 0))) add_rom_folder_path(s);
        }
        end_panel();
        ImGui::Dummy(ImVec2(0, sp(1.f)));
    }
    if (!A.st.rom_folders.empty()) sync_card();

    const hub::GameEntry* sel = find_entry(A.sel_game);
    if (sel) {
        if (A.hero_key != sel->key) { A.hero_key = sel->key; A.hero_t = 0.f; }
        ui::anim(A.hero_t, 1.f, 9.f);
        float avail_h = ImGui::GetContentRegionAvail().y;
        float hero_h = std::max(sp(23.5f), std::min(sp(40.f), avail_h * 0.38f));
        draw_hero(*sel, hero_h);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("shelves", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp(2.f), 0));
    A.page_scrollable = ImGui::GetScrollMaxY() > 0;
    home_shelves();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

/* ---------- Online ---------- */

/* ---------- Smash match picker UI ---------- */
static const ImU32 COL_REMIX = IM_COL32(250, 150, 40, 255);

static bool match_tile(const char* id, const std::string& label, ImVec2 sz, bool sel, ImU32 tint) {
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("t", sz, ImGuiButtonFlags_EnableNav);
    bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    float rd = 8 * S, pad = 8 * S;
    dl->AddRectFilled(p0, p1, sel ? with_alpha(tint, 0.35f) : (hov ? COL_PANEL2 : COL_PANEL), rd);
    dl->AddRect(p0, p1, sel ? tint : (hov ? IM_COL32(255, 255, 255, 60) : ui::HAIRLINE), rd, 0, sel ? 2 * S : 1.f);
    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    dl->PushClipRect(p0 + ImVec2(pad * 0.5f, 0), p1 - ImVec2(pad * 0.5f, 0), true);
    dl->AddText(ImVec2(p0.x + std::max(pad, (sz.x - ts.x) * 0.5f), p0.y + (sz.y - ts.y) * 0.5f), sel ? IM_COL32_WHITE : ui::TEXT, label.c_str());
    dl->PopClipRect();
    return clicked;
}

/* Cards in remix mode: "Fighter  <pill>  costume N" in place of the owned-games row. */
static void member_pick_row(int slot) {
    hubui::MemberPick none;
    const hubui::MemberPick* mp = &none;
    if (slot == room_my_slot()) mp = &A.my_pick;
    else { auto it = A.picks.find(slot); if (it != A.picks.end()) mp = &it->second; }
    int n = locals_of(slot);
    std::string lab = hubui::pick_label(*mp, n);
    bool rnd = mp->p[0].fighter < 0 && (n < 2 || mp->p[1].fighter < 0);
    ImU32 col = rnd ? IM_COL32(70, 72, 86, 255) : (mp->p[0].fighter > 0x0B ? mix(COL_REMIX, IM_COL32_BLACK, 0.25f) : mix(COL_ACCENT, IM_COL32_BLACK, 0.15f));
    ImGui::TextDisabled("Fighter");
    ImGui::SameLine(0, 8 * S);
    pill(lab.c_str(), col);
    if (n < 2 && mp->p[0].fighter >= 0) {
        ImGui::SameLine(0, 8 * S);
        ImGui::TextDisabled("costume %d", mp->p[0].costume + 1);
    }
}

static void match_panel(bool host, float width) {
    (void)width;
    const bool remix = match_remix_roster();
    auto fighters = hubui::fighter_choices(remix);
    begin_panel("matchpanel");
    section_title(remix ? "Smash Remix match" : "Match");
    ImGui::Dummy(ImVec2(0, 2 * S));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = 8 * S;

    // --- my fighter(s)
    int locals = std::max(1, std::min(2, A.my_locals));
    if (A.pick_edit >= locals) A.pick_edit = 0;
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Your fighter");
    ImGui::PopFont();
    if (locals == 2) {   // the 2nd local player picks their own
        for (int k = 0; k < 2; ++k) {
            ImGui::SameLine(0, k ? gap : 18 * S);
            std::string lab = "Player " + std::to_string(k + 1) + ": " + hubui::fighter_name(A.my_pick.p[k].fighter);
            if (match_tile(("pe" + std::to_string(k)).c_str(), lab, ImVec2(170 * S, 28 * S), A.pick_edit == k, COL_ACCENT)) A.pick_edit = k;
        }
    }
    hubui::MatchPick& cur = A.my_pick.p[A.pick_edit];
    {   // costume row
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Costume");
        int nc = cur.fighter >= 0 ? hubui::fighter_costumes(cur.fighter) : 0;
        if (nc == 0) {
            ImGui::SameLine(0, gap);
            ImGui::TextDisabled("random");
        }
        for (int c = 0; c < nc; ++c) {
            ImGui::SameLine(0, c ? 4 * S : gap);
            if (match_tile(("co" + std::to_string(c)).c_str(), std::to_string(c + 1), ImVec2(34 * S, 28 * S), cur.costume == c, COL_ACCENT)) {
                cur.costume = c;
                send_my_pick();
            }
        }
    }
    ImGui::Dummy(ImVec2(0, 2 * S));
    {   // fighter grid: Random, then Vanilla / Remix groups
        const float min_w = 112 * S;
        int cols = std::max(1, (int)((avail + gap) / (min_w + gap)));
        float tw = (avail - gap * (cols - 1)) / cols;
        ImVec2 tsz(tw, 30 * S);
        auto choose = [&](int id) {
            cur.fighter = id;
            cur.costume = 0;
            send_my_pick();
        };
        if (match_tile("fr", "Random", ImVec2(tw, 30 * S), cur.fighter < 0, ui::TEXT2)) choose(-1);
        for (int grp = 0; grp < 2; ++grp) {
            bool any = false;
            for (auto& f : fighters) any = any || f.remix == (grp == 1);
            if (!any) continue;
            ImGui::Dummy(ImVec2(0, 2 * S));
            ImGui::TextDisabled("%s", grp ? "REMIX" : "VANILLA");
            int i = 0;
            for (auto& f : fighters) {
                if (f.remix != (grp == 1)) continue;
                if (i % cols) ImGui::SameLine(0, gap);
                if (match_tile(("f" + std::to_string(f.id)).c_str(), f.name, tsz, cur.fighter == f.id, grp ? COL_REMIX : COL_ACCENT)) choose(f.id);
                ++i;
            }
        }
    }

    // --- stage + stocks (host decides; everyone sees)
    ImGui::Dummy(ImVec2(0, 6 * S));
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Stage");
    ImGui::PopFont();
    ImGui::SameLine(0, 14 * S);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_ACCENT), "%s", hubui::stage_name(A.rules.stage).c_str());
    if (host) {
        auto stages = hubui::stage_choices(remix);
        const float min_w = 150 * S;
        int cols = std::max(1, (int)((avail - 14 * S + gap) / (min_w + gap)));
        float tw = (avail - 14 * S - gap * (cols - 1)) / cols;
        ImGui::BeginChild("stagelist", ImVec2(0, 210 * S), ImGuiChildFlags_None);
        auto choose = [&](int id) {
            A.rules.stage = id;
            send_rules();
        };
        if (match_tile("sr", "Random", ImVec2(tw, 30 * S), A.rules.stage < 0, ui::TEXT2)) choose(-1);
        for (int grp = 0; grp < 2; ++grp) {
            bool any = false;
            for (auto& s : stages) any = any || s.remix == (grp == 1);
            if (!any) continue;
            ImGui::Dummy(ImVec2(0, 2 * S));
            ImGui::TextDisabled("%s", grp ? "REMIX" : "VANILLA");
            int i = 0;
            for (auto& s : stages) {
                if (s.remix != (grp == 1)) continue;
                if (i % cols) ImGui::SameLine(0, gap);
                if (match_tile(("s" + std::to_string(s.id)).c_str(), s.name, ImVec2(tw, 30 * S), A.rules.stage == s.id, grp ? COL_REMIX : COL_ACCENT)) choose(s.id);
                ++i;
            }
        }
        ImGui::EndChild();
    }
    ImGui::Dummy(ImVec2(0, 4 * S));
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Stocks");
    ImGui::PopFont();
    if (host) {
        for (int k = 1; k <= 5; ++k) {
            ImGui::SameLine(0, k == 1 ? 14 * S : 4 * S);
            if (match_tile(("st" + std::to_string(k)).c_str(), std::to_string(k), ImVec2(40 * S, 28 * S), A.rules.stocks == k, COL_ACCENT)) {
                A.rules.stocks = k;
                send_rules();
            }
        }
    } else {
        ImGui::SameLine(0, 14 * S);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_ACCENT), "%d", A.rules.stocks);
    }
    ImGui::Dummy(ImVec2(0, 2 * S));
    text_wrapped_disabled(host ? "Anyone still on Random, and a Random stage, is rolled for everyone when you press Launch."
                               : "The host picks the stage and stocks. Anyone on Random is rolled at launch.");
    end_panel();
    ImGui::Dummy(ImVec2(0, 4 * S));
}

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
    dl->AddText(ui::F.h2, ui::F.h2->FontSize, cc - ImVec2(its.x * 0.6f, its.y * 0.62f), IM_COL32(20, 20, 25, 255), init);
    dl->AddText(ui::F.h2, ui::F.h2->FontSize, ImVec2(p0.x + 74 * S, p0.y + 14 * S), IM_COL32_WHITE, p->name.c_str());
    ImGui::SetCursorScreenPos(p0 + ImVec2(74 * S, 46 * S));
    bool host = slot == 0;
    bool rd = host || A.ready[slot];
    if (slot == A.lobby.my_slot()) rd = host || A.my_ready;
    if (host) pill("HOST", COL_ACCENT);
    else pill(rd ? "Ready" : "Not ready", rd ? COL_GOOD : IM_COL32(70, 72, 86, 255));
    if (p->hello) {   // RetroHub version, bottom-right of the card; red when it differs from ours
        std::string theirs = p->app.empty() ? "version unknown" : hubui::app_version_label(p->app);
        bool differs = p->app.empty() || hubui::compare_app_versions(p->app, HUB_VERSION_STR) != 0;
        ImVec2 ts = ImGui::CalcTextSize(theirs.c_str());
        dl->AddText(ImVec2(p1.x - ts.x - 14 * S, p1.y - ts.y - 10 * S), differs ? COL_BAD : ui::TEXT2, theirs.c_str());
    }
    {   // local players (1-2 controllers on this PC)
        int n = locals_of(slot);
        bool mine = slot == room_my_slot();
        ImGui::SetCursorScreenPos(ImVec2(p1.x - 150 * S, p0.y + 12 * S));
        ImGui::PushID(slot);
        ImGui::TextDisabled("Local players");
        ImGui::SetCursorScreenPos(ImVec2(p1.x - 150 * S, p0.y + 36 * S));
        if (mine) {
            if (ImGui::RadioButton("1", n == 1)) { A.my_locals = 1; A.hello_dirty = true; }
            ImGui::SameLine();
            if (ImGui::RadioButton("2", n == 2)) { A.my_locals = 2; A.hello_dirty = true; }
        } else {
            pill(n == 2 ? "2 players" : "1 player", IM_COL32(70, 72, 86, 255));
        }
        ImGui::PopID();
    }
    // owned games
    ImGui::SetCursorScreenPos(p0 + ImVec2(16 * S, 88 * S));
    if (match_active()) {
        member_pick_row(slot);
    } else if (!p->hello) {
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

#include "friends_ui.inc"

static void page_online_home() {
    page_header("Play Online", "Host a room and share the code, or join a friend's room.");
    float avail = ImGui::GetContentRegionAvail().x;
    float gap = 20 * S;
    bool two = avail > 760 * S;
    float w = two ? (avail - gap) / 2 : avail;

    begin_panel("host", ImVec2(w, 0));
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Host a room");
    ImGui::PopFont();
    text_wrapped_disabled("Create a room, pick a game everyone owns, and share the 6-letter code.");
    ImGui::Dummy(ImVec2(0, 8 * S));
    if (button("Host Room", ImVec2(-1, sp(7.f)), true, A.proc == nullptr, ui::Icon::Globe)) enter_room(true, hubnet::new_room_code());
    end_panel();

    if (two) ImGui::SameLine();

    begin_panel("join", ImVec2(w, 0));
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Join a room");
    ImGui::PopFont();
    text_wrapped_disabled("Type the code your friend gave you.");
    ImGui::Dummy(ImVec2(0, 8 * S));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%s", A.join_buf.c_str());
    ImGui::PushFont(ui::F.h2);
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

    ImGui::Dummy(ImVec2(0, sp(1.f)));
    begin_panel("howto");
    section_title("How it works");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    int online_games = 0;
    for (auto& e : A.lib) if (supports_online(e)) ++online_games;
    static const char* steps[] = {"Host a room (or join with a code) - everyone in the room sees each other's games.",
                                  "The host picks a game that everybody owns, then everyone presses Ready.",
                                  "The host presses Launch - the game starts on every PC at the same moment."};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < 3; ++i) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = sp(1.75f);
        dl->AddCircleFilled(ImVec2(p.x + r, p.y + r + sp(0.25f)), r, with_alpha(ui::ACCENT, 0.25f), 24);
        char num[4] = {(char)('1' + i), 0};
        ImVec2 ts = ui::F.bold->CalcTextSizeA(ui::F.bold->FontSize, FLT_MAX, 0.f, num);
        dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(p.x + r - ts.x * 0.5f, p.y + r + sp(0.25f) - ts.y * 0.5f), ui::TEXT, num);
        ImGui::SetCursorScreenPos(ImVec2(p.x + r * 2 + sp(1.5f), p.y));
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted(steps[i]);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
    ImGui::Text("You have %d game%s that can be played online.", online_games, online_games == 1 ? "" : "s");
    ImGui::PopStyleColor();
    end_panel();
}

static void page_online_room() {
    // Header: big code + actions
    ImGui::PushFont(ui::F.big);
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
    ImGui::SameLine();
    fr_invite_button_and_picker(170 * S, 44 * S);
    ImGui::EndGroup();

    if (g_fake_room && A.selected_game.empty() && !A.lib.empty()) {
        for (auto& e : A.lib)
            if (supports_online(e)) { A.selected_game = room_id(e); if (e.title.find("Kart") != std::string::npos) break; }
    }
    if (!g_fake_room && A.lobby.failed()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_BAD), "%s", A.lobby.status().c_str());
        return;
    }
    if (!room_connected()) {
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
    float card_w = left_w - (match_active() ? ImGui::GetStyle().ScrollbarSize + 2 * S : 0.f);   // the match picker makes this column scroll
    float mw = (card_w - gap * (cols - 1)) / cols;
    std::map<int, const hubui::PeerInfo*> by_slot;
    for (auto& p : peers) by_slot[p.slot] = &p;
    for (int s = 0; s < 4; ++s) {
        if (s % cols) ImGui::SameLine(0, gap);
        member_card(by_slot.count(s) ? by_slot[s] : nullptr, s, ImVec2(mw, 124 * S));
    }

    ImGui::Dummy(ImVec2(0, 4 * S));
    if (!host && !allow_version_mismatch()) {
        std::string host_app;
        bool host_hello = false;
        for (auto& p : peers) if (p.slot == 0) { host_app = p.app; host_hello = p.hello; }
        auto act = hubui::guest_version_action(HUB_VERSION_STR, host_app, host_hello);
        if (act == hubui::GuestVersionAction::UpdateNow) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_BAD), "The host is on %s and you have %s - you need to update to play.",
                               hubui::app_version_label(host_app).c_str(), hubui::app_version_label(HUB_VERSION_STR).c_str());
            if (hubupdate::downloading()) {
                float pr = hubupdate::download_progress();
                ImGui::ProgressBar(pr < 0 ? 0.f : pr, ImVec2(260 * S, 0));
                text_wrapped_disabled(hubupdate::status().c_str());
            } else {
                if (button("Update now", ImVec2(200 * S, 40 * S), true, !hubupdate::checking())) {
                    hubupdate::check_async(true);
                    A.room_update_pending = true;
                    A.room_update_none = false;
                }
                if (A.room_update_pending && !hubupdate::checking()) {
                    A.room_update_pending = false;
                    if (hubupdate::update_available()) hubupdate::start_update();
                    else if (!hubupdate::check_failed()) A.room_update_none = true;
                }
                if (hubupdate::check_failed() && !hubupdate::status().empty()) text_wrapped_disabled(hubupdate::status().c_str());
                else if (A.room_update_none) text_wrapped_disabled("No newer release is published yet. Ask the host which version they're on.");
            }
        } else if (act == hubui::GuestVersionAction::Waiting) {
            text_wrapped_disabled("Checking versions...");
        } else if (act == hubui::GuestVersionAction::HostNeedsUpdate) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_BAD), "Host needs to update (host is on %s, you have %s).",
                               host_app.empty() ? "an old version" : hubui::app_version_label(host_app).c_str(),
                               hubui::app_version_label(HUB_VERSION_STR).c_str());
        }
    }
    if (!host) {
        bool before = A.my_ready;
        if (ImGui::Checkbox("I'm ready", &A.my_ready) && before != A.my_ready) A.hello_dirty = true;
    }
    ImGui::PushFont(ui::F.h2);
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
            bool clicked = ImGui::InvisibleButton("row", sz, ImGuiButtonFlags_EnableNav);
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
    if (rg && rg->system == hub::System::N64) {
        if (host) {
            if (ImGui::Checkbox("Use host's save (unlocked characters/boards)", &A.share_host_save)) A.hello_dirty = true;
        } else {
            ImGui::TextDisabled("%s", A.share_host_save ? "Using the host's save (unlocked characters/boards)." : "Starting from an empty save.");
        }
    }
    if (match_active()) match_panel(host, left_w);
    if (host) {
        bool ok = false;
        std::string why;
        if (!rg) why = "Choose a game to start";
        else {
            auto s = hubui::game_selectable(A.selected_game, max_players_for(*rg), peers, 0);
            ok = s.ok;
            why = s.reason;
            if (ok) {
                std::string host_app;
                for (auto& p : peers) if (p.slot == 0) host_app = p.app;
                auto vg = hubui::version_gate(host_app, peers, allow_version_mismatch());
                if (!vg.ok) { ok = false; why = vg.reason; }
            }
            if (ok)
                for (auto& p : peers)
                    if (p.slot != 0 && !A.ready[p.slot]) { ok = false; why = p.name + " isn't ready yet"; break; }
        }
        if (A.proc) { ok = false; why = "In game..."; }
        if (button("Launch", ImVec2(220 * S, 60 * S), true, ok)) {
            int delay = A.st.delay_auto ? 0 : A.st.input_delay;   // 0 = auto, same for everyone
            bool share = A.share_host_save && rg->system == hub::System::N64;
            unsigned seed = std::random_device{}();
            ++A.launch_counter;
            hubui::PortPlan plan = hubui::build_ports(member_pads());   // built once; guests use it verbatim
            hubui::MatchSpec spec;   // Randoms are rolled here, once, from the launch seed
            bool with_match = rg->system == hub::System::N64 && match_active();
            if (with_match) spec = hubui::resolve_match(plan.ports, all_picks(), A.rules, seed, match_remix_roster());
            A.lobby.send_msg(hubui::launch_json(A.selected_game, A.launch_counter, delay, seed, A.st.gb_core, share, &plan, with_match ? &spec : nullptr));
            launch_online(*rg, A.launch_counter, delay, true, A.st.gb_core, share, &plan, nullptr, with_match ? &spec : nullptr);
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
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Chat");
    ImGui::PopFont();
    float input_h = ImGui::GetFrameHeight() + 8 * S;
    ImGui::BeginChild("chatlog", ImVec2(0, -input_h), ImGuiChildFlags_None);
    for (auto& l : A.chat) {
        if (l.who.empty()) { text_wrapped_disabled(l.text); continue; }   // system line (e.g. host change)
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
        begin_panel("nopad", ImVec2(0, sp(30.f)));
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 o = ImGui::GetWindowPos();
            float w = ImGui::GetWindowWidth(), cx = o.x + w * 0.5f, y = o.y + sp(4.f);
            dl->AddCircleFilled(ImVec2(cx, y + sp(4.f)), sp(5.5f), with_alpha(ui::ACCENT, 0.10f), 48);
            dl->AddCircleFilled(ImVec2(cx, y + sp(4.f)), sp(4.f), with_alpha(ui::ACCENT, 0.22f), 48);
            ui::icon(dl, ui::Icon::Gamepad, ImVec2(cx, y + sp(4.f)), sp(5.f), mix(ui::ACCENT, IM_COL32_WHITE, 0.5f));
            centered_text(dl, ui::F.h2, cx, y + sp(10.5f), ui::TEXT, "No controller yet");
            auto lines = ui::wrap_lines(ui::F.body, ui::F.body->FontSize,
                                        "Plug in or pair a controller and it shows up here right away. The first one becomes port 1, the second port 2, and so on. "
                                        "You can also use the D-pad and A / B to move around RetroHub itself.",
                                        std::min(w - sp(6.f), sp(60.f)), 3);
            float ty = y + sp(10.5f) + ui::F.h2->FontSize + sp(1.f);
            for (auto& ln : lines) {
                centered_text(dl, ui::F.body, cx, ty, ui::TEXT2, ln.c_str());
                ty += ui::F.body->FontSize * 1.3f;
            }
        }
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
    ImGui::PushFont(ui::F.h2);
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
    text_wrapped_disabled("Pick a folder both PCs can see (OneDrive, Dropbox, Google Drive, a USB stick or a NAS). RetroHub keeps "
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
    bool can_sync = !A.st.sync_folder.empty() && !busy && !A.proc;
    if (button(busy && A.job.kind == SaveJob::Sync ? "Syncing..." : "Sync now", ImVec2(180 * S, 46 * S), true, can_sync)) start_sync(true);
    if (A.proc && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A game is running. Saves sync automatically when it closes.");
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
        std::wcsftime(nm, 64, L"RetroHub-saves-%Y%m%d.zip", &lt);
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

static void settings_label(const char* title, const char* hint = nullptr) {
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (hint) text_wrapped_disabled(hint);
}

static void page_settings() {
    page_header("Settings", "Make RetroHub yours.");

    begin_panel("set_player");
    section_title("Player");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    settings_label("Player name", "Shown to friends in online rooms.");
    char nb[40];
    std::snprintf(nb, sizeof nb, "%s", A.st.name.c_str());
    ImGui::SetNextItemWidth(sp(40.f));
    if (ImGui::InputText("##name", nb, 25)) { A.st.name = nb; mark_dirty(); }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    begin_panel("set_lib");
    section_title("Library");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    settings_label("ROM folders", "RetroHub scans these folders (and everything below them) for your game files.");
    if (A.st.rom_folders.empty()) text_wrapped_disabled("(none yet)");
    int remove_at = -1;
    for (size_t i = 0; i < A.st.rom_folders.size(); ++i) {
        ImGui::PushID((int)i);
        if (ui::icon_button("rm", ui::Icon::Close, sp(4.f), false, "Remove this folder")) remove_at = (int)i;
        ImGui::SameLine(0, sp(1.5f));
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("%s", A.st.rom_folders[i].c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopID();
    }
    if (remove_at >= 0) {
        A.st.rom_folders.erase(A.st.rom_folders.begin() + remove_at);
        mark_dirty();
        start_scan();
    }
    if (button("Add folder...", ImVec2(0, 0), false, true, ui::Icon::Plus)) add_rom_folder();
    ImGui::SameLine();
    if (button("Rescan", ImVec2(0, 0), false, !A.scanning, ui::Icon::Refresh)) start_scan();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (ImGui::Checkbox("Download cover art", &A.st.download_art)) {
        hubart::set_enabled(A.st.download_art);
        mark_dirty();
    }
    text_wrapped_disabled("Fetches box art for your games (and the games you could add) from the libretro-thumbnails project, "
                          "then keeps it on this PC. Turn this off to stay fully offline; the colourful tiles are used instead.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    settings_label("Smash Classic location");
    std::string se = smash_exe_path();
    text_wrapped_disabled(se.empty() ? "BattleShip.exe not found" : se);
    if (button("Browse for BattleShip.exe...", ImVec2(0, 0), false, true, ui::Icon::Folder)) locate_smash();
    if (!A.st.smash_exe.empty()) {
        ImGui::SameLine();
        if (button("Use default", ImVec2(0, 0))) { A.st.smash_exe.clear(); mark_dirty(); A.hello_dirty = true; start_scan(); }
    }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    begin_panel("set_play");
    section_title("Playing");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    settings_label("Game Boy / Game Boy Color emulator");
    int core = A.st.gb_core == "gambatte" ? 1 : 0;
    if (ImGui::RadioButton("SameBoy (most accurate)", core == 0)) { A.st.gb_core = "sameboy"; mark_dirty(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Gambatte (lighter)", core == 1)) { A.st.gb_core = "gambatte"; mark_dirty(); }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (ImGui::Checkbox("Start games fullscreen", &A.st.fullscreen)) mark_dirty();
    text_wrapped_disabled("Applies to Game Boy, GameCube, 3DS and Wii U games. N64 games: press Alt+Enter.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    settings_label("3DS and Wii U emulators");
    {
        std::string wiiu_game;
        for (auto& g : A.lib) if (g.system == hub::System::WiiU) { wiiu_game = g.path; break; }
        static hubemu::Install az, cm;   // the lookups touch the disk: refresh every 2 s, not every frame
        static double looked_at = -100;
        static std::string looked_cemu;
        if (ImGui::GetTime() - looked_at > 2.0 || looked_cemu != A.st.cemu_exe) {
            az = hubemu::find(hubemu::Kind::Azahar, "", "");
            cm = hubemu::find(hubemu::Kind::Cemu, A.st.cemu_exe, wiiu_game);
            looked_at = ImGui::GetTime();
            looked_cemu = A.st.cemu_exe;
        }
        text_wrapped_disabled("Azahar (3DS): " + (az.exe.empty() ? std::string("not installed - downloads the first time you play a 3DS game (about 42 MB)") : az.exe));
        text_wrapped_disabled("Cemu (Wii U): " + (cm.exe.empty() ? std::string("not found - downloads the first time you play a Wii U game (about 25 MB)") : cm.exe + "  (" + cm.how + ")"));
        if (button("Choose Cemu.exe...", ImVec2(0, 0), false, true, ui::Icon::Folder)) {
            std::string p = pick_path(A.hwnd, false, L"Cemu.exe", L"*.exe");
            if (!p.empty()) { A.st.cemu_exe = p; mark_dirty(); }
        }
        if (!A.st.cemu_exe.empty()) {
            ImGui::SameLine();
            if (button("Use automatic", ImVec2(0, 0))) { A.st.cemu_exe.clear(); mark_dirty(); }
        }
        text_wrapped_disabled("A Cemu you already have is used as it is: RetroHub never changes its settings or moves its saves; it only copies "
                              "the game's save folder into the save vault after you play (and restores it into an empty one).");
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    settings_label("Online input delay");
    ImGui::SetNextItemWidth(sp(40.f));
    if (ImGui::RadioButton("Auto (recommended)", A.st.delay_auto)) { A.st.delay_auto = true; mark_dirty(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Manual", !A.st.delay_auto)) { A.st.delay_auto = false; mark_dirty(); }
    if (A.st.delay_auto) {
        text_wrapped_disabled("The game measures your connection and picks the delay for everyone. The host's choice applies to the whole room.");
    } else {
        if (ImGui::SliderInt("##delay", &A.st.input_delay, 1, 8, "%d frames")) mark_dirty();
        text_wrapped_disabled("About " + std::to_string(A.st.input_delay * 1000 / 60) + " ms at 60 fps. Higher is smoother on slow connections but feels laggier.");
    }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    begin_panel("set_friends");
    section_title("Friends");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (ImGui::Checkbox("Show friends panel", &A.st.show_friends)) mark_dirty();
    if (ImGui::Checkbox("Appear offline", &A.st.appear_offline)) mark_dirty();
    text_wrapped_disabled("Friends see you as offline, but you can still use the friends list.");
    if (ImGui::Checkbox("Notification sound", &A.st.notif_sound)) mark_dirty();
    std::string mycode = fr_my_code();
    if (!mycode.empty()) {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        settings_label("Your friend code", "Share it so friends can add you.");
        ImGui::TextUnformatted(mycode.c_str());
        ImGui::SameLine();
        if (button("Copy", ImVec2(0, 0))) { SDL_SetClipboardText(mycode.c_str()); set_toast("Friend code copied.", true, 3.0); }
    }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    begin_panel("set_app");
    section_title("Appearance");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    settings_label("Sidebar");
    int sm = A.st.sidebar_mode;
    if (ImGui::RadioButton("Automatic", sm == 0)) { A.st.sidebar_mode = 0; mark_dirty(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Always expanded", sm == 1)) { A.st.sidebar_mode = 1; mark_dirty(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Icons only", sm == 2)) { A.st.sidebar_mode = 2; mark_dirty(); }
    text_wrapped_disabled("Automatic shrinks the sidebar to icons when the window is narrow.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (ImGui::CollapsingHeader("Advanced")) {
        char sb[256];
        std::snprintf(sb, sizeof sb, "%s", A.st.server.c_str());
        ImGui::TextUnformatted("Lobby server URL");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##srv", hubnet::default_server(), sb, sizeof sb)) { A.st.server = sb; mark_dirty(); }
        ImGui::TextDisabled("Leave empty for the default server.");
    }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(2.f)));
}

/* ---------- 3DS / Wii U emulator download + unzip dialogs ---------- */

static void poll_emulators() {
    bool success = false;
    if (EMU.poll(&success)) {
        if (success) {
            set_toast(std::string(hubemu::name(EMU.kind)) + " installed.");
            if (const hub::GameEntry* g = find_entry(EMU.game_key)) { hub::GameEntry copy = *g; play_entry(copy); }
        }
    }
    if (UNZ.state == UnzipJob::Working && UNZ.done) {
        if (UNZ.th.joinable()) UNZ.th.join();
        if (UNZ.ok) {
            UNZ.state = UnzipJob::Idle;
            set_toast("Unpacked. Starting the game...");
            if (const hub::GameEntry* g = find_entry(UNZ.key)) { hub::GameEntry copy = *g; play_entry(copy); }
        } else if (UNZ.error == "cancelled") {
            UNZ.state = UnzipJob::Idle;
        } else {
            UNZ.state = UnzipJob::Failed;
        }
    }
}

static void emu_modal_begin(const char* name, bool* open_flag) {
    ImGui::OpenPopup(name);
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(66.f), ImGui::GetIO().DisplaySize.x * 0.92f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    *open_flag = ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(2);
}

static void draw_emulator_dialogs() {
    if (EMU.state != hubemu::Installer::Idle) {
        const hubemu::Release& rel = hubemu::release(EMU.kind);
        std::string nm = hubemu::name(EMU.kind);
        bool open = false;
        emu_modal_begin("Emulator download", &open);
        if (open) {
            if (EMU.state == hubemu::Installer::Asking) {
                ImGui::TextWrapped("%s games need the %s emulator (about %d MB download from its official GitHub release, %s). "
                                   "It is kept inside RetroHub's data folder. Download now?",
                                   EMU.kind == hubemu::Kind::Azahar ? "Nintendo 3DS" : "Wii U", nm.c_str(), (int)(rel.size / 1048576), rel.license);
                ImGui::Dummy(ImVec2(0, 8 * S));
                if (button("Download", ImVec2(160 * S, 44 * S), true)) EMU.start(EMU.kind, hubemu::managed_root());
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (button("Cancel", ImVec2(160 * S, 44 * S))) EMU.state = hubemu::Installer::Idle;
            } else if (EMU.state == hubemu::Installer::Working) {
                std::string phases[] = {"Downloading " + nm + "...", "Checking the download...", "Unpacking...", "Installing..."};
                ImGui::TextUnformatted(phases[std::min(3, (int)EMU.phase)].c_str());
                uint64_t got = EMU.got, tot = EMU.total;
                char ov[48];
                std::snprintf(ov, sizeof ov, "%.1f / %.1f MB", got / 1048576.0, tot / 1048576.0);
                ImGui::ProgressBar(EMU.phase > 0 ? 1.f : (tot ? (float)got / (float)tot : 0.f), ImVec2(-1, 24 * S), EMU.phase > 0 ? "" : ov);
                ImGui::TextDisabled("Your game starts as soon as it is ready.");
            } else {
                std::string err;
                { std::lock_guard<std::mutex> lk(EMU.mu); err = EMU.error; }
                ImGui::TextWrapped("Could not install %s: %s", nm.c_str(), err.c_str());
                ImGui::Dummy(ImVec2(0, 8 * S));
                if (button("Close", ImVec2(160 * S, 44 * S), true)) EMU.state = hubemu::Installer::Idle;
                ImGui::SetItemDefaultFocus();
            }
            if (EMU.state == hubemu::Installer::Idle) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    if (UNZ.state != UnzipJob::Idle) {
        bool open = false;
        emu_modal_begin("Unpack 3DS game", &open);
        if (open) {
            const hub::GameEntry* g = find_entry(UNZ.key);
            if (UNZ.state == UnzipJob::Asking && g) {
                ImGui::TextWrapped("Azahar cannot open .zip files. Unpack \"%s\" (about %.1f GB) into RetroHub's data folder?", g->title.c_str(),
                                   unpacked_size(*g) / 1073741824.0);
                ImGui::Dummy(ImVec2(0, 2 * S));
                text_wrapped_disabled("The unpacked image goes to " + hub::extract_3ds_dir(*g) + " and needs the same amount of free disk space. "
                                      "Your zip is left untouched. You can also unpack it yourself into a ROM folder.");
                ImGui::Dummy(ImVec2(0, 8 * S));
                if (button("Unpack and play", ImVec2(200 * S, 44 * S), true)) start_unzip(*g);
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (button("Cancel", ImVec2(160 * S, 44 * S))) UNZ.state = UnzipJob::Idle;
            } else if (UNZ.state == UnzipJob::Working) {
                ImGui::TextUnformatted("Unpacking...");
                uint64_t got = UNZ.got, tot = UNZ.total;
                char ov[48];
                std::snprintf(ov, sizeof ov, "%.2f / %.2f GB", got / 1073741824.0, tot / 1073741824.0);
                ImGui::ProgressBar(tot ? (float)((double)got / (double)tot) : 0.f, ImVec2(-1, 24 * S), ov);
                ImGui::Dummy(ImVec2(0, 6 * S));
                if (button("Cancel", ImVec2(160 * S, 44 * S))) UNZ.cancel = true;
            } else {
                std::string err;
                { std::lock_guard<std::mutex> lk(UNZ.mu); err = UNZ.error; }
                ImGui::TextWrapped("Could not unpack the game: %s", err.c_str());
                ImGui::Dummy(ImVec2(0, 8 * S));
                if (button("Close", ImVec2(160 * S, 44 * S), true)) UNZ.state = UnzipJob::Idle;
                ImGui::SetItemDefaultFocus();
            }
            if (UNZ.state == UnzipJob::Idle) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
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
        ImGui::SetNextWindowSize(ImVec2(std::min(sp(66.f), ImGui::GetIO().DisplaySize.x * 0.92f), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
        bool dolphin_open = ImGui::BeginPopupModal("Dolphin emulator", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::PopStyleVar(2);
        if (dolphin_open) {
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
    draw_emulator_dialogs();
    draw_details_modal();
}

static void page_about() {
    page_header("About", "");
    begin_panel("about");
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("RetroHub");
    ImGui::PopFont();
    ImGui::TextDisabled("Version %s", HUB_VERSION_STR);
    ImGui::Dummy(ImVec2(0, 6 * S));
    ImGui::TextWrapped("One library for N64, GameCube, Wii U, Nintendo 3DS, Game Boy Advance, Game Boy and Game Boy Color: one-click resume, "
                       "room codes, online play, and saves that follow you between PCs.");
    ImGui::Dummy(ImVec2(0, 6 * S));
    text_wrapped_disabled("Smash Classic runs as a native port with rollback netplay. N64 games run in the built-in "
                          "mupen64plus runner and Game Boy games in the built-in libretro runner, both with lockstep input netplay. "
                          "GameCube games use Dolphin, Nintendo 3DS games use Azahar and Wii U games use Cemu; those emulators are downloaded "
                          "the first time you need them (or an existing Cemu is used). You supply your own game files; none are included.");
    ImGui::Dummy(ImVec2(0, 6 * S));
    ImGui::Dummy(ImVec2(0, 10 * S));
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Updates");
    ImGui::PopFont();
    if (hubupdate::downloading()) {
        float p = hubupdate::download_progress();
        ImGui::ProgressBar(p < 0 ? 0.f : p, ImVec2(sp(40.f), 0), p < 0 ? "Downloading..." : nullptr);
        text_wrapped_disabled(hubupdate::status().c_str());
    } else {
        ImGui::BeginDisabled(hubupdate::checking());
        if (ImGui::Button(hubupdate::checking() ? "Checking..." : "Check for updates")) {
            A.update_prompt_dismissed = false;
            A.update_check_requested = true;
            hubupdate::check_async(true);
        }
        ImGui::EndDisabled();
        if (hubupdate::update_available()) {
            ImGui::SameLine();
            std::string lbl = "Update to " + hubupdate::latest_version();
            if (ImGui::Button(lbl.c_str())) hubupdate::start_update();
        }
        std::string st = hubupdate::status();
        if (!st.empty()) text_wrapped_disabled(st.c_str());
        else if (A.update_check_requested && !hubupdate::checking() && !hubupdate::update_available() && !hubupdate::check_failed())
            text_wrapped_disabled("You're on the latest version.");
    }
    text_wrapped_disabled("RetroHub checks for updates each time it starts. Updating downloads the new version, "
                          "installs it over this folder (your saves and settings are kept) and reopens RetroHub.");
    end_panel();
}

/* Startup "new version available" prompt. Shown once per run when the launch
 * check finds a newer release that the player hasn't chosen to skip (a manual
 * "Check for updates" shows it even for a skipped version). */
static void draw_update_popup() {
    bool want = hubupdate::update_available() && !A.update_prompt_dismissed &&
                (A.update_check_requested || !hubupdate::latest_skipped());
    if (want && !ImGui::IsPopupOpen("Update available")) ImGui::OpenPopup("Update available");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(sp(52.f), 0), ImVec2(sp(80.f), vp->Size.y * 0.8f));
    if (!ImGui::BeginPopupModal("Update available", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
        return;
    ImGui::PushFont(ui::F.h2);
    ImGui::Text("RetroHub %s is available", hubupdate::latest_version().c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("You have %s. Do you want to update now?", hubupdate::current_version().c_str());
    std::string notes = hubupdate::release_notes();
    if (!notes.empty()) {
        ImGui::Dummy(ImVec2(0, 4 * S));
        ImGui::BeginChild("notes", ImVec2(sp(70.f), sp(18.f)), ImGuiChildFlags_Border);
        ImGui::TextWrapped("%s", notes.c_str());
        ImGui::EndChild();
    }
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (hubupdate::downloading()) {
        float p = hubupdate::download_progress();
        ImGui::ProgressBar(p < 0 ? 0.f : p, ImVec2(-1, 0), p < 0 ? "Downloading..." : nullptr);
        ImGui::TextDisabled("%s", hubupdate::status().c_str());
        ImGui::TextDisabled("RetroHub will close, install the update and reopen by itself.");
    } else {
        if (!hubupdate::status().empty() && hubupdate::check_failed())
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.45f, 1.f), "%s", hubupdate::status().c_str());
        if (ImGui::Button("Update now", ImVec2(sp(18.f), 0))) hubupdate::start_update();
        ImGui::SameLine();
        if (ImGui::Button("Later", ImVec2(sp(12.f), 0))) {
            A.update_prompt_dismissed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip this version", ImVec2(sp(18.f), 0))) {
            hubupdate::skip_latest();
            A.update_prompt_dismissed = true;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

/* ---------- toasts ---------- */

static void draw_toasts(float bottom, float right) {
    double now = ImGui::GetTime();
    A.toasts.erase(std::remove_if(A.toasts.begin(), A.toasts.end(), [&](const Toast& t) { return t.until < now; }), A.toasts.end());
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImFont* f = ui::F.body;
    float fs = f->FontSize, y = bottom;
    for (int i = (int)A.toasts.size() - 1; i >= 0; --i) {
        const Toast& t = A.toasts[i];
        float in = (float)std::min(1.0, (now - t.t0) / 0.28), out = (float)std::min(1.0, (t.until - now) / 0.35);
        in = 1.f - (1.f - in) * (1.f - in);   // ease-out
        float a = std::max(0.f, std::min(1.f, in * out));
        float maxw = std::min(sp(58.f), ds.x * 0.62f);
        std::vector<std::string> lines = ui::wrap_lines(f, fs, t.text, maxw - sp(6.f), 4);
        float tw = 0;
        for (auto& l : lines) tw = std::max(tw, f->CalcTextSizeA(fs, FLT_MAX, 0.f, l.c_str()).x);
        float w = tw + sp(6.f), h = lines.size() * fs * 1.25f + sp(2.5f);
        float x1 = right + (1.f - in) * sp(5.f);
        ImVec2 p1(x1, y), p0(x1 - w, y - h);
        ui::shadow(dl, p0, p1, sp(1.5f), a);
        dl->AddRectFilled(p0, p1, ui::scale_alpha(ui::PANEL2, a), sp(1.5f));
        dl->AddRect(p0, p1, ui::scale_alpha(ui::HAIRLINE, a), sp(1.5f), 0, 1.f);
        dl->AddRectFilled(ImVec2(p0.x + sp(1.f), p0.y + sp(1.f)), ImVec2(p0.x + sp(1.f) + sp(0.5f), p1.y - sp(1.f)), ui::scale_alpha(t.col, a), sp(0.25f));
        for (size_t k = 0; k < lines.size(); ++k)
            dl->AddText(f, fs, ImVec2(p0.x + sp(3.f), p0.y + sp(1.25f) + k * fs * 1.25f), ui::scale_alpha(ui::TEXT, a), lines[k].c_str());
        y -= h + sp(1.f);
    }
}

/* ---------- sidebar ---------- */

static bool nav_item(Page pg, const char* label, ui::Icon ic, float w, float expand, ImU32 badge) {
    ImGui::PushID((int)pg);
    bool pressed = ImGui::InvisibleButton("nav", ImVec2(w, sp(6.f)), ImGuiButtonFlags_EnableNav);
    A.side_ids.push_back(ImGui::GetItemID());
    if (A.seed_side && A.page == pg) { A.seed_side = false; set_nav_here(); }
    bool hov = ImGui::IsItemHovered();
    bool focus = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    bool sel = A.page == pg;
    float* an = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), 0.f);
    ui::anim(*an, sel ? 1.f : ((hov || focus) ? 0.5f : 0.f), 14.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    float rd = sp(1.5f);
    dl->AddRectFilled(p0, p1, with_alpha(sel ? ui::ACCENT : IM_COL32_WHITE, sel ? 0.20f : 0.07f * (*an * 2.f)), rd);
    if (*an > 0.6f) dl->AddRectFilled(ImVec2(p0.x, p0.y + sp(1.25f)), ImVec2(p0.x + sp(0.5f), p1.y - sp(1.25f)), ui::ACCENT, sp(0.25f));
    ImU32 col = mix(ui::TEXT2, IM_COL32_WHITE, std::min(1.f, *an * 1.4f));
    ImVec2 ic_c(p0.x + sp(3.f), (p0.y + p1.y) * 0.5f);
    ui::icon(dl, ic, ic_c, sp(3.f), sel ? mix(ui::ACCENT, IM_COL32_WHITE, 0.55f) : col);
    if (badge) dl->AddCircleFilled(ic_c + ImVec2(sp(1.5f), -sp(1.5f)), sp(0.625f), badge, 12);
    if (expand > 0.02f) {
        dl->PushClipRect(p0, p1, true);
        dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(p0.x + sp(6.f), ic_c.y - ui::F.bold->FontSize * 0.5f - 1), ui::scale_alpha(col, expand), label);
        dl->PopClipRect();
    }
    if (focus) ui::focus_ring(dl, p0, p1, rd);
    if (expand < 0.5f && hov) ImGui::SetTooltip("%s", label);
    ImGui::PopID();
    return pressed;
}

/* ---------- footer: contextual controller / keyboard hints + status ---------- */

struct Hint { const char* pad; const char* key; const char* label; };

static void draw_footer(float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddLine(p, ImVec2(p.x + w, p.y), ui::HAIRLINE, 1.f);
    bool pad = A.pad_mode;
    std::vector<Hint> hints;
    if (A.page == Page::Library && !A.lib.empty()) {
        hints = {{"A", "Enter", "Play"}, {"X", "F", "Favorite"}, {"Y", "I", "Details"}};
        if (A.view == LibView::All) hints.push_back({"B", "Esc", "Back"});
    } else if (A.page == Page::Library) {
        hints = {{"A", "Enter", "Select"}};
    } else {
        hints = {{"A", "Enter", "Select"}, {"B", "Esc", "Back"}};
    }
    if (pad) hints.push_back({"LB", "", "Prev page"}), hints.push_back({"RB", "", "Next page"});
    hints.push_back({"Start", "F2", "Friends"});

    std::string status;
    ImU32 dot = ui::TEXT3;
    if (A.proc) { status = "Playing " + A.proc_title; dot = ui::ACCENT; }
    else if (A.in_room) { status = "Room " + A.code + "  -  " + A.lobby.status(); dot = A.lobby.connected() ? ui::GOOD : ui::WARN; }
    else if (A.job.running) { status = "Syncing saves..."; dot = ui::ACCENT; }
    ImFont* cf = ui::F.cap;
    float sw = 0;
    if (!status.empty()) {
        status = ui::ellipsize(cf, cf->FontSize, status, std::min(w * 0.4f, sp(48.f)));
        sw = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, status.c_str()).x + sp(2.f);
    }
    float hy = p.y + (h - (cf->FontSize + sp(0.75f))) * 0.5f, x = p.x;
    for (auto& hn : hints) {
        bool is_shoulder = hn.pad[0] == 'L' || hn.pad[0] == 'R';
        if (!pad && is_shoulder) continue;
        float need = ui::key_hint_width(pad, hn.pad, hn.key, hn.label);
        if (x + need > p.x + w - sw - sp(2.f)) break;
        x += ui::key_hint(dl, ImVec2(x, hy), pad, hn.pad, hn.key, hn.label) + sp(3.f);
    }
    if (!status.empty()) {
        float tx = p.x + w - sw + sp(2.f);
        dl->AddCircleFilled(ImVec2(tx - sp(1.f), p.y + h * 0.5f), sp(0.5f), dot, 12);
        dl->AddText(cf, cf->FontSize, ImVec2(tx, p.y + (h - cf->FontSize) * 0.5f), ui::TEXT2, status.c_str());
    }
    ImGui::Dummy(ImVec2(w, h));
}

/* ---------- global input: back, page switching, quick actions ---------- */

static void global_input() {
    ImGuiIO& io = ImGui::GetIO();
    if (!A.focused || A.proc) return;
    if (ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
    if (io.WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) || ImGui::IsKeyPressed(ImGuiKey_F2, false)) fr_toggle_panel();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
        if (fr_handle_back()) return;
    int dir = 0;
    if (FR.panel_focused) {}
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) dir = 1;
    if (!FR.panel_focused && ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) dir = -1;
    if (dir) A.page = static_cast<Page>((6 + (int)A.page + dir) % 6);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        if (A.page == Page::Library) {
            if (A.view == LibView::All) { A.view = LibView::Home; A.search_buf[0] = 0; A.sys_filter.clear(); A.vis_dirty = true; }
        } else {
            A.page = Page::Library;
        }
    }
    if (A.page == Page::Library && !A.lib.empty() && find_entry(A.sel_game)) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || (ImGui::IsKeyPressed(ImGuiKey_I, false) && !io.KeyCtrl)) A.show_details = true;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || (ImGui::IsKeyPressed(ImGuiKey_F, false) && !io.KeyCtrl)) toggle_favorite(A.sel_game);
    }
}

/* ---------- frame ---------- */

static void draw_frame() {
    ImGuiIO& io = ImGui::GetIO();
    fr_anim_panel();
    const bool fr_docked = io.DisplaySize.x >= 1100.f * S;
    const float fr_pw = fr_panel_w(io.DisplaySize.x);
    const float dock_w = fr_docked ? fr_pw * FR.panel_t : 0.f;
    const float view_w = io.DisplaySize.x - dock_w;
    A.focused = g_headless || (SDL_GetWindowFlags(A.win) & SDL_WINDOW_INPUT_FOCUS) != 0;
    if (A.page != A.last_page || A.view != A.last_view) { A.last_page = A.page; A.last_view = A.view; A.nav_seed = true; }
    /* D-pad zones: the sidebar and the content are separate focus zones, otherwise ImGui's scoring happily jumps diagonally
     * from a hero button to a sidebar entry. Left with nothing further left enters the sidebar, Right leaves it. */
    bool in_side = false;
    {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        for (ImGuiID id : A.side_ids_prev) if (id == g.NavId && id != 0) in_side = true;
        if (A.zone_req == 1) { in_side = true; A.seed_side = true; }
        if (A.zone_req == 2) { in_side = false; A.nav_seed = true; }
        A.zone_req = 0;
        if (A.nav_seed) in_side = false;
        A.side_ids_prev.swap(A.side_ids);
        A.side_ids.clear();
    }
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(view_w, io.DisplaySize.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);

    /* ---- sidebar (icons + labels; shrinks to icons only when narrow) ---- */
    bool collapsed = hubui::sidebar_collapsed(A.st.sidebar_mode, view_w, 1040.f * S);
    float full_w = sp(29.f), mini_w = sp(9.f), target = collapsed ? mini_w : full_w;
    if (A.side_w <= 0) A.side_w = target;
    ui::anim(A.side_w, target, 14.f);
    float sw = A.side_w, expand = std::max(0.f, std::min(1.f, (sw - mini_w) / (full_w - mini_w)));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::SIDEBAR);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.5f), sp(3.f)));
    ImGui::BeginChild("side", ImVec2(sw, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* sdl = ImGui::GetWindowDrawList();
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);   // the first D-pad press should land in the content, not the sidebar
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, !in_side);
    float iw = sw - sp(3.f);
    {   // logo mark + wordmark
        ImVec2 p = ImGui::GetCursorScreenPos();
        float ms = sp(5.f);
        ImVec2 m0(p.x + sp(0.5f), p.y), m1(m0.x + ms, m0.y + ms);
        ui_draw_brand_logo(sdl, m0, ms);
        if (expand > 0.02f) {
            sdl->PushClipRect(ImVec2(m1.x, p.y), ImVec2(p.x + iw, p.y + ms), true);
            sdl->AddText(ui::F.h2, ui::F.h2->FontSize, ImVec2(m1.x + sp(1.25f), p.y + (ms - ui::F.h2->FontSize) * 0.5f), ui::scale_alpha(ui::TEXT, expand), "RetroHub");
            sdl->PopClipRect();
        }
        ImGui::Dummy(ImVec2(iw, ms + sp(3.f)));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, sp(0.5f)));
    ImU32 room_badge = A.in_room ? (A.lobby.connected() ? ui::GOOD : ui::WARN) : 0;
    if (nav_item(Page::Library, "Library", ui::Icon::Home, iw, expand, 0)) A.page = Page::Library;
    if (nav_item(Page::Online, A.in_room ? "Play Online  (in room)" : "Play Online", ui::Icon::Globe, iw, expand, room_badge)) A.page = Page::Online;
    if (nav_item(Page::Saves, A.job.running ? "Saves  (syncing...)" : "Saves", ui::Icon::Cloud, iw, expand, A.job.running ? ui::ACCENT : 0)) A.page = Page::Saves;
    if (nav_item(Page::Controllers, "Controllers", ui::Icon::Gamepad, iw, expand, 0)) A.page = Page::Controllers;
    if (nav_item(Page::Settings, "Settings", ui::Icon::Gear, iw, expand, 0)) A.page = Page::Settings;
    if (nav_item(Page::About, "About / Updates", ui::Icon::Info, iw, expand, 0)) A.page = Page::About;
    ImGui::PopStyleVar();

    // bottom: collapse toggle + player card
    float card_h = sp(7.f), bs = sp(4.f);
    ImVec2 wp = ImGui::GetWindowPos();
    float bottom = wp.y + ImGui::GetWindowHeight() - sp(3.f);
    float left = wp.x + sp(1.5f);
    float icx = left + sp(3.f);
    {
        float xl = icx - bs * 0.5f, xr = left + iw - bs;
        ImGui::SetCursorScreenPos(ImVec2(xl + (xr - xl) * expand, bottom - card_h - sp(1.f) - bs));
        if (ui::icon_button("side_toggle", collapsed ? ui::Icon::ChevronR : ui::Icon::ChevronL, bs, false, collapsed ? "Expand sidebar" : "Collapse sidebar")) {
            A.st.sidebar_mode = collapsed ? 1 : 2;
            mark_dirty();
        }
    }
    {
        ImVec2 p(left, bottom - card_h);
        sdl->AddRectFilled(p, p + ImVec2(iw, card_h), ui::scale_alpha(ui::PANEL, expand), sp(1.5f));
        sdl->AddRect(p, p + ImVec2(iw, card_h), ui::scale_alpha(ui::HAIRLINE, expand), sp(1.5f), 0, 1.f);
        ImVec2 ac(left + sp(3.f), p.y + card_h * 0.5f);
        sdl->AddCircleFilled(ac, sp(2.25f), ui::ACCENT, 24);
        std::string nm = A.st.name.empty() ? "?" : A.st.name;
        char init[8] = {(char)std::toupper((unsigned char)nm[0]), 0};
        ImVec2 its = ui::F.bold->CalcTextSizeA(ui::F.bold->FontSize, FLT_MAX, 0.f, init);
        sdl->AddText(ui::F.bold, ui::F.bold->FontSize, ac - its * 0.5f, IM_COL32_WHITE, init);
        const char* stt = A.proc ? "In game" : (A.in_room ? (A.lobby.connected() ? "In a room" : "Connecting") : "Offline");
        ImU32 sc = A.proc ? ui::ACCENT : (A.in_room ? (A.lobby.connected() ? ui::GOOD : ui::WARN) : ui::TEXT3);
        sdl->AddCircleFilled(ac + ImVec2(sp(1.6f), sp(1.6f)), sp(0.6f), sc, 12);
        sdl->AddCircle(ac + ImVec2(sp(1.6f), sp(1.6f)), sp(0.6f), ui::SIDEBAR, 12, 2.f);
        if (expand > 0.05f) {
            sdl->PushClipRect(p, p + ImVec2(iw, card_h), true);
            float tx = left + sp(6.f), mw = iw - sp(6.f) - sp(1.f);
            sdl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(tx, p.y + sp(1.25f)), ui::scale_alpha(ui::TEXT, expand),
                         ui::ellipsize(ui::F.bold, ui::F.bold->FontSize, nm, mw).c_str());
            sdl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p.y + sp(1.25f) + ui::F.bold->FontSize + sp(0.25f)), ui::scale_alpha(ui::TEXT2, expand), stt);
            sdl->PopClipRect();
        }
    }
    ImGui::PopItemFlag();
    ImGui::PopItemFlag();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 0);

    /* ---- content ---- */
    float hpad = (view_w - sw > 1200.f * S) ? sp(5.f) : sp(4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(hpad, sp(3.f)));
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, in_side);
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    fr_top_bar();
    if (A.proc) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = sp(5.f);
        dl->AddRectFilled(p, p + ImVec2(w, h), mix(ui::PANEL, ui::ACCENT, 0.22f), sp(1.5f));
        dl->AddRect(p, p + ImVec2(w, h), with_alpha(ui::ACCENT, 0.5f), sp(1.5f), 0, 1.f);
        float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 4.f);
        dl->AddCircleFilled(p + ImVec2(sp(2.f), h * 0.5f), sp(0.6f) + pulse * sp(0.25f), ui::ACCENT, 16);
        std::string m = A.proc_title + " is running - the hub is ready again when it closes.";
        dl->AddText(ui::F.body, ui::F.body->FontSize, p + ImVec2(sp(3.5f), (h - ui::F.body->FontSize) * 0.5f),
                    ui::TEXT, ui::ellipsize(ui::F.body, ui::F.body->FontSize, m, w - sp(5.f)).c_str());
        ImGui::Dummy(ImVec2(w, h + sp(2.f)));
    }

    float footer_h = sp(5.5f);
    ImGuiWindowFlags pf = A.page == Page::Library ? (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse) : ImGuiWindowFlags_None;
    ImGui::BeginChild("page", ImVec2(0, -footer_h), ImGuiChildFlags_NavFlattened, pf);
    switch (A.page) {
        case Page::Library: page_library(); break;
        case Page::Online: page_online(); break;
        case Page::Saves: page_saves(); break;
        case Page::Controllers: page_controllers(); break;
        case Page::Settings: page_settings(); break;
        case Page::About: page_about(); break;
    }
    ImGui::EndChild();
    draw_footer(footer_h);
    ImGui::EndChild();
    ImGui::PopItemFlag();

    {   // a Left/Right move that found no neighbour hands the focus to the other zone
        ImGuiContext& g = *ImGui::GetCurrentContext();
        bool miss = g.NavMoveSubmitted && g.NavMoveResultLocal.ID == 0 && g.NavMoveResultOther.ID == 0;
        if (miss && !g.IO.WantTextInput && !ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            if (g.NavMoveDir == ImGuiDir_Left && !in_side) A.zone_req = 1;
            else if (g.NavMoveDir == ImGuiDir_Right && in_side) A.zone_req = 2;
        }
    }

    draw_overlays();
    global_input();
    draw_toasts(io.DisplaySize.y - footer_h - sp(1.5f), view_w - sp(3.f));
    ImGui::End();
    fr_draw_panel(io.DisplaySize.x, io.DisplaySize.y);
    fr_draw_toasts(io.DisplaySize.x - (fr_docked ? fr_pw * FR.panel_t : (A.st.show_friends ? fr_pw * FR.panel_t : 0.f)) - sp(2.f), sp(7.5f));
}

/* ============================ main ============================ */

/* ---- headless screenshot harness (testing aid, no window is ever shown) ----
 * HUB_SCREENSHOT=<file.bmp>  renders with SDL's "dummy" video driver + software renderer, writes the frame, exits.
 * HUB_SHOT_SECS (default 4) wall-clock seconds to run first; HUB_SHOT_SIZE=WxH; HUB_SCALE=DPI scale (1, 1.5, 2);
 * HUB_SHOT_KEYS=right,right,down,enter,... keyboard-nav presses sent at 0.3s intervals before the shot;
 * HUB_SHOT_PAD=1 shows controller glyphs in the footer. */
static void save_screenshot(SDL_Renderer* ren, const char* path) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren, &w, &h);
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
}
static ImGuiKey shot_key(const std::string& n) {
    if (n == "right") return ImGuiKey_RightArrow;
    if (n == "left") return ImGuiKey_LeftArrow;
    if (n == "up") return ImGuiKey_UpArrow;
    if (n == "down") return ImGuiKey_DownArrow;
    if (n == "enter") return ImGuiKey_Enter;
    if (n == "esc") return ImGuiKey_Escape;
    if (n == "f2") return ImGuiKey_F2;
    if (n == "i") return ImGuiKey_I;
    if (n == "f") return ImGuiKey_F;
    return ImGuiKey_None;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    shell_set_app_id();
    const char* shot_path = std::getenv("HUB_SCREENSHOT");
    if (shot_path) { SDL_setenv("SDL_VIDEODRIVER", "dummy", 1); g_headless = true; }
    SDL_SetMainReady();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");   // smooth downscaling of cover art
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        MessageBoxA(nullptr, SDL_GetError(), "RetroHub", MB_ICONERROR);
        return 1;
    }

    float ddpi = 96.f;
    SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr);
    S = std::max(1.0f, ddpi / 96.0f);
    if (const char* sc = std::getenv("HUB_SCALE")) S = std::max(0.75f, (float)std::atof(sc));

    // settings
    A.settings_path = hub::data_dir() + "hub.json";
    A.st.server = hubnet::default_server();
    hubui::load_settings(A.settings_path, &A.st);
    if (const char* pg = std::getenv("HUB_START_PAGE")) {   // screenshot/testing aid: 0 Library, 1 Online, 2 Saves, 3 Controllers, 4 Settings, 5 About
        int n = std::atoi(pg);
        if (n >= 0 && n <= 5) A.page = static_cast<Page>(n);
    }
    if (const char* sg = std::getenv("HUB_SELECT_GAME")) A.env_select = sg;   // preselect a library game key (screenshots)
    if (const char* gm = std::getenv("HUB_START_GROUP")) A.group_mode = std::max(0, std::min(2, std::atoi(gm)));   // screenshots: 0 genre, 1 system, 2 A-Z
    if (const char* vw = std::getenv("HUB_START_VIEW")) {                    // "all" opens the All Games grid
        if (std::string(vw) == "all") A.view = LibView::All;
    }
    if (A.st.name.empty() || A.st.name == "Player") {   // default to the Windows user name
        wchar_t un[128];
        DWORD ul = 128;
        std::string u = GetUserNameW(un, &ul) ? U(un) : "";
        A.st.name = hubui::sanitize_player_name(u);
    }
    // No ROM folder is assumed: an empty library offers detected folders as one-click "Add" suggestions.
    if (!A.st.vault_dir.empty()) hub::saves::set_vault_dir(A.st.vault_dir);

    int win_w = (int)(1280 * S), win_h = (int)(780 * S);
    {   // never open larger than the screen
        SDL_Rect ub;
        if (!shot_path && SDL_GetDisplayUsableBounds(0, &ub) == 0) {
            win_w = std::min(win_w, (int)(ub.w * 0.94f));
            win_h = std::min(win_h, (int)(ub.h * 0.94f));
        }
    }
    if (shot_path) {
        win_w = (int)(1280 * S);
        win_h = (int)(780 * S);
        if (const char* sz = std::getenv("HUB_SHOT_SIZE")) std::sscanf(sz, "%dx%d", &win_w, &win_h);
    }
    A.win = SDL_CreateWindow("RetroHub", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
                             SDL_WINDOW_RESIZABLE | (shot_path ? 0 : SDL_WINDOW_SHOWN));
    SDL_Renderer* ren = A.win ? SDL_CreateRenderer(A.win, -1, shot_path ? SDL_RENDERER_SOFTWARE : (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)) : nullptr;
    if (!A.win || !ren) {
        MessageBoxA(nullptr, SDL_GetError(), "RetroHub", MB_ICONERROR);
        return 1;
    }
    SDL_SetWindowMinimumSize(A.win, (int)(960 * S), (int)(600 * S));
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(A.win, &wm)) A.hwnd = wm.info.win.window;
    if (!shot_path) {
        shell_set_window_icon(A.win);
        shell_ensure_start_menu_shortcut();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    ui::setup_fonts(S);
    ui::setup_theme(S);
    ImGui_ImplSDL2_InitForSDLRenderer(A.win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);
    hubart::init(ren, hub::data_dir() + "art\\");
    hubart::set_enabled(A.st.download_art);

    if (shot_path) {   // headless screenshot aids for the friends UI (never any network)
        A.st.show_friends = std::getenv("HUB_SHOT_FRIENDS") != nullptr;
        if (const char* se = std::getenv("HUB_SHOT_EMU")) {   // screenshot aid: show a 3DS / Wii U dialog (azahar | cemu | unzip | failed)
            std::string m = se;
            if (m == "azahar" || m == "cemu") { EMU.kind = m == "cemu" ? hubemu::Kind::Cemu : hubemu::Kind::Azahar; EMU.state = hubemu::Installer::Asking; }
            else if (m == "failed") { EMU.kind = hubemu::Kind::Azahar; EMU.error = "checksum mismatch: the download was rejected"; EMU.state = hubemu::Installer::Failed; }
            else if (m == "unzip") { UNZ.key = A.env_select; UNZ.state = UnzipJob::Asking; }
        }
        if (std::getenv("HUB_SHOT_FAKE_FRIENDS")) {
            fr_fake_init();
            if (std::getenv("HUB_SHOT_CHAT")) { fr_open_chat("miauid"); FR.chat_unread = 2; }
            if (std::getenv("HUB_SHOT_BELL")) FR.shot_bell = true;
        }
        if (std::getenv("HUB_SHOT_FAKE_ROOM")) {
            g_fake_room = true;
            if (const char* hcv = std::getenv("HUB_SHOT_HOSTCHANGE")) {   // screenshot aid: room right after a host change ("me" | "other")
                g_fake_other_host = std::string(hcv) == "other";
                std::string nm = g_fake_other_host ? "Mia" : "You";
                push_toast(hubui::host_changed_text(nm, !g_fake_other_host), ui::GOOD, 60.0);
            }
            A.in_room = true;
            A.code = "K7M2QX";
            A.page = Page::Online;
            if (std::getenv("HUB_SHOT_PICKER")) FR.open_picker = true;
            if (const char* rg = std::getenv("HUB_SHOT_ROOM_GAME")) A.selected_game = rg;   // e.g. smash64, remix
            if (A.selected_game == "remix" && std::getenv("HUB_SHOT_PICKS")) {   // fake picks for the match picker
                A.my_pick.p[0] = {0x1D, 2};
                A.picks[1].p[0] = {0x00, 0};
                A.picks[2].p[0] = {0x20, 4};
                A.rules.stage = 0x31;
                A.rules.stocks = 4;
            }
            if (std::getenv("HUB_SHOT_LOCALS2")) A.my_locals = 2;
        }
    }
    fr_start();

    refresh_pads();
    start_scan();
    auto_sync();   // pull in saves from other PCs at start
    const bool shot_update = shot_path && std::getenv("HUB_SHOT_UPDATE");   // screenshot the update prompt
    if ((!shot_path || shot_update) && !hubupdate::launch_check_disabled()) hubupdate::check_async(false);

    double shot_secs = 4.0;
    std::vector<ImGuiKey> shot_keys;
    ULONGLONG shot_t0 = GetTickCount64();
    if (shot_path) {
        if (const char* ss = std::getenv("HUB_SHOT_SECS")) shot_secs = std::atof(ss);
        if (const char* ks = std::getenv("HUB_SHOT_KEYS")) {
            std::stringstream kss(ks);
            std::string tok;
            while (std::getline(kss, tok, ',')) if (shot_key(tok) != ImGuiKey_None) shot_keys.push_back(shot_key(tok));
        }
        if (const char* pd = std::getenv("HUB_SHOT_PAD")) A.pad_mode = std::atoi(pd) != 0;
    }
    float shot_wx = 0, shot_wy = 0, shot_wn = 0;   // HUB_SHOT_WHEEL=x,y,notches (notches > 0 scrolls right/down)
    int shot_wheel_stage = 0;                      // 0 pending, 1 pointer placed, 2 wheel sent
    double shot_wheel_t = 0;
    const bool shot_wheel = shot_path && std::getenv("HUB_SHOT_WHEEL") &&
                            std::sscanf(std::getenv("HUB_SHOT_WHEEL"), "%f,%f,%f", &shot_wx, &shot_wy, &shot_wn) == 3;
    size_t shot_key_i = 0;
    bool shot_key_down = false;
    double shot_last_key = 0;

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_MOUSEWHEEL && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
                float wx = e.wheel.preciseX, wy = e.wheel.preciseY;
                if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { wx = -wx; wy = -wy; }
                if (route_wheel(io.MousePos.x, io.MousePos.y, wx, wy)) continue;   // over a shelf: scroll that row, not the page
            }
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
            if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) refresh_pads();
            // which device was used last decides whether the footer shows controller glyphs or key caps
            if (e.type == SDL_CONTROLLERBUTTONDOWN || (e.type == SDL_CONTROLLERAXISMOTION && std::abs((int)e.caxis.value) > 16000)) A.pad_mode = true;
            if (e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEMOTION) A.pad_mode = false;
        }
        hubart::pump();
        if (!(shot_path && std::getenv("HUB_SHOT_SKELETON")) && A.scan_ready.exchange(false)) {
            if (A.scan_th.joinable()) A.scan_th.join();
            A.scanning = false;
            apply_scan();
        }
        poll_process();
        poll_job();
        run_pending_sync();
        poll_dolphin();
        poll_emulators();
        lobby_tick();
        fr_tick();
        if (hubupdate::installer_started()) running = false;   // the installer replaces our files and relaunches us
        if (A.dirty && !g_headless && !ImGui::IsAnyItemActive()) {
            hubui::save_settings(A.settings_path, A.st);
            A.dirty = false;
        }

        if (SDL_GetWindowFlags(A.win) & SDL_WINDOW_MINIMIZED) { SDL_Delay(50); continue; }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        double shot_el = (GetTickCount64() - shot_t0) / 1000.0;
        if (shot_path && !shot_keys.empty()) {   // scripted keyboard navigation, starting once the library has loaded
            double start = std::max(1.5, shot_secs - 0.35 * (shot_keys.size() + 1));
            if (shot_key_down) { io.AddKeyEvent(shot_keys[shot_key_i - 1], false); shot_key_down = false; }
            else if (shot_key_i < shot_keys.size() && shot_el > start && shot_el - shot_last_key > 0.3 && A.scanned_once) {
                io.AddKeyEvent(shot_keys[shot_key_i++], true);
                shot_key_down = true;
                shot_last_key = shot_el;
            }
        }
        if (shot_wheel && shot_wheel_stage == 0 && A.scanned_once && shot_el > std::max(2.0, shot_secs - 2.0)) {
            io.AddMousePosEvent(shot_wx, shot_wy);   // window pixels
            shot_wheel_stage = 1;
        } else if (shot_wheel && shot_wheel_stage == 1) {
            if (!route_wheel(shot_wx, shot_wy, 0.f, -shot_wn)) io.AddMouseWheelEvent(0.f, -shot_wn);
            shot_wheel_stage = 2;
            shot_wheel_t = shot_el;
        }
        ImGui::NewFrame();
        ImGui::PushFont(ui::F.body);
        draw_frame();
        if (!shot_path || shot_update) draw_update_popup();
        ImGui::PopFont();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 16, 17, 22, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        if (shot_path) {
            bool ready = shot_el >= shot_secs && (A.scanned_once || shot_el > shot_secs + 20 || std::getenv("HUB_SHOT_NOWAIT")) && shot_key_i >= shot_keys.size() && !shot_key_down &&
                         (!shot_wheel || (shot_wheel_stage == 2 && shot_el - shot_wheel_t > 1.5));
            if (ready) { save_screenshot(ren, shot_path); running = false; }
            SDL_Delay(8);
        }
        SDL_RenderPresent(ren);
    }

    // clean shutdown
    if (A.dirty) hubui::save_settings(A.settings_path, A.st);
    A.lobby.stop();
    FR.cli.stop();
    if (A.scan_th.joinable()) A.scan_th.join();
    if (A.job.th.joinable()) A.job.th.join();
    if (DL.th.joinable()) DL.th.join();
    hub::flush_library_state();
    if (A.proc) CloseHandle(A.proc);
    for (auto* p : A.pads) SDL_GameControllerClose(p);
    hubart::shutdown();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(A.win);
    SDL_Quit();
    CoUninitialize();
    return 0;
}
