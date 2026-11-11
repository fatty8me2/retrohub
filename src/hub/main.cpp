/* RetroHub.exe entry point. */
#include "app.h"
#include "libview_ui.h"
#include "prefs.h"
#include "tv_logic.h"
#include "homefun.h"
#include "popup_guard.h"
#include "qm_pad.h"
#include "dolphin_speed.h"
#include "hang_watchdog.h"
#include "hub_log.h"
#include "winutil.h"
#include "hub_version.h"
#include "crash_report.h"
#include "crash_report_ui.h"
#include "idle_logic.h"
#include "boot_anim.h"
#include "toast.h"
#include "start_hint.h"
#include "wizard_logic.h"
#include "display.h"
#include "tv_mode.h"
#include "input_gate.h"

#include <cstring>

/* ---- Plan 4 shell helpers (bell notes for the updater, the launch tips card, the idle throttle) ---- */
namespace {

/* One-time bell entries about updates: "Update ready: installs when you finish playing" while an update waits for a game to close, and
 * what the startup pass found (an interrupted update repaired, the last installer's failure). Bell only: never a popup, never a toast
 * over a game. The bell history loads on the first group_tick(), so nothing is added before GRP.loaded. */
void update_bell_tick() {
    static std::vector<std::string> pending;
    static bool deferred_was = false, shot_done = false;
    std::vector<std::string> got;
    if (hubupdate::take_startup_notes(&got)) for (auto& n : got) pending.push_back(n);
    if (g_headless && !shot_done) {   // screenshot aid: HUB_SHOT_UPDATE_NOTES=ready,failed,postponed,rollback,interrupted
        if (const char* sn = std::getenv("HUB_SHOT_UPDATE_NOTES")) {
            shot_done = true;
            std::string all = sn, tok;
            for (size_t i = 0; i <= all.size(); ++i) {
                if (i < all.size() && all[i] != ',') { tok += all[i]; continue; }
                hubupdate::apply::Status st;
                st.version = "v1.30.0";
                if (tok == "ready") pending.push_back(hubupdate::kReadyBellText);
                else if (tok == "failed") { st.result = "failed"; st.reason = hubupdate::apply::plain_reason(32); pending.push_back(hubupdate::apply::status_line(st)); }
                else if (tok == "postponed") { st.result = "postponed"; pending.push_back(hubupdate::apply::status_line(st)); }
                else if (tok == "rollback") { st.result = "rollback_failed"; pending.push_back(hubupdate::apply::status_line(st)); }
                else if (tok == "interrupted") { st.result = "interrupted"; pending.push_back(hubupdate::apply::status_line(st)); }
                tok.clear();
            }
        }
    }
    if (!GRP.loaded) return;
    for (const std::string& n : pending) group_note(hubui::group::Kind::Info, "", "", "", "", "", n, 0);
    pending.clear();
    const bool d = hubupdate::update_deferred();
    if (d && !deferred_was) group_note(hubui::group::Kind::Info, "", "", "", "", "", hubupdate::kReadyBellText, 0);
    deferred_was = d;
}

/* The controller tips card for games RetroHub cannot draw into (Dolphin, PCSX2, Azahar, Cemu, mGBA, BattleShip), shown in the HUB'S OWN
 * window while the game is still starting and before the game's window exists. It used to be a topmost click-through window laid over
 * the running game (src/common/hint_popup.cpp); nothing is drawn over a game any more. The runners keep their own: retrorun draws it into
 * its frame, n64run into its own window. Hub-launched external games only (A.run.pad_exit), and only with the setting on. */
void draw_launch_tips(bool shot_force) {
    if (!A.st.start_hint && !shot_force) return;
    double el_ms = 0.0;
    if (shot_force) el_ms = 1500.0;   // HUB_SHOT_LAUNCH_TIPS: a frozen frame of the card for screenshots
    else {
        if (g_headless || !A.proc || !A.run.pad_exit) return;
        el_ms = (double)(GetTickCount64() - A.run.start_tick);
        if (el_ms > 20000.0) return;                         // an emulator that never shows a window: give up quietly
        static ULONGLONG checked_tick = 0; static bool game_window_up = false;
        const ULONGLONG now = GetTickCount64();
        if (now - checked_tick > 250) { checked_tick = now; game_window_up = running_game_hwnd() != nullptr; }
        if (game_window_up) return;                          // its window is up: the hub is behind it, nothing to show here
    }
    const float a = std::min(1.f, (float)(el_ms / (double)starthint::kFadeInMs)) * starthint::kMaxAlpha;   // fades in, then holds until the game's window covers the hub
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const std::vector<starthint::Row> rows = starthint::rows(starthint::Mode::Pad);
    ImFont* f = ui::F.body;
    const float fs = f->FontSize, pad = ui::sp(1.6f), lh = fs * 1.5f;
    float w = 0.f;
    std::vector<std::string> lines;
    for (const auto& r : rows) { lines.push_back(starthint::plain(r)); w = std::max(w, f->CalcTextSizeA(fs, FLT_MAX, 0.f, lines.back().c_str()).x); }
    const float title_h = ui::F.bold->FontSize * 1.4f;
    const float h = pad * 2 + title_h + lh * (float)lines.size();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float cw = w + pad * 2;   // bottom centre, above the footer hints (the sidebar's profile chip stays uncovered)
    const ImVec2 p0(vp->Pos.x + (vp->Size.x - cw) * 0.5f, vp->Pos.y + vp->Size.y - ui::sp(8.f) - h), p1(p0.x + cw, p0.y + h);
    dl->AddRectFilled(p0, p1, ui::scale_alpha(ui::PANEL2, a), ui::sp(1.5f));
    dl->AddRect(p0, p1, ui::scale_alpha(ui::HAIRLINE, a), ui::sp(1.5f), 0, 1.f);
    dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(p0.x + pad, p0.y + pad), ui::scale_alpha(ui::TEXT, a), "Controller tips");
    for (size_t i = 0; i < lines.size(); ++i)
        dl->AddText(f, fs, ImVec2(p0.x + pad, p0.y + pad + title_h + lh * (float)i), ui::scale_alpha(ui::TEXT2, a), lines[i].c_str());
}

/* An update may not be handed to the installer while a game runs OR is starting (the start animation plays before the game's process
 * exists, and quitting then would drop the launch). */
bool update_hold() { return session_active() || bootanim::active(); }

/* FNV-1a over the whole draw data: equal hashes on two consecutive frames = nothing on screen changed (idle throttle). */
unsigned long long hash_draw_data(const ImDrawData* dd) {
    unsigned long long h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const unsigned char* b = (const unsigned char*)p;
        size_t i = 0;
        for (; i + 8 <= n; i += 8) { unsigned long long v; std::memcpy(&v, b + i, 8); h = (h ^ v) * 1099511628211ull; }
        for (; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    if (!dd) return h;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* l = dd->CmdLists[n];
        mix(l->VtxBuffer.Data, (size_t)l->VtxBuffer.Size * sizeof(ImDrawVert));
        mix(l->IdxBuffer.Data, (size_t)l->IdxBuffer.Size * sizeof(ImDrawIdx));
        mix(l->CmdBuffer.Data, (size_t)l->CmdBuffer.Size * sizeof(ImDrawCmd));
    }
    return h;
}

}  // namespace

