#include "app.h"
#include "boot_anim.h"

/* Launch flow + settings glue for the game start animation (engine: boot_anim.cpp). Online launches (launch_online.cpp, rooms) never
 * pass through here, so online sync never waits on the animation. */

void boot_sync_options() {
    bootanim::Options& o = bootanim::options();
    o.enabled = A.st.boot_anim;
    o.sounds = A.st.boot_sounds;
    o.volume = std::max(0, std::min(100, A.st.boot_volume)) / 100.f;
    o.short_version = A.st.boot_short;
    o.skip_online = A.st.boot_skip_online;
}

/* User-initiated launch: play the animation (full from the details modal, short from the hero / quick paths), then `launch`
 * (default play_entry) runs exactly as before. */
void play_entry_anim(const hub::GameEntry& e, bool quick, std::function<void(const hub::GameEntry&)> launch) {
    if (!launch) launch = [](const hub::GameEntry& g) { play_entry(g); };
    boot_sync_options();
    const bool headless_skip = g_headless && !std::getenv("HUB_BOOTANIM_FORCE");
    if (headless_skip || A.proc || A.cloud_gate.phase != CloudGate::Idle || bootanim::active() || is_kanto(e) || e.system == hub::System::Unknown) {
        hub::GameEntry c = e;
        launch(c);   // play_entry reports "already running" etc. itself
        return;
    }
    const std::string key = e.key;
    hub::GameEntry copy = e;
    bootanim::start(e, [key, copy, launch]() {
        hub::GameEntry c = copy;
        if (const hub::GameEntry* g = find_entry(key)) c = *g;   // the library may have refreshed meanwhile
        A.sel_game = key;
        launch(c);
    }, /*online=*/A.in_room, quick, /*real_boot=*/!A.in_room && real_boot_will_use(e));
}

void boot_settings_ui() {
    settings_label("Game start animation");
    if (ImGui::Checkbox("Play the start animation", &A.st.boot_anim)) mark_dirty();
    text_wrapped_disabled("The game's cartridge or disc flies out of the shelf into its console before the game starts. "
                          "Press any button to skip it.");
    if (A.st.boot_anim) {
        if (ImGui::Checkbox("Startup sounds", &A.st.boot_sounds)) mark_dirty();
        if (A.st.boot_sounds) {
            ImGui::SetNextItemWidth(sp(30.f));
            if (ImGui::SliderInt("##bootvol", &A.st.boot_volume, 0, 100, "Volume %d%%")) mark_dirty();
        }
        if (ImGui::Checkbox("Short version (about 1 second)", &A.st.boot_short)) mark_dirty();
        if (ImGui::Checkbox("Skip in online games", &A.st.boot_skip_online)) mark_dirty();
        text_wrapped_disabled("Rooms start right away; nobody waits for an animation.");
    }
    real_boot_settings_ui();
}

/* Screenshot aid: HUB_SHOT_BOOTANIM=<system>:<t> freezes the animation at t seconds for the first game of that system. */
void boot_shot_tick() {
    static const char* env = std::getenv("HUB_SHOT_BOOTANIM");
    static bool done = false;
    if (!env || done || !g_headless) return;
    std::string v = env;
    auto c = v.find(':');
    hub::System sys = hub::system_from_short(v.substr(0, c));
    float t = c == std::string::npos ? 1.f : (float)std::atof(v.c_str() + c + 1);
    if (!A.scanned_once && !std::getenv("HUB_SHOT_NOWAIT")) return;
    done = true;
    hub::GameEntry e;
    e.system = sys;
    e.title = "Game Title";
    for (auto& g : A.lib) if (g.system == sys) { e = g; break; }
    boot_sync_options();
    bootanim::shot_freeze(sys, t);
    bootanim::start(e, []() {}, false, false);
}

void boot_draw_overlay() {
    boot_shot_tick();
    bootanim::draw();
}
