/* RetroHub.exe entry point. */
#include "app.h"

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    { int rc; if (hubupdate::apply::try_apply_mode(&rc)) return rc; }   // --apply-update: window-less updater mode
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

    if (shot_path) shot_init_ui();
    fr_start();

    refresh_pads();
    start_scan();
    auto_sync();   // pull in saves from other PCs at start
    SH.init(shot_path);
    if ((!shot_path || SH.update) && !hubupdate::launch_check_disabled()) hubupdate::check_async(false);

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
        SH.pre_frame(io);
        ImGui::NewFrame();
        ImGui::PushFont(ui::F.body);
        draw_frame();
        if (!shot_path || SH.update) draw_update_popup();
        ImGui::PopFont();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 16, 17, 22, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        if (shot_path) {
            if (SH.ready()) { save_screenshot(ren, shot_path); running = false; }
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
