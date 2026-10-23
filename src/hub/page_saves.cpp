#include "app.h"

/* ---------- Saves ---------- */

void refresh_vault() {
    A.vault_rows.clear();
    std::string root = with_slash(hub::saves::vault_dir());
    WIN32_FIND_DATAW sf;
    HANDLE hs = FindFirstFileW(W(root + "*").c_str(), &sf);
    if (hs != INVALID_HANDLE_VALUE) {
        do {
            std::string sys = U(sf.cFileName);
            if (!(sf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || sys.empty() || sys[0] == '.' || sys[0] == '_') continue;
            WIN32_FIND_DATAW gf;
            HANDLE hg = FindFirstFileW(W(root + sys + "\\*").c_str(), &gf);
            if (hg == INVALID_HANDLE_VALUE) continue;
            do {
                std::string g = U(gf.cFileName);
                if (!(gf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || g == "." || g == "..") continue;
                VaultRow r;
                if (hubui::parse_vault_meta(read_text_file(root + sys + "\\" + g + "\\meta.json"), &r.m)) {
                    if (r.m.system.empty()) r.m.system = sys;
                    A.vault_rows.push_back(r);
                }
            } while (FindNextFileW(hg, &gf));
            FindClose(hg);
        } while (FindNextFileW(hs, &sf));
        FindClose(hs);
    }
    std::sort(A.vault_rows.begin(), A.vault_rows.end(), [](const VaultRow& a, const VaultRow& b) { return a.m.updated > b.m.updated; });
    A.vault_dirty = false;
}

void result_line(const std::string& s, bool ok) {
    if (s.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ok ? COL_GOOD : COL_BAD));
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::PopStyleColor();
}

void page_saves() {
    page_header("Saves", "One portable save vault. Sync it between PCs or back it up; saves follow you across installs.");
    if (A.vault_dirty) refresh_vault();
    bool busy = A.job.running;

    begin_panel("vault");
    section_title("Save vault");
    text_wrapped_disabled(hub::saves::vault_dir());
    text_wrapped_disabled("Every game's in-game saves and save states live here. Copy this folder to move everything by hand.");
    if (button("Open vault folder", ImVec2(0, 0))) open_in_explorer(hub::saves::vault_dir());
    end_panel();
    ImGui::Dummy(ImVec2(0, 4 * S));

    begin_panel("sync");
    section_title("Sync between PCs");
    text_wrapped_disabled("Pick a folder both PCs can see (OneDrive, Dropbox, Google Drive, a USB stick or a NAS). RetroHub keeps "
                          "it in step with the vault in both directions and never deletes a save; if both sides changed, the older copy is kept.");
    ImGui::TextUnformatted("Sync folder");
    text_wrapped_disabled(A.st.sync_folder.empty() ? "(not set)" : A.st.sync_folder);
    if (button("Browse...", ImVec2(0, 0), A.st.sync_folder.empty())) {
        std::string p = pick_path(A.hwnd, true, nullptr, nullptr);
        if (!p.empty()) { A.st.sync_folder = p; mark_dirty(); }
    }
    if (!A.st.sync_folder.empty()) {
        ImGui::SameLine();
        if (button("Stop syncing", ImVec2(0, 0))) { A.st.sync_folder.clear(); mark_dirty(); A.sync_result.clear(); }
    }
    for (auto& sg : sync_suggestions()) {
        if (sg.path == A.st.sync_folder) continue;
        ImGui::SameLine();
        std::string lab = "Use " + sg.label + "##" + sg.path;
        if (button(lab.c_str(), ImVec2(0, 0))) {
            make_dirs(sg.path);
            A.st.sync_folder = sg.path;
            mark_dirty();
            start_sync(true);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sg.path.c_str());
    }
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (ImGui::Checkbox("Sync automatically after playing", &A.st.sync_auto)) mark_dirty();
    if (ImGui::Checkbox("Include save states (bigger)", &A.st.sync_states)) mark_dirty();
    ImGui::Dummy(ImVec2(0, 2 * S));
    bool can_sync = !A.st.sync_folder.empty() && !busy && !A.proc;
    if (button(busy && A.job.kind == SaveJob::Sync ? "Syncing..." : "Sync now", ImVec2(180 * S, 46 * S), true, can_sync)) start_sync(true);
    if (A.proc && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A game is running. Saves sync automatically when it closes.");
    if (!A.sync_result.empty()) {
        ImGui::SameLine();
        ImGui::BeginGroup();
        result_line(A.sync_result, A.sync_result_ok);
        ImGui::EndGroup();
    }
    end_panel();
    ImGui::Dummy(ImVec2(0, 4 * S));

    begin_panel("backup");
    section_title("Backup and restore");
    text_wrapped_disabled("Export writes every save to one .zip file. Import merges a zip back in (the newer file wins; the other is kept).");
    if (button("Export all saves...", ImVec2(0, 0), false, !busy)) {
        time_t t = time(nullptr);
        tm lt{};
        localtime_s(&lt, &t);
        wchar_t nm[64];
        std::wcsftime(nm, 64, L"RetroHub-saves-%Y%m%d.zip", &lt);
        std::string p = pick_save_path(A.hwnd, nm, L"Zip archive", L"*.zip", L"zip");
        if (!p.empty()) start_job(SaveJob::Export, p, true);
    }
    ImGui::SameLine();
    if (button("Import saves...", ImVec2(0, 0), false, !busy)) {
        std::string p = pick_path(A.hwnd, false, L"Zip archive", L"*.zip");
        if (!p.empty()) start_job(SaveJob::Import, p, true);
    }
    if (busy && A.job.kind != SaveJob::Sync) { ImGui::SameLine(); ImGui::TextDisabled("Working..."); }
    result_line(A.export_result, A.export_ok);
    result_line(A.import_result, A.import_ok);
    end_panel();
    ImGui::Dummy(ImVec2(0, 4 * S));

    section_title("In the vault (" + std::to_string(A.vault_rows.size()) + ")");
    if (A.vault_rows.empty()) {
        ImGui::TextDisabled("Nothing yet. Play a game and its saves show up here.");
    } else if (ImGui::BeginTable("vaulttbl", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Last updated", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();
        for (auto& r : A.vault_rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.m.title.empty() ? r.m.key.c_str() : r.m.title.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(hub::system_name(hub::system_from_short(r.m.system)));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmt_time(r.m.updated).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.m.device.c_str());
        }
        ImGui::EndTable();
    }
}

