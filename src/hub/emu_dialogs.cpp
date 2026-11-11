#include "app.h"

/* ---------- 3DS / Wii U emulator download + unzip dialogs ---------- */

void poll_emulators() {
    static bool recovered = false;
    if (!recovered && EMU.state != hubemu::Installer::Working) {   // an update cut short by a crash: roll it back / finish it, once, before anything uses the install
        recovered = true;
        hubemu::recover_interrupted(hubemu::Kind::Lichii);
        hubemu::recover_interrupted(hubemu::Kind::Fritchii);
    }
    poll_nxprep();
    bool success = false;
    if (EMU.poll(&success)) {
        const bool upd = EMU.updating;
        if (success) {
            set_toast(std::string(hubemu::name(EMU.kind)) + (upd ? " updated." : " installed."));
            if (const hub::GameEntry* g = find_entry(EMU.game_key)) { hub::GameEntry copy = *g; if (!A.proc) play_entry(copy); }
        } else if (upd) {   // a failed update is not a failed game: the old build is intact, so say so quietly and carry on with it
            std::string err;
            { std::lock_guard<std::mutex> lk(EMU.mu); err = EMU.error; }
            EMU_UPDATE_SKIP[(int)EMU.kind] = true;
            EMU.state = hubemu::Installer::Idle;
            set_toast(std::string("Could not update ") + hubemu::name(EMU.kind) + ": " + err + ". The current version still works.", false, 10.0);
            if (const hub::GameEntry* g = find_entry(EMU.game_key)) { hub::GameEntry copy = *g; if (!A.proc) play_entry(copy); }
        }
        EMU.updating = false;
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
    draw_nxprep_dialog();
    if (EMU.state != hubemu::Installer::Idle) {
        const hubemu::Release& rel = hubemu::release(EMU.kind);
        std::string nm = hubemu::name(EMU.kind);
        bool open = false;
        emu_modal_begin("Emulator download", &open);
        if (open) {
            const bool back = ui::modal_back();
            if (EMU.state == hubemu::Installer::Asking) {
                if (back) EMU.state = hubemu::Installer::Idle;
                if (hubemu::release_is_placeholder(EMU.kind)) {   // Fritchii before its release is pinned: nothing to download yet
                    ImGui::TextWrapped("%s isn't available yet. Pick Lichii in Settings > Switch to play now.", nm.c_str());
                    ImGui::Dummy(ImVec2(0, 8 * S));
                    if (button("Close", ImVec2(160 * S, 44 * S), true)) EMU.state = hubemu::Installer::Idle;
                    ImGui::SetItemDefaultFocus();
                } else if (EMU.updating && EMU.kind == hubemu::Kind::Fritchii && !hubemu::update_leftovers(EMU.kind, hubemu::find(EMU.kind, "", "").dir).empty()) {   // its own updater left a half-finished update: Fritchii would open an "Interrupted Update" box over the game
                    ImGui::TextWrapped("Fritchii was left half-updated by its own updater. Reinstall the tested version (%s, about %d MB, the copy built from Frizti's published source) so it starts cleanly? "
                                       "Your keys, firmware, saves and settings are kept.", rel.version, (int)(rel.size / 1048576));
                    ImGui::Dummy(ImVec2(0, 8 * S));
                    if (button("Reinstall", ImVec2(160 * S, 44 * S), true)) EMU.start(EMU.kind, hubemu::managed_root());
                    ImGui::SetItemDefaultFocus();
                    ImGui::SameLine();
                    if (button("Cancel", ImVec2(160 * S, 44 * S))) { EMU.state = hubemu::Installer::Idle; EMU.updating = false; }
                } else if (EMU.updating) {
                    ImGui::TextWrapped("%s has a newer version (%s, about %d MB download%s). Update it now? "
                                       "Your keys, firmware, saves and settings are kept. If the update fails, the current version stays.",
                                       nm.c_str(), rel.version, (int)(rel.size / 1048576), EMU.kind == hubemu::Kind::Fritchii ? ", a copy built from Frizti's published source" : " from its official release");
                    ImGui::Dummy(ImVec2(0, 8 * S));
                    if (button("Update", ImVec2(160 * S, 44 * S), true)) EMU.start(EMU.kind, hubemu::managed_root());
                    ImGui::SetItemDefaultFocus();
                    ImGui::SameLine();
                    if (button("Not now", ImVec2(160 * S, 44 * S))) {   // play with what is installed
                        EMU_UPDATE_SKIP[(int)EMU.kind] = true;
                        EMU.state = hubemu::Installer::Idle;
                        EMU.updating = false;
                        if (const hub::GameEntry* g = find_entry(EMU.game_key)) { hub::GameEntry copy = *g; if (!A.proc) play_entry(copy); }
                    }
                } else {
                if (EMU.kind == hubemu::Kind::Fritchii)
                    ImGui::TextWrapped("Fritchii: an Ocarina of Time-focused version of Lichii by Frizti (about %d MB). RetroHub downloads a copy built from Frizti's published source. "
                                       "It is kept inside RetroHub's data folder, separate from Lichii, and your saves are shared between the two. Download now?",
                                       (int)(rel.size / 1048576));
                else
                ImGui::TextWrapped("%s games need the %s emulator (about %d MB download from its official release, %s). "
                                   "It is kept inside RetroHub's data folder. Download now?",
                                   EMU.kind == hubemu::Kind::Azahar ? "Nintendo 3DS" : EMU.kind == hubemu::Kind::Dolphin ? "GameCube" : EMU.kind == hubemu::Kind::Pcsx2 ? "PlayStation 2" : EMU.kind == hubemu::Kind::Ppsspp ? "PSP" : EMU.kind == hubemu::Kind::Lichii ? "Switch / Switch 2" : "Wii U", nm.c_str(), (int)(rel.size / 1048576), rel.license);
                ImGui::Dummy(ImVec2(0, 8 * S));
                if (button("Download", ImVec2(160 * S, 44 * S), true)) EMU.start(EMU.kind, hubemu::managed_root());
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (button("Cancel", ImVec2(160 * S, 44 * S))) EMU.state = hubemu::Installer::Idle;
                }
            } else if (EMU.state == hubemu::Installer::Working) {
                std::string phases[] = {(EMU.updating ? "Updating " : "Downloading ") + nm + "...", "Checking the download...", "Unpacking...", "Installing..."};
                ImGui::TextUnformatted(phases[std::min(3, (int)EMU.phase)].c_str());
                uint64_t got = EMU.got, tot = EMU.total;
                char ov[48];
                std::snprintf(ov, sizeof ov, "%.1f / %.1f MB", got / 1048576.0, tot / 1048576.0);
                ImGui::ProgressBar(EMU.phase > 0 ? 1.f : (tot ? (float)got / (float)tot : 0.f), ImVec2(-1, 24 * S), EMU.phase > 0 ? "" : ov);
                ImGui::TextDisabled(EMU.updating ? "Your game starts as soon as the update is done." : "Your game starts as soon as it is ready.");
            } else {
                if (back) EMU.state = hubemu::Installer::Idle;
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
            if (ui::modal_back()) {
                if (UNZ.state == UnzipJob::Working) UNZ.cancel = true;
                else UNZ.state = UnzipJob::Idle;
            }
            if (UNZ.state == UnzipJob::Asking && g) {
                ImGui::TextWrapped("%s cannot open archives. Unpack \"%s\" (about %.1f GB) into RetroHub's data folder?", g->system == hub::System::GC ? "Dolphin" : g->system == hub::System::PS1 ? "The PlayStation emulator" : g->system == hub::System::PS2 ? "PCSX2" : g->system == hub::System::PSP ? "PPSSPP" : g->system == hub::System::WiiU ? "Cemu" : "Azahar", g->title.c_str(),
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
                ImGui::TextWrapped("Preparing %s (one time)...", g ? g->title.c_str() : "the game");
                text_wrapped_disabled("Big discs take a few minutes to unpack. The next time you press Play it starts at once. Nothing is wrong if this looks slow.");
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

