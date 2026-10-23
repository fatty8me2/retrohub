#pragma once

/* app.h - shared state, types and declarations of the hub UI. Every src/hub/*.cpp UI file includes only this. */

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
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "art.h"
#include "updater.h"
#include "update_apply.h"
#include "games.h"
#include "emulators.h"
#include "controls.h"
#include "netplay_dolphin.h"
#include "library.h"
#include "logic.h"
#include "savestore.h"
#include "cloud.h"
#include "shell_integration.h"
#include "invite.h"
#include "session.h"
#include "netplay_3ds_run.h"
#include "ui_kit.h"
#include "game_settings.h"
#include "launch_settings.h"

#ifndef HUB_VERSION_STR
#define HUB_VERSION_STR "0.0.0"
#endif

using hub::GameInfo;
using hub::RomFile;

using ui::button;
using ui::chip;
using ui::gradient_round_rect;
using ui::mix;
using ui::pill;
using ui::pill_at;
using ui::sp;
using ui::with_alpha;


/* shared constants (internal linkage: every file gets its own copy) */
static float& S = ui::S;  // DPI scale (lives in ui_kit)
static const ImU32 COL_ACCENT = ui::ACCENT;
static const ImU32 COL_GOOD = ui::GOOD;
static const ImU32 COL_BAD = ui::BAD;
static const ImU32 COL_WARN = ui::WARN;
static const ImU32 COL_PANEL = ui::PANEL;
static const ImU32 COL_PANEL2 = ui::PANEL2;
static const char* kNativeKey = "native:smash64";
static const float kTileAspect = 1.3f;   // tile width / height, shared by shelves, grid and hero art
static const ImU32 COL_REMIX = IM_COL32(250, 150, 40, 255);


/* ============================ types ============================ */

enum class Page { Library, Online, Saves, Controllers, Settings, About };

enum class LibView { Home, All };

/* A transient message (bottom right). */
struct Toast {
    std::string text;
    ImU32 col = 0;
    double t0 = 0, until = 0;
    bool top = false;   // stacks from the top edge instead of the bottom (keeps the chat input clear)
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
    enum Kind { None, Sync, Export, Import, Status } kind = None;   // Status: cloud_status probe only
    std::thread th;
    std::atomic<bool> running{false}, done{false};
    hub::saves::SyncReport report;
    bool export_ok = false;
    std::string export_err, path;
    bool include_states = false, manual = false;
    bool cloud = false;                              // also sync through the RetroHub account
    bool cloud_skipped = false;                      // cloud unavailable or unregistered: no transfer attempted
    hub::saves::CloudConfig cloud_cfg;
    hub::saves::CloudStatus cloud_stat;              // filled by Status jobs and by Sync jobs with cloud on
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
    std::vector<hubui::ConsoleSection> secs;     // All Games sections (cached by rebuild_vis)
    std::map<std::string, int> chip_counts;      // per-console counts under the current search + Online toggle
    int chip_total = 0;
    bool grid_scroll_top = false;                // reset the All Games scroll on the next frame
    bool picks_open = false;                      // Home: "Top picks you don't have yet" expanded
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
    hub::saves::CloudStatus cloud_stat;           // last cloud status (Saves page line)
    bool cloud_stat_known = false;                // a probe has answered since the toggle went on
    bool cloud_probed = false;                    // a probe was started for the current toggle state
    bool sync_error_toasted = false;              // a real sync error toast was shown this session
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
    std::string pending_play;    // --play <key> (own command line or forwarded by a second launch): started once the library has loaded
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
};

struct SyncSuggestion { std::string label, path; };

/* Explicit, confirmed unpacking of a zipped 3DS image (Azahar cannot open .zip files). */
struct UnzipJob {
    enum State { Idle, Asking, Working, Failed } state = Idle;
    std::thread th;
    std::atomic<bool> done{false}, ok{false}, cancel{false};
    bool quiet = false;   // unpacked for a room (GameCube netplay): do not start the game afterwards
    std::atomic<uint64_t> got{0}, total{0};
    std::string key, error;
    std::mutex mu;
};

/* ---------- GameCube online: Dolphin's own NetPlay, started and joined by RetroHub (netplay_dolphin.h) ----------
 * Dolphin's ENet/UDP goes through the room like 3DS: the host's Dolphin hosts on 127.0.0.1:<port>, every guest's Dolphin
 * connects (direct mode) to a loopback forwarder, and hubnp3ds::Runner carries the datagrams through hubnet::Session
 * (hole punch, relay fallback). No traversal server, no port forwarding. */
