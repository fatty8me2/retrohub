/* What's new: the patch notes list (shown on the Updates page, page_about.cpp) (the one-time Welcome back card lives in welcome_ui.cpp).
 * Data: <exe dir>\data\changelog.json (parsed by changelog.cpp). Shot aid: HUB_START_PAGE=5 (Updates page). */
#include "app.h"
#include "changelog.h"

namespace cl = hubui::changelog;

namespace {
struct ChangelogState {
    bool loaded = false;
    std::vector<cl::Entry> entries;
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

}  // namespace

void draw_changelog_list() {
    load();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    section_title("What's new");
    if (CL.entries.empty()) {
        begin_panel("cl_empty");
        text_wrapped_disabled("The patch notes file (data\changelog.json) was not found next to RetroHub.");
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

const std::vector<hubui::changelog::Entry>& changelog_entries() { load(); return CL.entries; }
