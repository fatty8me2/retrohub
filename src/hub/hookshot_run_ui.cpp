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

ULONGLONG g_lichii_once_at = 0;   // set with it: the choice expires after 20 s, so a launch that stopped at a prompt cannot leak into a later Play
std::string g_lichii_once;   // game key: the next launch_external for it skips Hookshot ("Play with Lichii this time")

}  // namespace

void hookshot_av1_ask(const hub::GameEntry& e) { ASK.open = true; ASK.key = e.key; ASK.focused = false; }
void hookshot_av1_ask_shot() { ASK.open = true; ASK.key.clear(); ASK.focused = false; }

bool hookshot_lichii_once_consume(const std::string& key) {
    if (g_lichii_once.empty() || g_lichii_once != key) return false;
    const bool fresh = GetTickCount64() - g_lichii_once_at < 20000;
    g_lichii_once.clear();
    return fresh;
}

void draw_hookshot_run_dialogs() {
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
    if (button("Play with Lichii this time", ImVec2(300 * S, 44 * S))) {
        ASK.open = false;
        if (const hub::GameEntry* g = find_entry(ASK.key)) { hub::GameEntry copy = *g; g_lichii_once = copy.key; g_lichii_once_at = GetTickCount64(); play_entry(copy); }
    }
    if (button("Cancel", ImVec2(300 * S, 44 * S))) ASK.open = false;
    if (!ASK.open) { ImGui::CloseCurrentPopup(); ASK.focused = false; }
    ImGui::EndPopup();
}
