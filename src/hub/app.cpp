#include "app.h"

App A;
bool g_headless = false;   // HUB_SCREENSHOT run: never launch games or touch the lobby

/* ============================ app state ============================ */









void push_toast(const std::string& s, ImU32 col, double secs, bool top) {
    double now = ImGui::GetTime();
    if (!A.toasts.empty() && A.toasts.back().text == s && A.toasts.back().until > now) {   // same message again: just extend it
        A.toasts.back().until = now + secs;
        return;
    }
    A.toasts.push_back({s, col, now, now + std::min(secs, 14.0), top});
    if (A.toasts.size() > 4) A.toasts.erase(A.toasts.begin());
}
void set_notice(const std::string& s, double secs) { autotest_log("notice: " + s); push_toast(s, ui::WARN, secs); }
void mark_dirty() { A.dirty = true; }

std::string server_url() {
    return A.st.server.empty() ? std::string(hubnet::default_server()) : A.st.server;
}
std::string smash_exe_path() {
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
std::string n64run_path() { return hub::exe_dir() + "n64run.exe"; }

