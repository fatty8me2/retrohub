/* RetroHub.exe entry point. */
#include "app.h"

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    { int rc; if (hubupdate::apply::try_apply_mode(&rc)) return rc; }   // --apply-update: window-less updater mode
    const bool autotest = autotest_active();   // QA room driver: isolated instance, no shell/registry/update side effects
    if (!autotest) shell_set_app_id();
    const char* shot_path = std::getenv("HUB_SCREENSHOT");
    // Single instance + command-line actions: `retrohub://join/<CODE>`, `--join <CODE>`, `--play <game key>`.
    std::wstring cmd_join, cmd_play;
    const bool has_join = invite_parse_command_line(GetCommandLineW(), &cmd_join);
    const bool has_play = invite_parse_play(GetCommandLineW(), &cmd_play);
    std::unique_ptr<InviteInstance> instance;
    InviteServer invite_srv;
    if (!shot_path && !autotest) {
        instance.reset(new InviteInstance(INVITE_MUTEX_NAME));
        if (!instance->is_primary()) {   // already running: hand the request over (or just ask it to show itself) and leave
            bool sent = has_join ? invite_send(INVITE_PIPE_NAME, cmd_join) : has_play ? invite_send_play(INVITE_PIPE_NAME, cmd_play) : invite_send_show(INVITE_PIPE_NAME);
            if (sent) return 0;
        } else {
            invite_srv.start(INVITE_PIPE_NAME);
        }
    }
    hubui::LinkJoinQueue link_join;   // cold-start --join / retrohub:// plus links forwarded by later launches
    if (has_join && !shot_path) link_join.push(U(cmd_join));
    bool told_need_name = false;
    if (has_play && !shot_path) A.pending_play = U(cmd_play);
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
    {   // the installer's chosen games folder: merged into rom_folders once, then the hand-off file is removed
        std::string hand = hub::data_dir() + "installer_games_dir.txt";
        std::ifstream in(std::filesystem::path(W(hand)), std::ios::binary);
        if (in) {
            std::stringstream ss; ss << in.rdbuf(); in.close();
            bool changed = hubui::merge_installer_games_dir(&A.st, ss.str());
            if (!changed || hubui::save_settings(A.settings_path, A.st)) {   // keep the hand-off file if saving failed: retry next start
                std::error_code ec; std::filesystem::remove(std::filesystem::path(W(hand)), ec);
            }
        }
    }
    if (const char* pg = std::getenv("HUB_START_PAGE")) {   // screenshot/testing aid: 0 Library, 1 Online, 2 Saves, 3 Controllers, 4 Settings, 5 About, 6 What's new
        int n = std::atoi(pg);
        if (n >= 0 && n <= 6) A.page = static_cast<Page>(n);
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
    gs_load();

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
    ImGui_ImplSDL2_InitForSDLRenderer(A.win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoAll);   // any connected pad drives the UI, hot-plug included
    hubart::init(ren, hub::data_dir() + "art\\");
    qm_init(ren);
    hubart::set_enabled(A.st.download_art);

    if (shot_path) shot_init_ui();
    fr_start();

    refresh_pads();
    start_scan();
    cloud_boot();  // cloud saves v2: automatic for every account (startup delta + retry queue run in the background)
    auto_sync();
    SH.init(shot_path);
    if (!autotest && (!shot_path || SH.update) && !hubupdate::launch_check_disabled()) hubupdate::check_async(false);

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
        hubart::pump();
        if (!(shot_path && std::getenv("HUB_SHOT_SKELETON")) && A.scan_ready.exchange(false)) {
            if (A.scan_th.joinable()) A.scan_th.join();
            A.scanning = false;
            apply_scan();
        }
        poll_process();
        poll_np3ds();
        kt_tick();
        poll_job();
        cloud_gate_tick();
        run_pending_sync();
        poll_emulators();
        lobby_tick();
        autotest_tick();
        fr_tick();
        group_tick();
        userflow_tick();
        {   // requests from a second launch (invite links, --play, "show yourself") and from our own command line
            std::wstring w;
            while (invite_srv.take(&w)) { link_join.push(U(w)); told_need_name = false; }
            const bool name_set = !A.st.name.empty();
            if (link_join.needs_name(name_set) && !told_need_name) {
                told_need_name = true;
                settings_open(hubui::ST_Account);
                set_notice("Pick a player name to join your friend's room. You will join right after.", 20.0);
            }
            std::string jc;
            if (link_join.take(name_set, &jc)) {
                fr_join_lobby(jc, "", "", "");
                if (A.proc) flash_taskbar();
                else if (A.win) { SDL_RestoreWindow(A.win); SDL_RaiseWindow(A.win); }
            }
            while (invite_srv.take_play(&w)) A.pending_play = U(w);
            if (invite_srv.take_show()) {
                if (A.proc) flash_taskbar();   // a game is running: never pull RetroHub in front of it
                else { SDL_RestoreWindow(A.win); SDL_RaiseWindow(A.win); }
            }
            if (!A.pending_play.empty() && !A.scanning) {   // library is loaded: start the favorite (offline)
                std::string key = A.pending_play;
                A.pending_play.clear();
                if (const hub::GameEntry* g = find_entry(key)) { A.sel_game = key; hub::GameEntry copy = *g; play_entry(copy); }
                else set_notice("That game is not in your library any more.");
            }
        }
        if (hubupdate::installer_started()) running = false;   // the installer replaces our files and relaunches us
        if (A.dirty && !g_headless && !ImGui::IsAnyItemActive()) {
            hubui::save_settings(A.settings_path, A.st);
            A.dirty = false;
        }

        if (SDL_GetWindowFlags(A.win) & SDL_WINDOW_MINIMIZED) { SDL_Delay(50); continue; }

        osk::pre_frame(io);
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        SH.pre_frame(io);
        if (!g_headless && (A.proc || !(SDL_GetWindowFlags(A.win) & SDL_WINDOW_INPUT_FOCUS))) {
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
            if (up && !manual) { io.AddKeyEvent(ImGuiKey_PageUp, true); pg_up = true; }
            if (dn && !manual) { io.AddKeyEvent(ImGuiKey_PageDown, true); pg_dn = true; }
            auto scroller = [&](int dir) -> ImGuiWindow* {   // nearest scrollable window around the focus that still has room that way
                for (ImGuiWindow* w = pg.NavWindow; w; w = w->ParentWindow)
                    if (w->ScrollMax.y > 0.f && (dir > 0 ? w->Scroll.y < w->ScrollMax.y - 1.f : w->Scroll.y > 1.f)) return w;
                return nullptr;
            };
            if (up || dn) {
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
        ImGui::SetKeyOwner(ImGuiKey_GamepadFaceUp, 1);   // Y is a context-action button here: ImGui's own "Y activates the focused item" must not fire it
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
        draw_frame();
        if (!shot_path || SH.update) draw_update_popup();
        osk::draw_root();
        ImGui::PopFont();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 16, 17, 22, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
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
            if (SH.ready()) { save_screenshot(ren, shot_path); running = false; }
            SDL_Delay(8);
        }
        SDL_RenderPresent(ren);
    }

    // clean shutdown
    find_games_shutdown();
    add_games_shutdown();
    if (A.dirty) hubui::save_settings(A.settings_path, A.st);
    invite_srv.stop();
    A.lobby.stop();
    FR.cli.stop();
    if (A.scan_th.joinable()) A.scan_th.join();
    if (A.job.th.joinable()) A.job.th.join();
    if (A.cloud_gate.th.joinable()) A.cloud_gate.th.join();
    cloud_shutdown();
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