struct GcOnline {
    gcnp::Run run;
    hubnp3ds::Runner fw;
    bool active = false, host = false, ready_sent = false, guest_wait = false, host_ready = false, checked = true;
    gcnp::Spec spec;                 // host: what we announce; guest: what the host announced
    hub::GameEntry entry;
    int launch_no = 0, port = 0;
    std::string fp_path, fp_sha;     // disc fingerprint (computed once per path on a worker thread)
    uint64_t fp_size = 0;
    std::atomic<bool> fp_done{false};
    std::thread fp_th;
};

struct ArtRef {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    bool loading = false;   // show a gentle shimmer for a few seconds while the fetch is in flight
};

struct TileSpec {
    std::string id, title, subtitle, badge;   // badge: system pill (empty inside a console section)
    std::string tag, tag_tip;                 // subtle state tag ("ZIP", "NATIVE", ...) + hover explanation
    bool tag_warn = false;                    // tint the tag as a warning (needs keys / encrypted)
    ImU32 c0 = 0, c1 = 0;
    ArtRef art;
    bool favorite = false, online = false, selected = false, owned = true, show_star = true;
    const char* cta = nullptr;   // hover caption for catalog tiles ("Add ROM")
};

struct TileResult {
    bool visible = false, hovered = false, focused = false, clicked = false, activated = false, star = false, details = false;
};

struct Hint { const char* pad; const char* key; const char* label; };


/* Headless screenshot driver state (HUB_SCREENSHOT / HUB_SHOT_* knobs; shot.cpp). */
struct ShotDriver {
    const char* path = nullptr;
    bool update = false;                  // HUB_SHOT_UPDATE: also draw the update prompt
    double secs = 4.0;                    // HUB_SHOT_SECS
    std::vector<ImGuiKey> keys;           // HUB_SHOT_KEYS
    ULONGLONG t0 = 0;
    double el = 0;                        // seconds since start (set by pre_frame)
    float wx = 0, wy = 0, wn = 0;         // HUB_SHOT_WHEEL=x,y,notches (notches > 0 scrolls right/down)
    bool wheel = false;
    int wheel_stage = 0;                  // 0 pending, 1 pointer placed, 2 wheel sent
    double wheel_t = 0;
    size_t key_i = 0;
    bool key_down = false;
    double last_key = 0;
    void init(const char* shot_path);
    void pre_frame(ImGuiIO& io);
    bool ready() const;
};
extern ShotDriver SH;
void shot_init_ui();

/* ============================ globals ============================ */

/* app.cpp */
extern App A;
extern bool g_headless;   // HUB_SCREENSHOT run: never launch games or touch the lobby

/* launch.cpp */
extern hubemu::Installer EMU;   // download-on-demand job + its dialog state
extern UnzipJob UNZ;
extern hubnp3ds::Runner NP3;                 // 3DS local-wireless forwarder (np_3ds)
extern std::string NP3_mp;                    // "-m ..." for Azahar once NP3 is ready
extern hub::GameEntry NP3_game;
extern bool NP3_pending;

/* launch_online.cpp */
extern GcOnline GCO;

/* lobby.cpp */
extern bool g_fake_room;
extern bool g_fake_other_host;   // HUB_SHOT_HOSTCHANGE=other: Mia (slot 0) is the host, we are slot 1

/* tiles.cpp */
extern bool g_suppress_click;   // set by shelf_strip while a drag-pan is in progress / just ended


/* ============================ functions (grouped by the file that defines them) ============================ */


/* ---- util.cpp ---- */
std::wstring W(const std::string& s);
std::string U(const std::wstring& w);
bool file_exists(const std::string& p);
std::string dir_of(const std::string& p);
std::string file_name(const std::string& p);
std::string pick_path(HWND owner, bool folder, const wchar_t* filter_name, const wchar_t* filter_spec);
std::string pick_save_path(HWND owner, const wchar_t* default_name, const wchar_t* filter_name,
                                  const wchar_t* filter_spec, const wchar_t* default_ext);
bool dir_exists(const std::string& p);
std::string read_text_file(const std::string& p);
void make_dirs(const std::string& p);
std::string with_slash(std::string p);
std::string find_upward(const std::string& rel);
std::string env_var(const wchar_t* name);
void count_files(const std::string& dir, int* files, uint64_t* bytes);
void remove_tree(const std::string& dir);
std::string fmt_time(uint64_t unix_s);
uint64_t unix_now();
std::string fmt_ago(uint64_t unix_s);
void game_colors(const std::string& id, ImU32* a, ImU32* b);
const char* short_name(const std::string& id);
void system_colors(hub::System s, ImU32* a, ImU32* b);
const char* sys_badge(hub::System s);
void entry_colors(const hub::GameEntry& e, ImU32* a, ImU32* b);


/* ---- app.cpp ---- */
void push_toast(const std::string& s, ImU32 col, double secs = 6.0, bool top = false);
void set_notice(const std::string& s, double secs = 8.0);
void mark_dirty();
std::string server_url();
std::string smash_exe_path();
std::string n64run_path();


