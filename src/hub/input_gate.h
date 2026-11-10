// Input gate helpers shared by the hub loop and its tests (no SDL / app state).
#pragma once
struct ImGuiContext;

namespace inputgate {

/* Removes every queued gamepad key PRESS (a Key event with Down == true, key in [GamepadStart, GamepadRStickDown]) from the context's
 * input event queue. ImGui_ImplSDL2_NewFrame polls the pad itself every frame (NavEnableGamepad) and queues presses there, so dropping SDL
 * events is not enough while a game owns the pad. Releases are kept so a key held before blocking is released and never sticks.
 * Call right after ImGui_ImplSDL2_NewFrame while osk::hub_pad_blocked(). Returns how many events were removed. */
int purge_pad_presses(ImGuiContext* ctx);

/* A file / folder picker may open only in a hub window that has settled input focus (osk::hub_input_blocked(): focus + regain grace) while no game runs
 * (headless runs have neither and always may). */
inline bool picker_may_open(bool headless, bool input_blocked, bool game_running) { return headless || (!input_blocked && !game_running); }

}  // namespace inputgate
