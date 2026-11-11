#include "app.h"
#include "crash_report.h"
#include "crash_report_ui.h"
#include "hub_log.h"
#include "imgui_internal.h"

/* One note per crash report, modelled on crash_note_ui.cpp. Waits (stays pending) while a game runs, the hub is not in front or
 * another popup is open; nothing is opened until a button is clicked. Dismiss marks every existing report seen. */

namespace {
crashrep::Found g_found;
std::string g_logs;
bool g_pending = false;
}

void crash_report_ui_init(const std::string& logs_dir_utf8) {
    g_logs = logs_dir_utf8;
    g_pending = crashrep::find_unseen(logs_dir_utf8, &g_found);
    if (g_pending) hublog::warn("previous run ended in a crash: %s", g_found.name.c_str());
}

void draw_crash_report_note() {
    if (!g_pending) return;
    const char* id = "RetroHub closed";
    const bool is_open = ImGui::IsPopupOpen(id);
    if (!g_headless && !prompt_allowed()) {
        if (is_open) {   // a game started under it: hide it without ever submitting its window
            ImGuiContext& g = *ImGui::GetCurrentContext();
            const ImGuiID pid = ImGui::GetID(id);
            for (int i = 0; i < g.OpenPopupStack.Size; ++i)
                if (g.OpenPopupStack[i].PopupId == pid) { ImGui::ClosePopupToLevel(i, false); break; }
        }
        return;
    }
    if (!is_open) {
        if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;   // another card is up: wait for it
        ImGui::OpenPopup(id);
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(sp(52.f), 0), ImVec2(sp(80.f), vp->Size.y * 0.8f));
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) return;
    bool dismiss = ui::modal_back();
    ImGui::PushFont(ui::F.h2);
    ImGui::TextWrapped("RetroHub closed unexpectedly last time.");
    ImGui::PopFont();
    ImGui::TextDisabled("A report was saved. It holds no account or friend details, only technical information.");
    ImGui::Dummy(ImVec2(0, 4 * S));
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
    if (row_button("Open report folder") && !g_headless)
        ShellExecuteW(nullptr, L"open", W(g_found.dir_utf8).c_str(), nullptr, nullptr, SW_SHOWNORMAL);   // only from a button click
    if (row_button("Copy report")) SDL_SetClipboardText(crashrep::read_report(g_found.path_utf8).c_str());
    if (row_button("Dismiss")) dismiss = true;
    if (dismiss) {
        crashrep::mark_all_seen(g_logs);
        g_pending = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