/* ---- lib_state.cpp ---- */
bool is_native(const hub::GameEntry& e);
std::string sys_short(const hub::GameEntry& e);
std::string no_slash(std::string p);
void start_scan();
const hub::GameState& gstate(const std::string& key);
void refresh_state(const std::string& key);
const hub::GameEntry* find_entry(const std::string& key);
void apply_scan();
const GameInfo* catalog_for(const hub::GameEntry& e);
void rebuild_vis();
bool supports_online(const hub::GameEntry& e);
int max_players_for(const hub::GameEntry& e);
int local_players_for(const hub::GameEntry& e);
std::string room_id(const hub::GameEntry& e);
const hub::GameEntry* find_room_entry(const std::string& id);
std::string upper(std::string s);
void my_games(std::map<std::string, std::string>* games, std::vector<std::string>* keys);


/* ---- save_jobs.cpp ---- */
void set_toast(const std::string& s, bool good = true, double secs = 6.0);
void start_job(SaveJob::Kind k, const std::string& path, bool manual);
const std::vector<SyncSuggestion>& sync_suggestions();
void start_sync(bool manual);
bool sync_enabled();                 // a sync folder is set or cloud saves are on
void start_cloud_probe();            // Status job: refresh the cloud status line
void run_pending_sync();
void auto_sync();
std::string report_summary(const hub::saves::SyncReport& r, const char* sent_word, const char* got_word);
void poll_job();


/* ---- launch.cpp ---- */
bool spawn(const std::string& exe, const std::string& args, const std::string& cwd,
                  const std::vector<std::pair<std::string, std::string>>& env, const RunInfo& ri);
void poll_process();
std::string find_core(const std::string& name);
std::string quote(const std::string& s);
std::string title_arg(const std::string& game_title);
std::string save_stem_arg(const hub::GameEntry& e);
std::vector<std::pair<std::string, std::string>> smash_env(const std::string& exe);
RunInfo run_info_for(const hub::GameEntry& e, bool online);
uint64_t unpacked_size(const hub::GameEntry& e);
std::string launch_path_for(const hub::GameEntry& e);
void launch_external(const hub::GameEntry& e, const RunInfo& ri, const std::string& battery, const std::string& mp_arg = "");
void start_unzip(const hub::GameEntry& e);
void play_entry(const hub::GameEntry& e);
void add_game_shortcut(const hub::GameEntry& e);   // "Add to desktop": .lnk to RetroHub.exe --play <key> (shell_shortcut_dir())
void flash_taskbar();                              // FlashWindowEx on the hub window; never takes focus


/* ---- launch_online.cpp ---- */
bool gc_needs_unpack(const hub::GameEntry& e);
bool gc_fingerprint_ready(const std::string& path);
bool gc_process_alive();
void gc_kill();
void gc_prefetch(const hub::GameEntry& e);
bool gc_host_spec(const hub::GameEntry& e, int players, gcnp::Spec* out);
bool gc_begin(const hub::GameEntry& e, bool host, int slot, int port);
void gc_stop_all();
void gc_tick();
void gc_guest_prepare(const hub::GameEntry& e, const gcnp::Spec& s, int launch_no);
void gc_guest_poll();
void launch_online_3ds(const hub::GameEntry& e, int launch_no, bool is_host, const hubui::Json* msg);
void poll_np3ds();
void launch_online(const hub::GameEntry& e, int launch_no, int delay, bool is_host, const std::string& core_in, bool share_save,
                          const hubui::PortPlan* host_plan = nullptr, const hubui::Json* launch_msg = nullptr,
                          const hubui::MatchSpec* host_match = nullptr, const std::string* launch_body = nullptr);

/* settings_games_ui.cpp: per-game settings (vault: _settings\game_settings.json) */
extern hub::gs::Store GSTORE;
void gs_load();   // after the vault dir is known
hub::gs::Values gs_values_for(const hub::GameEntry& e);   // resolved incl. the global fullscreen default
void gs_draw_game_section(const hub::GameEntry& e);       // details modal
void gs_draw_defaults_area();                              // Settings page


/* ---- lobby.cpp ---- */
bool room_connected();
std::vector<hubnet::Member> room_members();
int room_my_slot();
int locals_of(int slot);
std::vector<hubui::MemberPads> member_pads();
std::string member_name(int slot);
bool match_active();
bool match_remix_roster();
std::map<int, hubui::MemberPick> all_picks();
void send_my_pick();
void send_rules();
void leave_room();
void enter_room(bool host, const std::string& code);
bool is_host();
bool allow_version_mismatch();
std::vector<hubui::PeerInfo> peer_list();
void lobby_tick();


