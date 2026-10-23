#include "app.h"

/* ---------- 3DS / Wii U emulator download + unzip dialogs ---------- */

void poll_emulators() {
    bool success = false;
    if (EMU.poll(&success)) {
        if (success) {
            set_toast(std::string(hubemu::name(EMU.kind)) + " installed.");
            if (const hub::GameEntry* g = find_entry(EMU.game_key)) { hub::GameEntry copy = *g; play_entry(copy); }
        }
    }
    if (UNZ.state == UnzipJob::Working && UNZ.done) {
        if (UNZ.th.joinable()) UNZ.th.join();
        if (UNZ.ok) {
            UNZ.state = UnzipJob::Idle;
            if (UNZ.quiet) { UNZ.quiet = false; set_toast("Unpacked."); }
            else {
                set_toast("Unpacked. Starting the game...");
                if (const hub::GameEntry* g = find_entry(UNZ.key)) { hub::GameEntry copy = *g; play_entry(copy); }
            }
        } else if (UNZ.error == "cancelled") {
            UNZ.state = UnzipJob::Idle;
            UNZ.quiet = false;
        } else {
            UNZ.state = UnzipJob::Failed;
        }
    }
}

void emu_modal_begin(const char* name, bool* open_flag) {
    ImGui::OpenPopup(name);
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(66.f), ImGui::GetIO().DisplaySize.x * 0.92f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    *open_flag = ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(2);
}

void draw_emulator_dialogs() {
    if (EMU.state != hubemu::Installer::Idle) {
        const hubemu::Release& rel = hubemu::release(EMU.kind);
        std::string nm = hubemu::name(EMU.kind);
        bool open = false;
        emu_modal_begin("Emulator download", &open);
        if (open) {
            if (EMU.state == hubemu::Installer::Asking) {
                ImGui::TextWrapped("%s games need the %s emulator (about %d MB download from its official release, %s). "
                                   "It is kept inside RetroHub's data folder. Download now?",
                                   EMU.kind == hubemu::Kind::Azahar ? "Nintendo 3DS" : EMU.kind == hubemu::Kind::Dolphin ? "GameCube" : "Wii U", nm.c_str(), (int)(rel.size / 1048576), rel.license);
                ImGui::Dummy(ImVec2(0, 8 * S));
                if (button("Download", ImVec2(160 * S, 44 * S), true)) EMU.start(EMU.kind, hubemu::managed_root());
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (button("Cancel", ImVec2(160 * S, 44 * S))) EMU.state = hubemu::Installer::Idle;
            } else if (EMU.state == hubemu::Installer::Working) {
                std::string phases[] = {"Downloading " + nm + "...", "Checking the download...", "Unpacking...", "Installing..."};
                ImGui::TextUnformatted(phases[std::min(3, (int)EMU.phase)].c_str());
                uint64_t got = EMU.got, tot = EMU.total;
                char ov[48];
                std::snprintf(ov, sizeof ov, "%.1f / %.1f MB", got / 1048576.0, tot / 1048576.0);
                ImGui::ProgressBar(EMU.phase > 0 ? 1.f : (tot ? (float)got / (float)tot : 0.f), ImVec2(-1, 24 * S), EMU.phase > 0 ? "" : ov);
                ImGui::TextDisabled("Your game starts as soon as it is ready.");
            } else {
                std::string err;
                { std::lock_guard<std::mutex> lk(EMU.mu); err = EMU.error; }
                ImGui::TextWrapped("Could not install %s: %s", nm.c_str(), err.c_str());
                ImGui::Dummy(ImVec2(0, 8 * S));
                if (button("Close", ImVec2(160 * S, 44 * S), true)) EMU.state = hubemu::Installer::Idle;
                ImGui::SetItemDefaultFocus();
            }
            if (EMU.state == hubemu::Installer::Idle) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    if (UNZ.state != UnzipJob::Idle) {
        bool open = false;
        emu_modal_begin("Unpack game", &open);
        if (open) {
            const hub::GameEntry* g = find_entry(UNZ.key);
            if (UNZ.state == UnzipJob::Asking && g) {
                ImGui::TextWrapped("%s cannot open archives. Unpack \"%s\" (about %.1f GB) into RetroHub's data folder?", g->system == hub::System::GC ? "Dolphin" : "Azahar", g->title.c_str(),
                                   unpacked_size(*g) / 1073741824.0);
                ImGui::Dummy(ImVec2(0, 2 * S));
                text_wrapped_disabled("The unpacked image goes to " + hub::extract_3ds_dir(*g) + " and needs the same amount of free disk space. "
                                      "Your archive is left untouched. You can also unpack it yourself into a ROM folder.");
                ImGui::Dummy(ImVec2(0, 8 * S));
                if (button("Unpack and play", ImVec2(200 * S, 44 * S), true)) start_unzip(*g);
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (button("Cancel", ImVec2(160 * S, 44 * S))) UNZ.state = UnzipJob::Idle;
            } else if (UNZ.state == UnzipJob::Working) {
                ImGui::TextUnformatted("Unpacking...");
                uint64_t got = UNZ.got, tot = UNZ.total;
                char ov[48];
                std::snprintf(ov, sizeof ov, "%.2f / %.2f GB", got / 1073741824.0, tot / 1073741824.0);
                ImGui::ProgressBar(tot ? (float)((double)got / (double)tot) : 0.f, ImVec2(-1, 24 * S), ov);
                ImGui::Dummy(ImVec2(0, 6 * S));
                if (button("Cancel", ImVec2(160 * S, 44 * S))) UNZ.cancel = true;
            } else {
                std::string err;
                { std::lock_guard<std::mutex> lk(UNZ.mu); err = UNZ.error; }
                ImGui::TextWrapped("Could not unpack the game: %s", err.c_str());
                ImGui::Dummy(ImVec2(0, 8 * S));
                if (button("Close", ImVec2(160 * S, 44 * S), true)) UNZ.state = UnzipJob::Idle;
                ImGui::SetItemDefaultFocus();
            }
            if (UNZ.state == UnzipJob::Idle) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
}

