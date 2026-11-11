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

/* Screenshot aid, headless only: HUB_SHOT_CRASHNOTE=dump | nodump | start fakes one note. Touches no files and starts nothing. */
static void shot_fake_note() {
    static bool done = false;
    const char* m = g_headless ? std::getenv("HUB_SHOT_CRASHNOTE") : nullptr;
    if (done || !m || !*m) return;
    done = true;
    hubcrash::Crash c;
    c.emulator = "Fritchii";
    c.version = "5.7";
    c.exe_name = "lichii.exe";
    c.game = "The Legend of Zelda: Ocarina of Time";
    c.exit_code = 0xC0000005u;
    c.pid = 4242;
    c.session_secs = 312;
    c.time_unix = (long long)unix_now();
    c.log_dir = "C:\RetroHub\emulators\fritchii\user\log";
    if (std::string(m) == "dump") c.dump_path = "C:\Users\player\AppData\Local\CrashDumps\lichii.exe.4242.dmp";
    if (std::string(m) == "start") { c.session_secs = 2; c.exit_code = 0xC0000135u; }
    g_note = c;
    g_pending = true;
}

void draw_crash_note() {
    shot_fake_note();
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
    /* The buttons wrap onto a new line when the next one would not fit in the card (small windows, big UI size). */
    bool first = true;
    auto row_button = [&](const char* label) {
        const float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.f;
        if (!first) {
            const float x_end = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w <= x_end) ImGui::SameLine();
        }
        first = false;
        return ImGui::Button(label);
    };
    if (!g_note.dump_path.empty() && row_button("Open crash file folder")) open_folder(hubcrash::folder_of(g_note.dump_path));
    if (!g_note.log_dir.empty() && row_button("Open emulator log")) open_folder(g_note.log_dir);
    if (row_button("Copy details")) SDL_SetClipboardText(hubcrash::details_text(g_note).c_str());
    if (row_button("Dismiss")) dismiss = true;
    if (dismiss) {
        g_pending = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
