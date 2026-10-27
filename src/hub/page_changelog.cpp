/* What's new: the patch-notes page (sidebar + About) and the one-time "What's new in vX" popup after an update.
 * Data: <exe dir>\data\changelog.json (parsed by changelog.cpp). Shot aids: HUB_START_PAGE=6 (page), HUB_SHOT_CHANGELOG=popup
 * (popup over the current page; the popup never opens by itself in screenshot/headless runs), HUB_SHOT_CHANGELOG_VER=<x.y.z>. */
#include "app.h"
#include "changelog.h"
#include "wizard_logic.h"

namespace cl = hubui::changelog;

namespace {
struct ChangelogState {
    bool loaded = false;
    std::vector<cl::Entry> entries;
    bool popup_open = false, popup_request = false;
    std::string popup_ver;
    bool env_done = false;
} CL;

void load() {
    if (CL.loaded) return;
    CL.loaded = true;
    std::string text;
    FILE* f = std::fopen((hub::exe_dir() + "data/changelog.json").c_str(), "rb");
    if (f) {
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    cl::parse(text, &CL.entries);
}

ImU32 section_col(int s) { return s == cl::New ? COL_GOOD : s == cl::Improved ? COL_ACCENT : COL_WARN; }

void draw_entry_body(const cl::Entry& e) {
    for (int s = 0; s < cl::kSections; ++s) {
        if (e.items[s].empty()) continue;
        ImGui::Dummy(ImVec2(0, 3 * S));
        ImGui::PushFont(ui::F.bold);
        ImGui::PushStyleColor(ImGuiCol_Text, section_col(s));
        ImGui::TextUnformatted(cl::section_title(s));
        ImGui::PopStyleColor();
        ImGui::PopFont();
        for (const std::string& b : e.items[s]) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", b.c_str());
        }
    }
}

void dismiss_popup(bool see_all) {
    CL.popup_open = false;
    A.st.changelog_seen = CL.popup_ver;
    mark_dirty();
    if (see_all) A.page = Page::Changelog;
}
}  // namespace

void page_changelog() {
    load();
    page_header("What's new", "Patch notes for every RetroHub update");
    if (CL.entries.empty()) {
        begin_panel("cl_empty");
        text_wrapped_disabled("The patch notes file (data\\changelog.json) was not found next to RetroHub.");
        end_panel();
        return;
    }
    begin_panel("changelog");
    bool first = true;
    for (const cl::Entry& e : CL.entries) {
        std::string label = "v" + e.version + "    " + cl::pretty_date(e.date) + (e.title.empty() ? "" : "    " + e.title) + "###cl" + e.version;
        ImGui::SetNextItemOpen(first, first ? ImGuiCond_Once : ImGuiCond_Once);
        bool cur = std::string(HUB_VERSION_STR).find(e.version) != std::string::npos;
        if (cur) label = "v" + e.version + "  (installed)    " + cl::pretty_date(e.date) + (e.title.empty() ? "" : "    " + e.title) + "###cl" + e.version;
        if (ImGui::CollapsingHeader(label.c_str())) {
            ImGui::Indent(sp(1.f));
            draw_entry_body(e);
            ImGui::Unindent(sp(1.f));
            ImGui::Dummy(ImVec2(0, 4 * S));
        }
        first = false;
    }
    end_panel();
}

void draw_changelog_popup() {
    load();
    const char* shot = std::getenv("HUB_SHOT_CHANGELOG");
    if (!CL.env_done && shot && A.scanned_once) {   // screenshot aid
        CL.env_done = true;
        const char* v = std::getenv("HUB_SHOT_CHANGELOG_VER");
        CL.popup_ver = v ? v : (CL.entries.empty() ? "" : CL.entries[0].version);
        CL.popup_request = true;
    } else if (!CL.popup_request && !CL.popup_open && !shot && !g_headless && A.scanned_once && !CL.env_done && prompt_allowed()) {
        using hubui::changelog::Popup;
        std::string cur = HUB_VERSION_STR;
        bool fresh = hubui::wiz::should_autorun(A.st.setup_done, A.st.find_games_done, !A.st.rom_folders.empty());
        bool blocked = wizard_state() != -1 || A.popup_last_frame;
        Popup act = cl::popup_action(A.st.changelog_seen, cur, cl::find(CL.entries, cur) != nullptr, fresh, blocked);
        if (act == Popup::MarkSeen) {
            A.st.changelog_seen = cur;
            mark_dirty();
        } else if (act == Popup::Show) {
            CL.popup_ver = cur;
            CL.popup_request = true;
        }
    }
    if (CL.popup_request) { CL.popup_request = false; CL.popup_open = true; ImGui::OpenPopup("What's new##popup"); }
    if (!CL.popup_open) return;
    const cl::Entry* e = cl::find(CL.entries, CL.popup_ver);
    if (!e) { CL.popup_open = false; return; }

    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(72.f), ImGui::GetIO().DisplaySize.x * 0.92f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal("What's new##popup", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleVar(2);
    if (!open) return;
    if (ui::modal_back()) { dismiss_popup(false); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    ImGui::PushFont(ui::F.title);
    ImGui::Text("What's new in v%s", e->version.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s%s%s", cl::pretty_date(e->date).c_str(), e->title.empty() ? "" : "  -  ", e->title.c_str());
    ImGui::Dummy(ImVec2(0, 2 * S));
    float maxh = std::max(sp(12.f), ImGui::GetIO().DisplaySize.y * 0.56f);
    ImGui::BeginChild("cl_pop_body", ImVec2(0, maxh), ImGuiChildFlags_NavFlattened);
    {   // D-pad / stick up-down scroll the notes (focus stays on the buttons)
        float dir = (ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickDown) ? 1.f : 0.f) -
                    (ImGui::IsKeyDown(ImGuiKey_GamepadDpadUp) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickUp) ? 1.f : 0.f);
        if (dir != 0.f) ImGui::SetScrollY(ImGui::GetScrollY() + dir * ImGui::GetIO().DeltaTime * 700.f * S);
    }
    draw_entry_body(*e);
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (button("Got it", ImVec2(160 * S, 44 * S), true)) { dismiss_popup(false); ImGui::CloseCurrentPopup(); }
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (button("See all patch notes", ImVec2(240 * S, 44 * S))) { dismiss_popup(true); ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}