/* ---- ui_common.cpp ---- */
void page_header(const char* title, const char* sub);
void begin_panel(const char* id, ImVec2 size = ImVec2(0, 0), ImU32 bg = COL_PANEL);
void end_panel();
void text_wrapped_disabled(const std::string& s);
void add_rom_folder_path(const std::string& p);
void add_rom_folder();
void locate_smash();


/* ---- tiles.cpp ---- */
void toggle_favorite(const std::string& key);
void section_title(const std::string& t);
void refresh_save_info(const hub::GameEntry& e);
std::string size_text(uint64_t b);
void open_in_explorer(const std::string& dir);
ArtRef art_lookup(const std::string& seen_key, bool have_name, SDL_Texture* tex, int w, int h);
ArtRef art_for_entry(const hub::GameEntry& e);
ArtRef art_for_pick(const GameInfo& g);
std::string initials(const std::string& t);
void draw_no_art(ImDrawList* dl, const TileSpec& t, ImVec2 p0, ImVec2 p1, float rad, float dim);
TileResult draw_tile(const TileSpec& t, ImVec2 size);
TileSpec spec_for_entry(const hub::GameEntry& e, const char* shelf, bool recent_sub, bool show_sys = true);
void handle_entry_result(const hub::GameEntry& e, const TileSpec& spec, const TileResult& r);


/* ---- shelves.cpp ---- */
float shelf_arrow_size();
float shelf_arrows_w();
void shelf_header(const char* title, const std::string& right, float bleed, int count = -1, bool* link_clicked = nullptr, bool arrows = false);
bool route_wheel(float px, float py, float wx, float wy);
void shelf_strip(const std::string& id, int count, float tw, float th, const std::function<TileResult(int, ImVec2)>& draw_one);


/* ---- page_library.cpp ---- */
void go_add_rom(const GameInfo& g);
void home_shelves();
void set_nav_here();
void seed_nav_here();
float action_row(const hub::GameEntry& e, float bh, bool draw, ImVec2 at, bool compact, bool seed_nav = false);
void draw_hero(const hub::GameEntry& e, float h);
void skeleton_home();
void centered_text(ImDrawList* dl, ImFont* f, float cx, float y, ImU32 col, const char* s);
void empty_library_state();
void library_top_bar();
void selected_bar(const hub::GameEntry& e);
void all_games_view();
void draw_details_modal();
void ensure_selection();
void sync_card();
void page_library();


/* ---- match_ui.cpp ---- */
bool match_tile(const char* id, const std::string& label, ImVec2 sz, bool sel, ImU32 tint);
void member_pick_row(int slot);
void match_panel(bool host, float width);
void member_card(const hubui::PeerInfo* p, int slot, ImVec2 size);


/* ---- page_online.cpp ---- */
void page_online_home();
void page_online_room();
void page_online();


/* ---- page_controllers.cpp ---- */
void refresh_pads();
void stick(ImDrawList* dl, ImVec2 c, float r, float x, float y, const char* label);
void btn_dot(ImDrawList* dl, ImVec2 c, float r, bool on, const char* label);
void page_controllers();
void open_controls_for(const hub::GameEntry& e);
void open_port_assign();                         // Controllers page in "Assign players"
bool draw_players_strip(int players);            // library details: "Players: 1 2 3 4" strip; true = Reassign clicked
std::string controls_arg(const std::string& system_key, const std::string& game_key);
hubctl::Profile controls_profile(const std::string& system_key, const std::string& game_key);
hubemu::DolphinPads dolphin_pads_for(const hubctl::Profile& gc_profile);
hubemu::AzPad azahar_pad_for(const hubctl::Profile& profile_3ds);


/* ---- page_saves.cpp ---- */
void refresh_vault();
void result_line(const std::string& s, bool ok);
void page_saves();


/* ---- page_settings.cpp ---- */
void settings_label(const char* title, const char* hint = nullptr);
void page_settings();
void draw_account_section();   // account_ui.cpp: Settings > Account (link a PC, export / import)


/* ---- find_games_ui.cpp ---- */
void find_games_open();   // start the scan + checklist modal (first run, Settings > ROM folders)
void draw_find_games();   // call every frame


/* ---- emu_dialogs.cpp ---- */
void poll_emulators();
void emu_modal_begin(const char* name, bool* open_flag);
void draw_emulator_dialogs();


/* ---- page_about.cpp ---- */
void draw_overlays();
void page_about();


/* ---- frame.cpp ---- */
void draw_update_popup();
void draw_toasts(float bottom, float right, float top);
bool nav_item(Page pg, const char* label, ui::Icon ic, float w, float expand, ImU32 badge);
void draw_footer(float h);
void global_input();
void draw_frame();


/* ---- shot.cpp ---- */
void save_screenshot(SDL_Renderer* ren, const char* path);
ImGuiKey shot_key(const std::string& n);


#include "friends_ui.h"
