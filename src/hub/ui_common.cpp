#include "app.h"

/* ============================ widgets / pages ============================ */

void page_header(const char* title, const char* sub) {
    ImGui::PushFont(ui::F.title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (sub && *sub) ImGui::TextDisabled("%s", sub);
    ImGui::Dummy(ImVec2(0, sp(1.f)));
}

void begin_panel(const char* id, ImVec2 size, ImU32 bg) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleColor(ImGuiCol_Border, ui::HAIRLINE);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, sp(2.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.5f), sp(2.25f)));
    ImGuiChildFlags fl = ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders;
    if (size.y == 0) fl |= ImGuiChildFlags_AutoResizeY;
    ImGui::BeginChild(id, size, fl);
}
void end_panel() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void text_wrapped_disabled(const std::string& s) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::PopStyleColor();
}

void add_rom_folder_path(const std::string& p) {
    if (std::find(A.st.rom_folders.begin(), A.st.rom_folders.end(), p) == A.st.rom_folders.end()) A.st.rom_folders.push_back(p);
    mark_dirty();
    start_scan();
}
void add_rom_folder() {
    std::string p = pick_path(A.hwnd, true, nullptr, nullptr);
    if (p.empty()) return;
    add_rom_folder_path(p);
}
void locate_smash() {
    std::string p = pick_path(A.hwnd, false, L"BattleShip.exe", L"*.exe");
    if (p.empty()) return;
    A.st.smash_exe = p;
    mark_dirty();
    A.hello_dirty = true;
    start_scan();
}