static int hub_main();

/* Last-resort guard: an exception nobody caught (for example a std::filesystem path conversion) is written to the log and the
 * process leaves with a normal exit code, instead of std::terminate -> abort -> a silent 0xC0000409. */
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    try {
        return hub_main();
    } catch (const std::exception& e) {
        hublog::error("fatal: uncaught exception in the main thread: %s", e.what());
    } catch (...) {
        hublog::error("fatal: uncaught unknown exception in the main thread");
    }
    std::fprintf(stderr, "RetroHub: stopped by an unexpected error (see hub.log)\n");
    hublog::shutdown();
    return hubupdate::apply::kStartFailExit;
}

static int hub_main() {
    { int rc; if (hubupdate::apply::try_apply_mode(&rc)) return rc; }   // --apply-update: window-less updater mode
    const bool autotest = autotest_active();   // QA room driver: isolated instance, no shell/registry/update side effects
    if (!autotest) shell_set_app_id();
    const std::string shot_path_store = winutil::getenv_utf8("HUB_SCREENSHOT");   // UTF-8: std::getenv would hand back ANSI bytes (a non-ANSI folder name turns into '?')
    const char* shot_path = std::getenv("HUB_SCREENSHOT") ? shot_path_store.c_str() : nullptr;
    // Single instance + command-line actions: `retrohub://join/<CODE>`, `--join <CODE>`, `--play <game key>`.
    std::wstring cmd_join, cmd_play;
    const bool has_join = invite_parse_command_line(GetCommandLineW(), &cmd_join);
    const bool has_play = invite_parse_play(GetCommandLineW(), &cmd_play);
    std::wstring cmd_acct;   // retrohub://invite/XXXX-XXXX or --invite XXXX-XXXX: an account invite code for the waiting screen
    const bool has_acct = invite_parse_account_command_line(GetCommandLineW(), &cmd_acct);
    std::wstring cmd_night;   // retrohub://night/<id>: a game night reminder toast was clicked
    const bool has_night = invite_parse_night_command_line(GetCommandLineW(), &cmd_night);
    std::string pending_night;
    std::unique_ptr<InviteInstance> instance;
    InviteServer invite_srv;
    if (!shot_path && !autotest) {
        instance.reset(new InviteInstance(INVITE_MUTEX_NAME));
        if (!instance->is_primary()) {   // already running: hand the request over (or just ask it to show itself) and leave
            bool sent = has_night ? invite_send_night(INVITE_PIPE_NAME, cmd_night) : has_acct ? invite_send_account(INVITE_PIPE_NAME, cmd_acct) : has_join ? invite_send(INVITE_PIPE_NAME, cmd_join) : has_play ? invite_send_play(INVITE_PIPE_NAME, cmd_play) : invite_send_show(INVITE_PIPE_NAME);
            if (sent) return 0;
        } else {
            invite_srv.start(INVITE_PIPE_NAME);
        }
    }
    hubui::LinkJoinQueue link_join;   // cold-start --join / retrohub:// plus links forwarded by later launches
    if (has_join && !shot_path) link_join.push(U(cmd_join));
    bool told_need_name = false;
    if (has_play && !shot_path) A.pending_play = U(cmd_play);
    if (has_acct && !shot_path) lockout_invite_link(U(cmd_acct));
    if (has_night && !shot_path) pending_night = U(cmd_night);
    if (shot_path) { SDL_setenv("SDL_VIDEODRIVER", "dummy", 1); g_headless = true; }
    if (qm_e2e_active()) qm_e2e_boot();   // HUB_QM_E2E (needs HUB_SCREENSHOT): only the test's virtual joystick exists
    SDL_SetMainReady();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");   // smooth downscaling of cover art
    const bool log_on = !shot_path && !autotest;   // QA drivers never write to the player's logs folder
    if (log_on) {   // hub.log + crash report (docs/ARCHITECTURE.md, "Logs and crash reports"); a crash note for the previous run is queued here
        hublog::init(hub::data_dir() + "logs\\", HUB_VERSION_STR);
        crashrep::install(hub::data_dir() + "logs\\", HUB_VERSION_STR, &hang_watchdog_current_phase);
        crash_report_ui_init(hub::data_dir() + "logs\\");
        hublog::info("startup: process began");
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "RetroHub: SDL_Init failed: %s\n", SDL_GetError());   // no MessageBox: it could land over a game
        hublog::error("startup: SDL_Init failed: %s", SDL_GetError());
        hublog::shutdown();
        return hubupdate::apply::kStartFailExit;
    }

    float ddpi = 96.f;
    SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr);
    S = std::max(1.0f, ddpi / 96.0f);   // provisional (display 0): the real scale is taken from the monitor the window opens on, below
    if (const char* sc = std::getenv("HUB_SCALE")) S = std::max(0.75f, (float)std::atof(sc));

    // settings
    A.settings_path = hub::data_dir() + "hub.json";
    A.st.server = hubnet::default_server();
    hubui::load_settings(A.settings_path, &A.st);
    hublog::info("startup: settings loaded (+%llu ms)", hublog::since_start_ms());
    {   // D2: Reduce motion's first-run default follows Windows "Show animations"; after that it is the player's own choice
        bool key_saved = false;
        { std::ifstream hin(std::filesystem::path(W(A.settings_path)), std::ios::binary); std::stringstream hs; hs << hin.rdbuf(); key_saved = hs.str().find("\"reduce_motion\"") != std::string::npos; }
        if (!g_headless) {
            const bool want = hubdisplay::initial_reduce_motion(key_saved, A.st.reduce_motion, hubdisplay::os_animations_on());
            if (want != A.st.reduce_motion || !key_saved) { A.st.reduce_motion = want; A.dirty = true; }
        }
        if (std::getenv("HUB_REDUCE_MOTION")) A.st.reduce_motion = true;
        if (g_headless) if (const char* us = std::getenv("HUB_UI_SCALE")) A.st.ui_scale = hubdisplay::clamp_ui_scale(std::atoi(us));   // screenshots: UI size in percent
        ui::reduce_motion = A.st.reduce_motion;
    }
    const std::string installer_hand = hub::data_dir() + "installer_games_dir.txt";
    bool installer_prefill_kept = false;   // the hand-off file stays until the first-run wizard is finished or skipped (see below)
    {   // the installer's chosen games folder (written by a FRESH install only, never by an upgrade): merged into rom_folders
        std::ifstream in(std::filesystem::path(W(installer_hand)), std::ios::binary);
        if (in) {
            std::stringstream ss; ss << in.rdbuf(); in.close();
            const std::string text = ss.str();
            hubui::Settings probe;
            const bool usable = hubui::merge_installer_games_dir(&probe, text);   // does the file name a real folder at all?
            const bool changed = hubui::merge_installer_games_dir(&A.st, text);
            /* H4: the pre-filled folder must not make a brand-new install look "already set up". Decide BEFORE it counts: a fresh
             * install (nothing finished, no folders of the player's own) always runs the setup wizard, with the folder as a pre-filled
             * answer in its Games step. The file is the marker that survives a restart mid-wizard. */
            const bool fresh = hubui::wiz::fresh_install(A.st.setup_done, A.st.find_games_done, A.st.rom_folders.size(), usable);
            const bool saved = !changed || hubui::save_settings(A.settings_path, A.st);   // keep the file if saving failed: retry next start
            if (usable && fresh) { hubui::wiz::set_installer_prefill(true); installer_prefill_kept = true; }
            else if (saved) { std::error_code ec; std::filesystem::remove(std::filesystem::path(W(installer_hand)), ec); }
        }
    }
    if (const char* pg = std::getenv("HUB_START_PAGE")) {   // screenshot/testing aid: 0 Library, 1 Online, 2 Saves, 3 Controllers, 4 Settings, 5 Updates (6 = Updates too: the old What's new page), 7 Discover
        int n = std::atoi(pg);
        static const Page order[] = {Page::Library, Page::Online, Page::Saves, Page::Controllers, Page::Settings, Page::About, Page::About, Page::Discover};   // the numbers predate Discover; 7 = Discover
        if (n >= 0 && n <= 7) A.page = order[n];
    }
    if (const char* sg = std::getenv("HUB_SELECT_GAME")) A.env_select = sg;   // preselect a library game key (screenshots)
    if (const char* ss = std::getenv("HUB_START_SYS")) A.sys_filter = ss;   // screenshots: console filter for All Games (n64 gc wiiu 3ds nds gba gbc gb)
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
    if (g_headless) {   // screenshot mode never touches a real vault: HUB_SHOT_VAULT=<dir>, else a scratch dir under %TEMP%
        const std::string sv = winutil::getenv_utf8("HUB_SHOT_VAULT");   // UTF-8 (std::getenv is ANSI: u8path would throw on an accented folder)
        std::string dir = !sv.empty() ? sv : (std::filesystem::temp_directory_path() / "retrohub-shot-vault").u8string();
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
        hub::saves::set_vault_dir(dir);
        hub::library_set_data_dir(dir);   // library.json / library_cache.json (play history, fake or not) live in the scratch dir too: a headless run never writes <exe>\userdata\library.json
    }
    gs_load();
    if (!shot_path) {   // favorites follow the account: mirror them into <vault>/_account/prefs.json and tell the cloud engine on change
        hub::prefs::set_vault_root(hub::saves::vault_dir());
        hub::prefs::set_change_hook([] { hub::saves::cloud_account_prefs_changed(); });
        hub::prefs::seed_from_library();
    }

    int win_w = 0, win_h = 0;
    /* D1: open where the player left the window (same monitor, position, size, maximized), or centred on the main monitor when that
     * monitor is gone or the rect is no longer on screen. Start-up only: nothing here moves a window that is already up. */
    int win_x = SDL_WINDOWPOS_CENTERED, win_y = SDL_WINDOWPOS_CENTERED;
    bool start_max = false;
    float dpi_s = S;
    if (!shot_path) {
        const std::vector<hubdisplay::Monitor> mons = hubdisplay::monitors();
        const hubdisplay::Monitor* cand = hubdisplay::find_monitor(mons, A.st.win_monitor);
        if (!cand) cand = hubdisplay::primary_monitor(mons);
        if (cand) dpi_s = hubdisplay::dpi_scale_for(cand);
        if (const char* sc = std::getenv("HUB_SCALE")) dpi_s = std::max(0.75f, (float)std::atof(sc));
        S = hubdisplay::effective_scale(dpi_s, A.st.ui_scale);
        int dw = (int)(1280 * S), dh = (int)(780 * S);
        SDL_Rect ub;
        if (SDL_GetDisplayUsableBounds(0, &ub) == 0) { dw = std::min(dw, (int)(ub.w * 0.94f)); dh = std::min(dh, (int)(ub.h * 0.94f)); }
        hubdisplay::Saved sv;
        sv.rect = hubdisplay::Rect{A.st.win_x, A.st.win_y, A.st.win_w, A.st.win_h};
        sv.maximized = A.st.win_max;
        sv.monitor = A.st.win_monitor;
        const hubdisplay::Placement pl = hubdisplay::plan_start(sv, A.st.remember_window, mons, dw, dh);
        win_w = dw; win_h = dh;
        if (pl.rect.w > 0 && !mons.empty()) {
            win_x = pl.rect.x; win_y = pl.rect.y; win_w = pl.rect.w; win_h = pl.rect.h;
            start_max = pl.maximized;
            if (const hubdisplay::Monitor* pm = hubdisplay::find_monitor(mons, pl.monitor)) {
                if (!std::getenv("HUB_SCALE")) dpi_s = hubdisplay::dpi_scale_for(pm);
                S = hubdisplay::effective_scale(dpi_s, A.st.ui_scale);
            }
        }
    } else {
        S = hubdisplay::effective_scale(S, A.st.ui_scale);
        win_w = (int)(1280 * S);
        win_h = (int)(780 * S);
        if (const char* sz = std::getenv("HUB_SHOT_SIZE")) std::sscanf(sz, "%dx%d", &win_w, &win_h);
    }
    hubdisplay::set_dpi_scale(dpi_s);
    /* Never open over a running game: `--background` (the update relaunch passes it) or a full-screen / busy app up front means the
     * window is shown minimised without activation instead (see popup_guard.h). */
    const bool quiet_start = !shot_path && hubguard::start_quiet(wcsstr(GetCommandLineW(), L"--background") != nullptr, game_active());
    A.win = SDL_CreateWindow("RetroHub", win_x, win_y, win_w, win_h,
                             SDL_WINDOW_RESIZABLE | ((shot_path || quiet_start) ? 0 : SDL_WINDOW_SHOWN));
    if (A.win && start_max && !quiet_start) SDL_MaximizeWindow(A.win);   // the window was just created for the player (never while minimised for a game)
    SDL_Renderer* ren = A.win ? SDL_CreateRenderer(A.win, -1, shot_path ? SDL_RENDERER_SOFTWARE : (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)) : nullptr;
    if (!A.win || !ren) {
        std::fprintf(stderr, "RetroHub: window/renderer failed: %s\n", SDL_GetError());   // no MessageBox: it could land over a game
        hublog::error("startup: window/renderer failed: %s", SDL_GetError());
        hublog::shutdown();
        return hubupdate::apply::kStartFailExit;   // the update applier treats this code (and real crashes) as "did not start"
    }
    {
        SDL_RendererInfo ri;
        hublog::info("startup: window + renderer ready (+%llu ms), renderer %s, %dx%d", hublog::since_start_ms(), SDL_GetRendererInfo(ren, &ri) == 0 && ri.name ? ri.name : "?", win_w, win_h);
    }
    if (!shot_path) { hubdisplay::seed_monitor(A.win); hubdisplay::apply_min_size(A.win, S); }   // clamped to the work area (L3); headless shots keep the size they ask for
    S = tvmode::init(A.win, g_headless, S, hubtv::has_tv_flag(GetCommandLineW()) || std::getenv("HUB_TV") != nullptr, A.st.tv_start, win_h);   // TV mode scale (fonts are built next)
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(A.win, &wm)) A.hwnd = wm.info.win.window;
    if (quiet_start) hub_show_noactivate();
    if (!shot_path && !autotest) {
        shell_set_window_icon(A.win);
        shell_ensure_start_menu_shortcut();
        shell_ensure_url_protocol();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavEscapeClearFocusItem = false;   // B / Esc must never drop the focus highlight (the app handles "back" itself)
    io.IniFilename = nullptr;
    ui::setup_fonts(S);
    ui::setup_theme(S);
    theme_init();
    ImGui_ImplSDL2_InitForSDLRenderer(A.win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoAll);   // any connected pad drives the UI, hot-plug included
    hubart::init(ren, hub::data_dir() + "art\\");
    lib_fake_art_init(ren);
    qm_init(ren);
    hublog::info("startup: UI ready (+%llu ms)", hublog::since_start_ms());
    if (log_on && crashrep::reassert()) hublog::warn("another module replaced the crash filter; ours is back in front");   // overlay / driver DLLs are loaded by now
    hubart::set_enabled(A.st.download_art);
    hub::rom_cache_set_limit((uint64_t)A.st.rom_cache_limit_gb << 30);

    if (shot_path) shot_init_ui();
    const bool had_account = !g_headless && hubnet::identity_exists(hub::data_dir(), hub::saves::vault_dir());   // BEFORE fr_start() creates one: tells an upgrading friend from a new install
    fr_start();
    lockout_init(had_account, FR.started && !FR.fake ? FR.cli.my_uid() : std::string());

    refresh_pads();
    if (qm_e2e_active()) qm_e2e_start();
    start_scan();
    cloud_boot();  // cloud saves v2: automatic for every account (startup delta + retry queue run in the background)
    auto_sync();
    SH.init(shot_path);
    hubupdate::set_game_probe(&game_active);
    hubupdate::set_session_probe(&update_hold);      // an update downloads during a game but installs only once no session is running or starting
    hubtoast::set_game_probe(&game_active);          // the one gate for Windows toasts: never while a game runs
    if (!autotest && (!shot_path || SH.update) && !hubupdate::launch_check_disabled()) hubupdate::check_async(false);
    if (!autotest && !shot_path) hubupdate::startup_pass_async();   // repairs an interrupted update; the one-time bell notes come back via take_startup_notes()

    {   // UI hang watchdog: a stalled UI thread leaves userdata\logs\hang-<time>.txt (breadcrumb phases). HUB_HANG_SECS overrides the 5 s.
        const char* hs = std::getenv("HUB_HANG_SECS");
        hang_watchdog_start(hub::data_dir() + "logs\\", hs ? std::max(1.0, std::atof(hs)) : 5.0);
    }
    hublog::info("startup: services started, entering main loop (+%llu ms), %d games", hublog::since_start_ms(), (int)A.lib.size());
    bool running = true;
    bool log_game_on = false;   // hub.log: a hub-launched game process starting / ending
    ULONGLONG log_game_t0 = 0;
    int hang_test_frame = 0;   // HUB_HANG_TEST: main-loop iterations (ImGui frames may already be past 30 after the boot screen)

    /* Idle throttle (idle_logic.h): while the hub is in the background the loop keeps pumping every poller at its old cadence but draws a
     * frame only when something needs one. On by default for a normal run; off for the QA drivers (headless shots, RETROHUB_AUTOTEST, the
     * quick-menu e2e). QA switches: HUB_IDLE_THROTTLE=0 (off everywhere: the "before" of a measurement) / =1 (on even for a QA driver),
     * HUB_IDLE_SIM=bg (a headless run pretends to be behind a game). HUB_FPS_REPORT=<file> writes frames presented / loop iterations per
     * second and the CPU used at exit; with it, HUB_EXIT_AFTER=<seconds> ends the run (so a stray variable can never close a real session). */
    const char* idle_env = std::getenv("HUB_IDLE_THROTTLE");
    const char* idle_sim = std::getenv("HUB_IDLE_SIM");
    const std::string idle_sim_s = idle_sim ? idle_sim : "";
    const bool idle_sim_bg = idle_sim_s.find("bg") != std::string::npos;           // HUB_IDLE_SIM=bg: a headless run pretends to be behind a game
    const bool idle_sim_noraster = idle_sim_s.find("noraster") != std::string::npos; // HUB_IDLE_SIM=...,noraster: skip the SOFTWARE rasteriser (it costs ~110 ms a frame in the dummy-driver harness) so the numbers show the hub's own per-frame work and the loop pacing, as a GPU build would
    const bool idle_off = idle_env && idle_env[0] == '0', idle_force = idle_env && idle_env[0] == '1';
    const bool idle_on = !idle_off && !qm_e2e_active() && (idle_force || idle_sim_bg || (!autotest && !g_headless));
    const char* fps_report = std::getenv("HUB_FPS_REPORT");
    const double exit_after_s = (fps_report && *fps_report && std::getenv("HUB_EXIT_AFTER")) ? std::atof(std::getenv("HUB_EXIT_AFTER")) : 0.0;
    unsigned long long last_render_ms = 0, last_activity_ms = 0, prev_frame_hash = 0;
    unsigned long long n_loops = 0, n_presented = 0, n_waits = 0;
    LARGE_INTEGER qpc_f{}; QueryPerformanceFrequency(&qpc_f);
    auto qpc_ms = [&]() { LARGE_INTEGER i; QueryPerformanceCounter(&i); return (double)i.QuadPart * 1000.0 / (double)qpc_f.QuadPart; };
    double ms_pump = 0, ms_ui = 0, ms_raster = 0, ms_present = 0, q_iter = 0, q_ui = 0, q_raster = 0, q_present = 0;   // HUB_FPS_REPORT: where a frame's time goes
    std::vector<double> frame_ms;   // HUB_FPS_REPORT: whole-iteration time of every drawn frame (median / worst in the report)
    const ULONGLONG run_t0 = GetTickCount64();
    auto cpu_seconds = []() {   // CPU the process really used (kernel + user): independent of how busy the rest of the PC is
        FILETIME ct, et, kt, ut;
        if (!GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) return 0.0;
        auto to_s = [](const FILETIME& f) { return (double)(((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7; };
        return to_s(kt) + to_s(ut);
    };
    bool warm_taken = false;   // HUB_FPS_REPORT: the first 3 s (fonts, library, textures) are start-up, not steady state
    double warm_cpu = 0, warm_secs = 0;
    unsigned long long warm_presented = 0, warm_loops = 0;
    bool prefill_cleanup_pending = installer_prefill_kept;

    while (running) {
        const ULONGLONG iter_t0 = GetTickCount64();
        bool input_this_iter = false, dirty_this_iter = false;
        ++n_loops;
        if (fps_report) {
            q_iter = qpc_ms();
            if (!warm_taken && iter_t0 - run_t0 >= 3000) { warm_taken = true; warm_cpu = cpu_seconds(); warm_secs = (double)(iter_t0 - run_t0) / 1000.0; warm_presented = n_presented; warm_loops = n_loops - 1; }
        }
        if (exit_after_s > 0.0 && (double)(iter_t0 - run_t0) / 1000.0 >= exit_after_s) running = false;   // QA measurement runs only (see above)
        hang_watchdog_tick();
        if ((A.proc != nullptr) != log_game_on) {
            log_game_on = A.proc != nullptr;
            if (log_game_on) { log_game_t0 = iter_t0; hublog::info("game process started: %s (emulator kind %d)", A.run.title.c_str(), A.run.emu_kind); }
            else hublog::info("game process ended after %llu s", (unsigned long long)((iter_t0 - log_game_t0) / 1000));   // the exit code is logged by launch.cpp's poll_process once it adopts hublog
        }
        if (g_headless && std::getenv("HUB_HANG_TEST") && ++hang_test_frame == 30) hang_watchdog_phase("hang_test"), Sleep((DWORD)(std::atof(std::getenv("HUB_HANG_TEST")) * 1000));   // QA: simulate a hang
        hang_watchdog_phase("events");
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            qm_pad_event(e);   // the quick menu's own controls: button-down taps, whatever has the focus
            if (e.type == SDL_MOUSEWHEEL && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
                float wx = e.wheel.preciseX, wy = e.wheel.preciseY;
                if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { wx = -wx; wy = -wy; }
                if (route_wheel(io.MousePos.x, io.MousePos.y, wx, wy)) { input_this_iter = true; continue; }   // Home: vertical = eased page scroll, horizontal / Shift = the row under the pointer
            }
            if (osk::hub_input_blocked() && (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP || e.type == SDL_TEXTINPUT || e.type == SDL_TEXTEDITING)) continue;   // hub unfocused: no key / text event may reach the UI (quit, window, drop, device events still pass)
            if (osk::hub_pad_blocked() && (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP || e.type == SDL_CONTROLLERAXISMOTION ||
                e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP || e.type == SDL_JOYAXISMOTION || e.type == SDL_JOYHATMOTION)) continue;   // unfocused or a game runs: the pad is not the hub's (the open quick menu keeps its pad events)
            if (attract_swallow_event(e)) continue;   // attract mode: this event only wakes the hub
            input_this_iter = true;                   // only events that reach the UI count (a pad's axis noise behind a game is dropped above)
            ImGui_ImplSDL2_ProcessEvent(&e);
            tvmode::handle_event(e, io.WantTextInput);
            hubdisplay::note_event(e);
            if (e.type == SDL_DROPFILE && e.drop.file) { add_games_drop(e.drop.file); SDL_free(e.drop.file); }
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
            if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) refresh_pads();
            // which device was used last decides whether the footer shows controller glyphs or key caps
            if (e.type == SDL_CONTROLLERBUTTONDOWN || (e.type == SDL_CONTROLLERAXISMOTION && std::abs((int)e.caxis.value) > 16000)) {
                if (!A.pad_mode) A.pad_first = true;   // keyboard/mouse -> pad: make sure there is a focus to move
                A.pad_mode = true;
            }
            if (e.type == SDL_KEYDOWN || e.type == SDL_TEXTINPUT) osk::note_keyboard();
            if (e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEMOTION) A.pad_mode = false;
        }
        hang_watchdog_phase("hubart::pump");
        hubart::pump();
        qm_textures_begin_frame(); state_textures_begin_frame(); challenge_textures_begin_frame();   // before any draw: only now are last frame's evicted textures freed
        if (!(shot_path && std::getenv("HUB_SHOT_SKELETON")) && A.scan_ready.exchange(false)) {
            if (A.scan_th.joinable()) A.scan_th.join();
            A.scanning = false;
            hang_watchdog_phase("apply_scan");
            apply_scan();
            dirty_this_iter = true;   // the library changed: draw it
        }
        hang_watchdog_phase("poll_process");
        poll_process();
        hang_watchdog_phase("tvmode::tick");
        tvmode::tick(A.win, A.proc != nullptr);
        hubdisplay::tick(A.win, false, tvmode::active());   // remember the window once a move / resize has settled
        if (const float ds = hubdisplay::take_display_changed(A.win)) { hubdisplay::set_dpi_scale(ds); tvmode::set_base_scale(hubdisplay::effective_scale(ds, A.st.ui_scale)); }   // dragged to a monitor with another scale
        hang_watchdog_phase("poll_np3ds");
        poll_np3ds();
        hang_watchdog_phase("poll_rplay");
        poll_rplay();
        hang_watchdog_phase("kt_tick");
        kt_tick();
        hang_watchdog_phase("poll_job");
        poll_job();
        hang_watchdog_phase("cloud_gate_tick");
        cloud_gate_tick();
        gs_cloud_sync_tick();
        hang_watchdog_phase("cloud_bg_poll");
        cloud_bg_poll();
        hang_watchdog_phase("run_pending_sync");
        run_pending_sync();
        hang_watchdog_phase("poll_emulators");
        poll_emulators();
        hang_watchdog_phase("poll_gamepack");
        poll_gamepack();
        hang_watchdog_phase("lobby_tick");
        lobby_tick();
        hang_watchdog_phase("spectate_tick");
        spectate_tick();
        hang_watchdog_phase("dropin_tick");
        dropin_tick();
        hang_watchdog_phase("autotest_tick");
        autotest_tick();
        hang_watchdog_phase("fr_tick");
        fr_tick();
        lockout_tick();
        icon_refresh_tick();
        hang_watchdog_phase("ghost_tick");
        ghost_tick();
        group_tick();
        update_bell_tick();   // after group_tick(): the bell history is loaded by then
        hubupdate::tick();    // hands a downloaded update to the installer once no game is running
        userflow_tick();
        {   // requests from a second launch (invite links, --play, "show yourself") and from our own command line
            std::wstring w;
            while (invite_srv.take(&w)) { if (launch_blocked("invite link")) continue; link_join.push(U(w)); told_need_name = false; }
            while (invite_srv.take_account(&w)) { lockout_invite_link(U(w)); hub_surface(); }   // an account invite link: fills the waiting screen's code field (no-op while a game runs: nothing is raised)
            if (app_locked()) { link_join = hubui::LinkJoinQueue(); A.pending_play.clear(); }   // a locked account joins and plays nothing, however the request arrived
            const bool name_set = !A.st.name.empty();
            if (link_join.needs_name(name_set) && !told_need_name) {
                told_need_name = true;
                settings_open(hubui::ST_Account);
                set_notice("Pick a player name to join your friend's room. You will join right after.", 20.0);
            }
            std::string jc;
            if (link_join.take(name_set, &jc)) {
                fr_join_lobby(jc, "", "", "");
                hub_surface();   // no-op while a game runs: the join is queued and starts after the game; nothing is shown or flashed
            }
            while (invite_srv.take_night(&w)) { if (!launch_blocked("game night link")) pending_night = U(w); }
            if (app_locked()) pending_night.clear();
            if (!pending_night.empty() && !A.scanning) { gn_open_night(pending_night); pending_night.clear(); }   // the card / bell entry; nothing is raised
            while (invite_srv.take_play(&w)) { if (!launch_blocked("--play")) A.pending_play = U(w); }
            if (invite_srv.take_show()) hub_surface();   // a game is running: never pull RetroHub in front of it (refused inside)
            if (!A.pending_play.empty() && !A.scanning) {   // library is loaded: start the favorite (offline)
                std::string key = A.pending_play;
                A.pending_play.clear();
                if (const hub::GameEntry* g = find_entry(key)) { A.sel_game = key; hub::GameEntry copy = *g; play_entry_anim(copy, true); }
                else set_notice("That game is not in your library any more.");
            }
        }
        /* The installer replaces our files and relaunches us. It was started only when no session was running (hubupdate::tick); if one
         * started in the second since, stay: quitting now would leave the installer waiting on a game. */
        if (hubupdate::installer_started() && !update_hold()) running = false;
        static double settings_retry_at = 0;   // a failed save stays dirty and is retried every few seconds, not every frame
        if (A.dirty && !g_headless && !ImGui::IsAnyItemActive() && ImGui::GetTime() >= settings_retry_at) {
            if (hubui::save_settings(A.settings_path, A.st)) A.dirty = false;
            else settings_retry_at = ImGui::GetTime() + 5.0;
        }
        if (prefill_cleanup_pending && A.st.setup_done) {   // the first-run wizard was finished or skipped: the installer's hand-off file has done its job
            prefill_cleanup_pending = false;
            hubui::wiz::set_installer_prefill(false);
            std::error_code ec; std::filesystem::remove(std::filesystem::path(W(installer_hand)), ec);
        }

        if (SDL_GetWindowFlags(A.win) & SDL_WINDOW_MINIMIZED) { hubupdate::apply::note_boot_ok(); SDL_Delay(50); continue; }   // up, but minimised behind a game: no frame will be presented, so this counts as started

        bool bg_this_iter = false;
        if (idle_on) {   // idle throttle: pump everything every iteration, draw only when something needs a frame (idle_logic.h)
            const Uint32 wf = SDL_GetWindowFlags(A.win);
            hubidle::Inputs in;
            in.background = idle_sim_bg ? true : hubidle::is_background((wf & SDL_WINDOW_INPUT_FOCUS) != 0, (wf & SDL_WINDOW_SHOWN) != 0 && !(wf & SDL_WINDOW_HIDDEN), session_active());
            in.busy = qm_is_open() || bootanim::active() || !A.toasts.empty() ||            // the View+Y menu, the start animation, toasts
                      GCO.active || gc_process_alive() || NP3.active() || NP3_pending ||    // Dolphin NetPlay / 3DS room drivers
                      rplay_session_alive() || kt_session_active();                          // remote play, Kanto Together
            in.input = input_this_iter;
            in.dirty = dirty_this_iter;
            in.now_ms = iter_t0;
            in.last_render_ms = last_render_ms;
            if (input_this_iter) last_activity_ms = iter_t0;
            in.last_activity_ms = last_activity_ms;
            bg_this_iter = in.background;
            const hubidle::Decision d = hubidle::decide(in);
            if (!d.render) {
                hang_watchdog_phase("idle_wait");
                SDL_WaitEventTimeout(nullptr, d.wait_ms);   // input wakes it at once; nothing is consumed
                if (const int pad = hubidle::pad_ms(GetTickCount64() - iter_t0)) SDL_Delay((Uint32)pad);   // noise events must not make the pumps spin
                ++n_waits;
                continue;
            }
        }

        if (fps_report) { q_ui = qpc_ms(); ms_pump += q_ui - q_iter; }
        osk::pre_frame(io);
        ui::reduce_motion = A.st.reduce_motion;
        tvmode::frame_begin(A.win, ren);
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        if (osk::hub_pad_blocked()) inputgate::purge_pad_presses(ImGui::GetCurrentContext());   // the backend polls the pad itself: a game owns it, drop the queued presses (releases stay)
        SH.pre_frame(io);
        if (osk::hub_input_blocked()) {   // also stop ImGui's own pad/keyboard nav so nothing is activated or typed
            if (osk::hub_pad_blocked()) io.ClearInputKeys();
            else for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; ++k) io.AddKeyEvent((ImGuiKey)k, false);   // quick menu open: pad stays, keyboard is dropped
            io.InputQueueCharacters.resize(0);
        }
        if (!g_headless && !(SDL_GetWindowFlags(A.win) & SDL_WINDOW_INPUT_FOCUS) && !qm_is_open()) {
            /* A game is running (or another window has focus): pad buttons belong to that, never to the hub behind it. */
            for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickRight; ++k) io.AddKeyAnalogEvent((ImGuiKey)k, false, 0.f);
        }
        {   // LT / RT = page up / page down in whatever list has the focus (ImGui moves the focus along with the scroll)
            static bool pg_up = false, pg_dn = false;
            static ImGuiKey nudge = ImGuiKey_None;   // one-shot d-pad press that lands the focus after a manual page scroll
            struct Pend { int dir = 0, wait = 0, refocus = 0; ImGuiID nav = 0, wid = 0; float scroll = 0; };
            static Pend pend;
            /* After the step-onto-screen nudge: if nothing focusable is on screen (e.g. the page ends in a plain table) ImGui lets the focus
             * escape to the top bar. Put it back where it was, on the page. */
            struct Back { int wait = 0; ImGuiWindow* win = nullptr; ImGuiID wid = 0, nav = 0, scope = 0; ImGuiNavLayer layer = ImGuiNavLayer_Main; ImRect rel; };
            static Back back;
            ImGuiContext& pg = *ImGui::GetCurrentContext();
            if (pg_up) { io.AddKeyEvent(ImGuiKey_PageUp, false); pg_up = false; }
            if (pg_dn) { io.AddKeyEvent(ImGuiKey_PageDown, false); pg_dn = false; }
            if (nudge != ImGuiKey_None) { io.AddKeyAnalogEvent(nudge, false, 0.f); nudge = ImGuiKey_None; }
            if (back.wait > 0 && --back.wait == 0) {
                ImGuiWindow* page = ImGui::FindWindowByID(back.wid);
                bool inside = false;
                for (ImGuiWindow* c = pg.NavWindow; c; c = c->ParentWindow) if (c == page) { inside = true; break; }
                if (page && back.win && pg.NavWindow && !inside && pg.WindowsById.GetVoidPtr(back.win->ID) == back.win) {
                    ImGui::FocusWindow(back.win);
                    ImGui::SetNavID(back.nav, back.layer, back.scope, back.rel);
                }
            }
            const bool up = A.pad_mode && ImGui::IsKeyPressed(ImGuiKey_GamepadL2, true), dn = A.pad_mode && ImGui::IsKeyPressed(ImGuiKey_GamepadR2, true);
            /* Library / friends panel: ImGui's own page move (it lands the focus on the item a page away). Every other page keeps
             * its items in nested panels, where that move jumps to the top bar: scroll by hand there and step the focus onto the screen. */
            const bool manual = A.page != Page::Library && !A.st.show_friends;
            if (up && !manual && !A.lib_pg) { io.AddKeyEvent(ImGuiKey_PageUp, true); pg_up = true; }   // A.lib_pg: libview_ui.cpp pages All Games itself (ImGui's page move lands on the selected bar below)
            if (dn && !manual && !A.lib_pg) { io.AddKeyEvent(ImGuiKey_PageDown, true); pg_dn = true; }
            auto scroller = [&](int dir) -> ImGuiWindow* {   // nearest scrollable window around the focus that still has room that way
                for (ImGuiWindow* w = pg.NavWindow; w; w = w->ParentWindow)
                    if (w->ScrollMax.y > 0.f && (dir > 0 ? w->Scroll.y < w->ScrollMax.y - 1.f : w->Scroll.y > 1.f)) return w;
                return nullptr;
            };
            if ((up || dn) && !A.lib_pg) {
                ImGuiWindow* w = scroller(up ? -1 : 1);
                pend = Pend{up ? -1 : 1, 3, 0, pg.NavId, w ? w->ID : 0, w ? w->Scroll.y : 0.f};
                if (manual) {
                    pend.wait = 0;
                    if (w && !ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
                        ImGui::SetScrollY(w, std::max(0.f, std::min(w->ScrollMax.y, w->Scroll.y + pend.dir * w->InnerRect.GetHeight() * 0.85f)));
                        pend.refocus = 3;
                    } else pend.dir = 0;
                }
            } else if (pend.dir && pend.wait > 0 && --pend.wait == 0) {
                /* ImGui's own page move found nothing to move to (the page's items sit in nested panels, or the focused panel is at its
                 * end): scroll the nearest scrollable window by hand, then step the focus onto what is now on screen. */
                ImGuiWindow* w = pend.wid ? ImGui::FindWindowByID(pend.wid) : nullptr;
                const bool stuck = pg.NavId == pend.nav && (!w || std::abs(w->Scroll.y - pend.scroll) < 1.f);
                if (stuck && !pg.NavWindowingTarget && !ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
                    ImGuiWindow* t = scroller(pend.dir);
                    if (t) {
                        ImGui::SetScrollY(t, std::max(0.f, std::min(t->ScrollMax.y, t->Scroll.y + pend.dir * t->InnerRect.GetHeight() * 0.85f)));
                        pend.wid = t->ID;
                        pend.refocus = 3;
                    }
                }
                if (!pend.refocus) pend.dir = 0;
            } else if (pend.refocus > 0 && --pend.refocus == 0) {
                ImGuiWindow* w = ImGui::FindWindowByID(pend.wid);
                if (w && pg.NavWindow) {
                    ImRect r = ImGui::WindowRectRelToAbs(pg.NavWindow, pg.NavWindow->NavRectRel[pg.NavLayer]);
                    const ImRect view = w->InnerRect;
                    if (r.Min.y < view.Min.y || r.Max.y > view.Max.y) {   // the focused item scrolled out of view: ImGui steps from the clamped rect to what is on screen
                        /* The old rect can lie far outside the view (e.g. above it, level with the top bar that floats over the page): stepping from
                         * there lands on the nearest thing to THAT spot, the top bar. Start the step from the edge of the view instead. */
                        const float edge = pend.dir > 0 ? view.Min.y : view.Max.y;
                        ImRect from(r.Min.x, edge, r.Max.x, edge);
                        pg.NavWindow->NavRectRel[pg.NavLayer] = ImGui::WindowRectAbsToRel(pg.NavWindow, from);
                        back = Back{3, pg.NavWindow, w->ID, pg.NavId, pg.NavFocusScopeId, pg.NavLayer, pg.NavWindow->NavRectRel[pg.NavLayer]};
                        nudge = pend.dir > 0 ? ImGuiKey_GamepadDpadDown : ImGuiKey_GamepadDpadUp;
                        io.AddKeyAnalogEvent(nudge, true, 1.f);
                    }
                }
                pend.dir = 0;
            }
        }
        qm_pre_newframe();
        ImGui::SetKeyOwner(ImGuiKey_GamepadFaceUp, 1);   // Y is a context-action button here: ImGui's own "Y activates the focused item" must not fire it
        hang_watchdog_phase("imgui_newframe");
        ImGui::NewFrame();
        {   // a held X/Menu would open ImGui's window switcher; this app has no use for it (X is a real button here)
            ImGuiContext& g = *ImGui::GetCurrentContext();
            g.NavWindowingTarget = g.NavWindowingTargetAnim = nullptr;
            g.NavWindowingToggleLayer = false;
            if (A.pad_mode && g.NavWindow) g.NavCursorVisible = true;   // pad users always see where the focus is (ImGui hides it after a mouse click)
            if (A.pad_first) {   // first pad input after using the mouse/keyboard: if nothing is focused, land on the page's default item
                A.pad_first = false;
                if (!g.NavId || !g.NavWindow) A.nav_seed = true;
            }
        }
        ImGui::PushFont(ui::F.body);
        hang_watchdog_phase("draw_frame");
        hang_watchdog_detail((int)A.page);
        draw_frame();
        hang_watchdog_phase("draw_popups");
        if (shot_path) bootart_shot_draw(ren);   // HUB_SHOT_BOOTART gallery
        if (!shot_path || SH.update) draw_update_popup();   // opens itself only when prompt_allowed()
        if (!shot_path || std::getenv("HUB_SHOT_CRASHNOTE")) draw_crash_note();   // opens itself only when prompt_allowed()
        if (log_on) draw_crash_report_note();   // "RetroHub closed unexpectedly last time": only when prompt_allowed() and no other popup is up
        draw_launch_tips(shot_path && std::getenv("HUB_SHOT_LAUNCH_TIPS"));   // controller tips for a game that is still starting (never over a game window)
        osk::draw_root();
        ImGui::PopFont();
        hang_watchdog_phase("render");
        if (fps_report) { q_raster = qpc_ms(); ms_ui += q_raster - q_ui; }
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 16, 17, 22, 255);
        SDL_RenderClear(ren);
        if (!idle_sim_noraster) ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        if (idle_on) {   // idle throttle: did anything on screen change since the previous frame? (keeps the capped fast cadence while it does)
            if (bg_this_iter) {
                const unsigned long long h = hash_draw_data(ImGui::GetDrawData());
                if (h != prev_frame_hash) last_activity_ms = GetTickCount64();
                prev_frame_hash = h;
            } else prev_frame_hash = 0;
            last_render_ms = GetTickCount64();
        }
        if (shot_path) {
            if (!SH.pending_shot.empty()) {   // shot:<name> from HUB_SHOT_PAD -> <stem>-<name>.bmp
                std::string sp_ = shot_path;
                size_t dot = sp_.rfind('.');
                save_screenshot(ren, (sp_.substr(0, dot == std::string::npos ? sp_.size() : dot) + "-" + SH.pending_shot + ".bmp").c_str());
                SH.pending_shot.clear();
            }
            if (std::string uf = userflow_take_shot(); !uf.empty()) {
                std::string sp_ = shot_path;
                size_t dot = sp_.rfind('.');
                save_screenshot(ren, (sp_.substr(0, dot == std::string::npos ? sp_.size() : dot) + "-" + uf + ".bmp").c_str());
            }
            if (qm_e2e_active()) { if (!qm_e2e_frame()) running = false; }
            else if (SH.fail) { running = false; }
            else if (SH.ready()) {
                save_screenshot(ren, shot_path);
                const char* pn[] = {"library", "discover", "online", "saves", "controllers", "settings", "about"};
                char line[512];
                std::snprintf(line, sizeof line, "SHOT_CAPTURED page=%s details=%s game=%s sel=%s", (int)A.page >= 0 && (int)A.page < 7 ? pn[(int)A.page] : "?",
                              g_details_shown.empty() ? "no" : "yes", g_details_shown.empty() ? "-" : g_details_shown.c_str(), A.sel_game.empty() ? "-" : A.sel_game.c_str());
                std::fprintf(stderr, "%s\n", line);
                shot_trace("captured", line + 14);
                running = false;
            }
            SDL_Delay(8);
        }
        hang_watchdog_phase("present");
        if (fps_report) { q_present = qpc_ms(); ms_raster += q_present - q_raster; }
        SDL_RenderPresent(ren);
        ++n_presented;
        hubupdate::apply::note_boot_ok();   // first frame is on screen: tells a watching update applier this version started (no-op without --boot-ok)
        if (fps_report) { const double q_end = qpc_ms(); ms_present += q_end - q_present; frame_ms.push_back(q_end - q_iter); }
    }

    if (fps_report && *fps_report) {   // QA: how many frames were really presented, and how often the pumps ran (docs/ARCHITECTURE.md, "Idle throttle")
        const double secs = std::max(0.001, (double)(GetTickCount64() - run_t0) / 1000.0);
        const double cpu = cpu_seconds();
        const double ssecs = warm_taken ? std::max(0.001, secs - warm_secs) : secs;   // steady state = after the first 3 s
        const double scpu = warm_taken ? cpu - warm_cpu : cpu;
        const unsigned long long spres = warm_taken ? n_presented - warm_presented : n_presented, sloops = warm_taken ? n_loops - warm_loops : n_loops;
        if (FILE* rf = std::fopen(fps_report, "wb")) {
            const double np = n_presented ? (double)n_presented : 1.0;
            std::sort(frame_ms.begin(), frame_ms.end());
            const double med = frame_ms.empty() ? 0.0 : frame_ms[frame_ms.size() / 2], worst = frame_ms.empty() ? 0.0 : frame_ms.back();
            std::fprintf(rf, "idle_throttle=%d total: secs=%.2f presented=%llu loops=%llu waits=%llu cpu_s=%.2f | steady (after 3 s): secs=%.2f presented_per_s=%.1f loops_per_s=%.1f cpu_pct=%.1f | "
                             "ms_per_frame: pump=%.1f ui=%.1f raster+delay=%.1f present=%.1f median_iteration=%.1f worst_iteration=%.1f\n",
                         idle_on ? 1 : 0, secs, n_presented, n_loops, n_waits, cpu, ssecs, (double)spres / ssecs, (double)sloops / ssecs, 100.0 * scpu / ssecs,
                         ms_pump / np, ms_ui / np, ms_raster / np, ms_present / np, med, worst);
            std::fclose(rf);
        }
    }

    // clean shutdown
    hublog::info("shutdown: main loop ended");
    hub::scan_request_cancel();   // the library scan (and a ROM-cache clear / trim) stops at its next check point, so the joins below return promptly
    hubdisplay::tick(A.win, true, tvmode::active());   // last window rect / monitor / maximized, before the window is hidden
    hang_watchdog_stop();   // before every shutdown join (updater startup thread, art worker): a slow join must not trip the watchdog on a normal exit
    hubupdate::shutdown();   // stop waiting for a game to close, drop the unused installer copy, join the startup pass
    SDL_HideWindow(A.win);   // the joins below can wait on a slow network: never leave a frozen-looking window up meanwhile
    dolphin_speed_stop();   // the Dolphin speed DSU server (if a GameCube game ran)
    find_games_shutdown();
    add_games_shutdown();
    gamepack_shutdown();
    if (A.dirty) hubui::save_settings(A.settings_path, A.st);
    invite_srv.stop();
    gn_shutdown();   // hand the game night reminders back to Windows (scheduled toasts) now that RetroHub is closing
    A.lobby.stop();
    FR.cli.stop();
    cloud_gate_cancel();   // the gate thread may be waiting on a background cloud job: stop it before the joins
    if (A.scan_th.joinable()) A.scan_th.join();
    settings_cache_shutdown();
    if (A.job.th.joinable()) A.job.th.join();
    if (A.cloud_gate.th.joinable()) A.cloud_gate.th.join();
    cloud_shutdown();
    hub::flush_library_state();
    if (A.proc) CloseHandle(A.proc);
    for (auto* p : A.pads) SDL_GameControllerClose(p);
    g_io_shutdown = true;
    hubart::shutdown();
    hubtoast::shutdown();   // the toast worker finishes its queue and joins (no toast can follow), before the log closes
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(A.win);
    SDL_Quit();
    CoUninitialize();
    hublog::shutdown();
    return SH.fail ? 2 : (qm_e2e_active() ? qm_e2e_exit_code() : 0);
}
