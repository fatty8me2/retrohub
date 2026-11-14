#include "app.h"

#include "hookshot.h"

/* The small prompt before Hookshot starts when Windows has no AV1 video decoder. Hookshot would open a top-most message box over the game
 * for that, and nothing may pop up over a running game, so RetroHub asks first, in its own window (docs/EMULATORS.md, Hookshot). */

namespace {

struct Av1Ask {
    bool open = false;
    bool focused = false;
    std::string key;
} ASK;

/* Play was pressed with "Plays with" set to Hookshot or Longshot, but that build is not on this PC. RetroHub used to start Lichii without a word. */
struct MissingAsk {
    bool open = false;
    bool focused = false;
    std::string key, name, why;
} MISS;

ULONGLONG g_lichii_once_at = 0;   // set with it: the choice expires after 20 s, so a launch that stopped at a prompt cannot leak into a later Play
std::string g_lichii_once;   // game key: the next launch_external for it skips Hookshot ("Play with Lichii this time")

}  // namespace

void hookshot_av1_ask(const hub::GameEntry& e) { ASK.open = true; ASK.key = e.key; ASK.focused = false; }
void hookshot_av1_ask_shot() { ASK.open = true; ASK.key.clear(); ASK.focused = false; }

void hookshot_missing_ask(const hub::GameEntry& e, int flavor, const std::string& why) {
    MISS.open = true; MISS.focused = false; MISS.key = e.key; MISS.why = why;
    MISS.name = hookshot::info((hookshot::Flavor)flavor).name;
}
void hookshot_missing_ask_shot(int flavor) { MISS.open = true; MISS.focused = false; MISS.key.clear(); MISS.why = "not built yet"; MISS.name = hookshot::info((hookshot::Flavor)flavor).name; }

/* Starts the game with Lichii for this one launch (the "Plays with" choice stays as it is). */
static void play_with_lichii_once(const std::string& key) {
    if (const hub::GameEntry* g = find_entry(key)) { hub::GameEntry copy = *g; g_lichii_once = copy.key; g_lichii_once_at = GetTickCount64(); play_entry(copy); if (A.cloud_gate.phase == CloudGate::Idle && !A.job.running) g_lichii_once.clear(); }   // cleared whatever play_entry did (it can return early), unless the launch was deferred (cloud check / save sync re-enters play_entry; the 20 s expiry covers that)
}

static void draw_missing_dialog() {
    if (!MISS.open) return;
    bool open = false;
    const std::string title = MISS.name + " is not built on this PC";
    emu_modal_begin(title.c_str(), &open);
    if (!open) return;
    if (ui::modal_back()) MISS.open = false;
    ImGui::TextWrapped("This game is set to play with %s, but %s is not ready here (%s).", MISS.name.c_str(), MISS.name.c_str(), MISS.why.c_str());
    ImGui::Dummy(ImVec2(0, 2 * S));
    text_wrapped_disabled("Build it from the game's page (About, at the bottom), or play with Lichii this time. To change it for good, use Settings > Plays with on the game's page.");
    ImGui::Dummy(ImVec2(0, 8 * S));
    if (!MISS.focused) { ImGui::SetKeyboardFocusHere(0); MISS.focused = true; }   // a pad lands on the first button
    if (button("Open the game's page", ImVec2(300 * S, 44 * S), true)) {
        MISS.open = false;
        if (find_entry(MISS.key)) { A.sel_game = MISS.key; A.show_details = true; }
    }
    if (button("Play with Lichii this time", ImVec2(300 * S, 44 * S))) { MISS.open = false; play_with_lichii_once(MISS.key); }
    if (button("Cancel", ImVec2(300 * S, 44 * S))) MISS.open = false;
    if (!MISS.open) { ImGui::CloseCurrentPopup(); MISS.focused = false; }
    ImGui::EndPopup();
}

/* The cloud check before a launch was cancelled (or timed out): that launch is off, so its "Lichii this time" must not reach a later Play. */
void hookshot_lichii_once_clear() { g_lichii_once.clear(); }

/* Every frame: while the cloud check for the launch is really pending (it can take longer than 20 s), the one-shot stays fresh, so the launch
 * that follows does not come back to the prompt. Once the check is over the 20 s run again. */
static void lichii_once_tick() {
    if (!g_lichii_once.empty() && A.cloud_gate.phase != CloudGate::Idle) g_lichii_once_at = GetTickCount64();
}

bool hookshot_lichii_once_consume(const std::string& key) {
    if (g_lichii_once.empty() || g_lichii_once != key) return false;
    const bool fresh = GetTickCount64() - g_lichii_once_at < 20000;
    g_lichii_once.clear();
    return fresh;
}

void draw_hookshot_run_dialogs() {
    lichii_once_tick();
    draw_missing_dialog();
    if (!ASK.open) return;
    bool open = false;
    emu_modal_begin("Movies need a Windows add-on", &open);
    if (!open) return;
    if (ui::modal_back()) ASK.open = false;
    ImGui::TextWrapped("Ocarina of Time's movies need Microsoft's free AV1 Video Extension.");
    ImGui::Dummy(ImVec2(0, 2 * S));
    text_wrapped_disabled("Get it from the Microsoft Store, then press Play again. Or play with Lichii this time.");
    ImGui::Dummy(ImVec2(0, 8 * S));
    if (!ASK.focused) { ImGui::SetKeyboardFocusHere(0); ASK.focused = true; }   // a pad lands on the first button
    if (button("Get it from the Microsoft Store", ImVec2(300 * S, 44 * S), true)) {
        if (!g_headless) ShellExecuteW(nullptr, L"open", L"ms-windows-store://pdp/?ProductId=9MVZQVXJBQ9V", nullptr, nullptr, SW_SHOWNORMAL);
        ASK.open = false;   // the next Play looks for the decoder again
    }
    if (button("Play with Lichii this time", ImVec2(300 * S, 44 * S))) { ASK.open = false; play_with_lichii_once(ASK.key); }
    if (button("Cancel", ImVec2(300 * S, 44 * S))) ASK.open = false;
    if (!ASK.open) { ImGui::CloseCurrentPopup(); ASK.focused = false; }
    ImGui::EndPopup();
}
