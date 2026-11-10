// Console test: a pad press that ImGui's SDL backend polls while a game owns the pad must never reach the hub UI (input_gate.h).
// Real ImGui context, no renderer: shows the bug (without the purge the queued A press is applied and activates the nav-focused
// button) and the fix (with it nothing happens), and that a queued RELEASE still goes through.
#include "input_gate.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

struct Frame { bool pressed = false, activated = false, down = false; };

/* One hub frame: what main.cpp does around ImGui_ImplSDL2_NewFrame while pad-blocked. `backend_press` plays the backend's poll. */
static Frame frame(bool backend_press, bool purge, bool release_all, bool set_focus = false) {
    ImGuiIO& io = ImGui::GetIO();
    if (backend_press) io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);           // ImGui_ImplSDL2_UpdateGamepads
    if (purge) inputgate::purge_pad_presses(ImGui::GetCurrentContext());
    if (release_all) {
        io.ClearInputKeys();
        for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickRight; ++k) io.AddKeyAnalogEvent((ImGuiKey)k, false, 0.f);
    }
    ImGui::NewFrame();
    Frame f;
    f.pressed = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
    f.down = ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(400, 300));
    ImGui::Begin("w");
    if (set_focus) ImGui::SetKeyboardFocusHere();
    f.activated = ImGui::Button("Choose a folder...");
    ImGui::End();
    ImGui::EndFrame();
    return f;
}

static void fresh() {
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;   // the SDL backend sets this while a pad is connected
    unsigned char* pix; int w, h; io.Fonts->GetTexDataAsRGBA32(&pix, &w, &h);
    io.DeltaTime = 1.f / 60.f;
    frame(false, false, false, true);   // focus the button
    frame(false, false, false);
    frame(false, false, false);
    ImGui::GetCurrentContext()->NavCursorVisible = true;   // pad-driven nav, as in the hub (A.pad_mode)
}

int main() {
    // the bug: the backend's polled press survives ClearInputKeys + the queued releases for one frame
    {   fresh();
        Frame f = frame(true, false, true);
        CHECK(f.pressed);
        CHECK(f.activated);
    }
    // the fix: purge right after the backend's poll
    {   fresh();
        Frame f = frame(true, true, true);
        CHECK(!f.pressed);
        CHECK(!f.activated);
        CHECK(!f.down);
        for (int i = 0; i < 4; ++i) { f = frame(true, true, true); CHECK(!f.pressed && !f.activated && !f.down); }   // every frame the game keeps A held
    }
    // purge counts, and leaves non-pad keys and releases alone
    {   fresh();
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, true, 0.9f);
        io.AddKeyEvent(ImGuiKey_A, true);
        CHECK(inputgate::purge_pad_presses(ImGui::GetCurrentContext()) == 2);
        CHECK(ImGui::GetCurrentContext()->InputEventsQueue.Size == 1);
        CHECK(inputgate::purge_pad_presses(nullptr) == 0);
        ImGui::NewFrame(); ImGui::EndFrame();
    }
    // a release is still applied: a key held before blocking is released, never stuck
    {   fresh();
        Frame f = frame(true, false, false);   // unblocked: A goes down
        CHECK(f.pressed && f.down);
        f = frame(false, false, false);
        CHECK(f.down);
        f = frame(false, true, true);          // blocked now: release queued, purge keeps it
        CHECK(!f.down);
    }
    // picker guard: only a settled-focus hub with no game running (or headless) may open a file / folder dialog
    CHECK(inputgate::picker_may_open(false, false, false));
    CHECK(!inputgate::picker_may_open(false, true, false));    // unfocused / regain grace
    CHECK(!inputgate::picker_may_open(false, false, true));    // a game runs, hub focused: still no dialog over it
    CHECK(inputgate::picker_may_open(true, true, true));       // headless
    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("hub_input_gate_test: ok\n");
    return 0;
}
