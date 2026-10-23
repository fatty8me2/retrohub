/* RetroHub.exe entry point. */
#include "app.h"

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
    if (const char* pg = std::getenv("HUB_START_PAGE")) {   // screenshot/testing aid: 0 Library, 1 Online, 2 Saves, 3 Controllers, 4 Settings, 5 About
        int n = std::atoi(pg);
        if (n >= 0 && n <= 5) A.page = static_cast<Page>(n);
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
                push_toast(hubui::host_changed_text(nm, !g_fake_other_host), ui::GOOD, 60.0, /*top=*/true);
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
        poll_np3ds();
        poll_job();
        run_pending_sync();
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
