#include "app.h"
#include "crash_note.h"
#include "imgui_internal.h"

/* "<Emulator> stopped unexpectedly" note. launch.cpp's poll_process() calls crash_note_session_ended() when an external emulator's
 * session ends; the note is a modal card that opens only when prompt_allowed() (no game running, hub already in front), so it never
 * covers a game. Nothing is opened until a button is clicked. */

namespace {
hubcrash::Crash g_note;
bool g_pending = false;

void open_folder(const std::string& dir) {
    if (dir.empty() || g_headless) return;
    ShellExecuteW(nullptr, L"open", std::filesystem::u8path(dir).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);   // only from a button click
}
}

void crash_note_session_ended(int emu_kind, const std::string& exe_path, const std::string& emu_dir, const std::string& game,
                              unsigned long pid, unsigned long exit_code, long long start_unix, long long session_secs) {
    if (emu_kind < 0 || !hubcrash::is_crash((uint32_t)exit_code)) return;
    const hubemu::Kind k = (hubemu::Kind)emu_kind;
    hubcrash::Crash c;
    c.emulator = hubemu::name(k);
    c.version = hubemu::release(k).version ? hubemu::release(k).version : "";
    const size_t sl = exe_path.find_last_of("\\/");
    c.exe_name = sl == std::string::npos ? exe_path : exe_path.substr(sl + 1);
    c.game = game;
    c.exit_code = (uint32_t)exit_code;
    c.pid = pid;
    c.session_secs = session_secs;
    c.time_unix = (long long)unix_now();
    c.dump_path = hubcrash::find_dump(hubcrash::default_dumps_dir(), c.exe_name, pid, start_unix > 0 ? start_unix - 2 : 0);   // a dump older than this session is a recycled pid's
    if (hubemu::is_nx(k) && !emu_dir.empty()) {
        std::error_code ec;
        const std::string lg = emu_dir + "\\user\\log";
        if (std::filesystem::is_directory(std::filesystem::u8path(lg), ec)) c.log_dir = lg;
    }
    g_note = c;
    g_pending = true;
    autotest_log("crash note: " + hubcrash::headline(c) + " exit " + hubcrash::hex_code(c.exit_code) + (c.dump_path.empty() ? "" : " dump " + c.dump_path));
}

void draw_crash_note() {
    if (!g_pending) return;
    const char* id = "Emulator stopped";
    /* Checked before the window is submitted: never a frame of the card over a game. The note stays pending and comes back when the hub is in front. */
    if (!g_headless && !prompt_allowed()) {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        const ImGuiID pid = ImGui::GetID(id);
        for (int i = 0; i < g.OpenPopupStack.Size; ++i)
            if (g.OpenPopupStack[i].PopupId == pid) { ImGui::ClosePopupToLevel(i, false); break; }   // hide it without ever submitting its window
        return;
    }
    if (!ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);   // waits for: no game running, hub focused
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(sp(52.f), 0), ImVec2(sp(80.f), vp->Size.y * 0.8f));
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) return;
    bool dismiss = ui::modal_back();
    ImGui::PushFont(ui::F.h2);
    ImGui::TextWrapped("%s", hubcrash::headline(g_note).c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("You can try the game again.");
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (!g_note.dump_path.empty() && ImGui::Button("Open crash file folder")) open_folder(hubcrash::folder_of(g_note.dump_path));
    if (!g_note.dump_path.empty()) ImGui::SameLine();
    if (!g_note.log_dir.empty() && ImGui::Button("Open emulator log")) open_folder(g_note.log_dir);
    if (!g_note.log_dir.empty()) ImGui::SameLine();
    if (ImGui::Button("Copy details")) SDL_SetClipboardText(hubcrash::details_text(g_note).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Dismiss")) dismiss = true;
    if (dismiss) {
        g_pending = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
