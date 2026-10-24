#include "app.h"
#include "addgames.h"

#include <shlobj.h>

/* ---------- "Add games": upload your own ROMs into RetroHub ----------
 * Entry points: the Add games tile (Home), the banner (All Games), the "+ Add games" header button, and drag-and-drop of
 * files / folders onto the window. Files are COPIED into the games folder (sorted by console) on a worker thread; folders
 * are added to rom_folders in place, like the old "Add ROM folder". Screenshot aids: HUB_SHOT_ADDGAMES=dialog|progress|results|toast. */

namespace ag = hub::addgames;

namespace {

struct AddGames {
    enum St { Closed, Choose, Importing, Done } st = Closed;
    ag::Job job;
    std::vector<std::string> drops;   // paths from SDL_DROPFILE, resolved on the next frame (a drop arrives as many events)
    std::vector<std::string> queue;   // files waiting for the running job
    std::vector<ag::Item> result;
    std::string root;
    bool rescan_pending = false, shot_tried = false, fake_progress = false;
};
AddGames AG;

std::string documents_dir() {
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p) out = U(p);
    if (p) CoTaskMemFree(p);
    if (out.empty()) {
        const char* up = std::getenv("USERPROFILE");
        out = std::string(up ? up : "C:") + "\\Documents";
    }
    return out;
}

/* Multi-select file dialog for every supported format. */
std::vector<std::string> pick_game_files() {
    std::vector<std::string> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST);
    std::wstring spec;
    for (const std::string& e : ag::supported_exts()) spec += (spec.empty() ? L"*" : L";*") + W(e);
    COMDLG_FILTERSPEC f[2] = {{L"Game files", spec.c_str()}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, f);
    if (SUCCEEDED(dlg->Show(A.hwnd))) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(dlg->GetResults(&arr))) {
            DWORD n = 0;
            arr->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* it = nullptr;
                PWSTR p = nullptr;
                if (SUCCEEDED(arr->GetItemAt(i, &it))) {
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out.push_back(U(p)); CoTaskMemFree(p); }
                    it->Release();
                }
            }
            arr->Release();
        }
    }
    dlg->Release();
    return out;
}

bool is_dir(const std::string& p) {
    DWORD a = GetFileAttributesW(W(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool add_folder_in_place(const std::string& p) {
    bool added = hubui::merge_installer_games_dir(&A.st, p);   // absolute-path check + case/slash-insensitive dedupe
    if (added) { mark_dirty(); AG.rescan_pending = true; }
    return added;
}

void start_import(std::vector<std::string> files) {
    std::set<std::string> known;
    for (const hub::GameEntry& e : A.lib) known.insert(e.key);
    AG.root = ag::games_root(A.st.rom_folders, documents_dir());
    AG.result.clear();
    AG.st = AddGames::Importing;
    AG.job.start(std::move(files), AG.root, std::move(known));
}

void finish_import() {
    AG.job.join();
    AG.result = AG.job.items();
    AG.st = AddGames::Done;
    int added = ag::count_outcome(AG.result, ag::Outcome::Added);
    if (added) {   // the games folder joins rom_folders (once), then the library rescans
        if (hubui::merge_installer_games_dir(&A.st, AG.root)) mark_dirty();
        AG.rescan_pending = true;
    }
    int bad = ag::count_outcome(AG.result, ag::Outcome::Failed);
    std::string t = ag::summary_text(AG.result);
    push_toast(t, bad && !added ? ui::BAD : (bad ? ui::WARN : ui::GOOD), 12.0);
}

std::string bytes_text(uint64_t b) { return size_text(b); }

}  // namespace

/* Files and folders from the picker or a window drop. Folders are added in place; files are copied. */
void add_games_paths(const std::vector<std::string>& paths) {
    int folders_added = 0, folders_seen = 0;
    for (const std::string& p : paths) {
        if (is_dir(p)) { ++folders_seen; folders_added += add_folder_in_place(p); }
        else AG.queue.push_back(p);
    }
    if (folders_added) set_toast(folders_added == 1 ? "Folder added - scanning your games..." : "Folders added - scanning your games...");
    else if (folders_seen && AG.queue.empty()) set_toast("That folder is already in your library.", false);
}

void add_games_drop(const std::string& path) { AG.drops.push_back(path); }

void add_games_open() {
    if (AG.st == AddGames::Importing) return;
    AG.st = AddGames::Choose;
}

/* ---------- the entry points drawn in the library ---------- */

/* Header button: "+ Add games". Returns the button's width for layout. */
float add_games_header_width(float bh) { return ImGui::CalcTextSize("Add games").x + sp(5.f) + bh * 0.6f + sp(1.f); }
void add_games_header_button(float bh) {
    if (button("Add games", ImVec2(add_games_header_width(bh), bh), true, true, ui::Icon::Plus)) add_games_open();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add your own game files (or drop them on this window)");
}

/* The tile that leads the first Home shelf. */
TileResult add_games_tile(ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImGui::InvisibleButton("##addgames_tile", size);
    TileResult r;
    r.visible = ImGui::IsItemVisible();
    r.hovered = ImGui::IsItemHovered();
    r.focused = ImGui::IsItemFocused();
    r.clicked = ImGui::IsItemClicked(0) && !g_suppress_click;
    r.activated = ImGui::IsItemActivated() && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    float rad = sp(1.75f);
    bool hot = r.hovered || r.focused;
    dl->AddRectFilled(p0, p1, ui::mix(ui::PANEL, ui::ACCENT, hot ? 0.18f : 0.08f), rad);
    dl->AddRect(p0, p1, hot ? ui::ACCENT : ui::with_alpha(ui::ACCENT, 0.55f), rad, 0, hot ? 2.f : 1.5f);
    float cx = (p0.x + p1.x) * 0.5f, cy = p0.y + size.y * 0.30f, ir = std::min(size.x, size.y) * 0.15f;
    dl->AddCircleFilled(ImVec2(cx, cy), ir, ui::with_alpha(ui::ACCENT, hot ? 0.35f : 0.22f), 40);
    ui::icon(dl, ui::Icon::Plus, ImVec2(cx, cy), ir * 1.1f, ui::mix(ui::ACCENT, IM_COL32_WHITE, 0.55f));
    float ty = cy + ir + sp(1.f);
    auto centered = [&](ImFont* f, ImU32 col, const std::string& s, float maxw) {
        std::vector<std::string> lines = ui::wrap_lines(f, f->FontSize, s, maxw, 3);
        for (auto& ln : lines) {
            ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, ln.c_str());
            dl->AddText(f, f->FontSize, ImVec2(cx - ts.x * 0.5f, ty), col, ln.c_str());
            ty += f->FontSize * 1.2f;
        }
    };
    centered(ui::F.bold, ui::TEXT, "Add your own games", size.x - sp(3.f));
    ty += sp(0.4f);
    centered(ui::F.cap, ui::TEXT2, "Drop ROM files here or click to choose", size.x - sp(3.f));
    if (r.hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (r.clicked || r.activated) add_games_open();
    return r;
}

/* Banner at the top of All Games. */
void add_games_banner() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x, h = sp(9.f);
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
    ImGui::InvisibleButton("##addgames_banner", ImVec2(w, h));
    bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    float rad = sp(2.f);
    dl->AddRectFilled(p0, p1, ui::mix(ui::PANEL, ui::ACCENT, hot ? 0.18f : 0.08f), rad);
    dl->AddRect(p0, p1, hot ? ui::ACCENT : ui::with_alpha(ui::ACCENT, 0.55f), rad, 0, hot ? 2.f : 1.5f);
    float ir = sp(2.75f), cy = (p0.y + p1.y) * 0.5f, cx = p0.x + sp(2.f) + ir;
    dl->AddCircleFilled(ImVec2(cx, cy), ir, ui::with_alpha(ui::ACCENT, hot ? 0.35f : 0.22f), 40);
    ui::icon(dl, ui::Icon::Plus, ImVec2(cx, cy), ir * 1.1f, ui::mix(ui::ACCENT, IM_COL32_WHITE, 0.55f));
    float tx = cx + ir + sp(2.f);
    dl->AddText(ui::F.h2, ui::F.h2->FontSize, ImVec2(tx, cy - ui::F.h2->FontSize - sp(0.1f)), ui::TEXT, "Add your own games");
    dl->AddText(ui::F.body, ui::F.body->FontSize, ImVec2(tx, cy + sp(0.4f)), ui::TEXT2, "Drop ROM files here or click to choose");
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemClicked(0) || (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) add_games_open();
    ImGui::Dummy(ImVec2(0, sp(1.5f)));
}

/* ---------- the dialog ---------- */

static void add_games_modal() {
    ImGui::OpenPopup("##addgames");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(84.f), ImGui::GetIO().DisplaySize.x * 0.94f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal("##addgames", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!open) return;
    const float bh = sp(5.5f), bw = ImGui::GetContentRegionAvail().x;

    ImGui::PushFont(ui::F.title);
    ImGui::TextUnformatted(AG.st == AddGames::Done ? "All done" : AG.st == AddGames::Importing ? "Adding your games" : "Add your own games");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    if (AG.st == AddGames::Choose) {
        text_wrapped_disabled("RetroHub copies your game files into its own games folder, sorted by console. Your original files are never moved or changed.");
        std::string root = ag::games_root(A.st.rom_folders, documents_dir());
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::TextWrapped("Games folder: %s", root.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
        if (button("Choose game files...", ImVec2(bw, bh), true, true, ui::Icon::Plus)) {
            std::vector<std::string> f = pick_game_files();
            if (!f.empty()) AG.queue.insert(AG.queue.end(), f.begin(), f.end());
        }
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (button("Choose a folder...", ImVec2(bw, bh), false, true, ui::Icon::Folder)) {
            std::string p = pick_path(A.hwnd, true, nullptr, nullptr);
            if (!p.empty()) {
                bool added = add_folder_in_place(p);
                set_toast(added ? "Folder added - scanning your games..." : "That folder is already in your library.", added);
                AG.st = AddGames::Closed;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::PushTextWrapPos(0.f);
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::TextUnformatted("A folder is added where it is (nothing is copied). You can also drag files or folders onto this window at any time.");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::TextUnformatted("Formats: .z64 .n64 .v64 .gba .gb .gbc .nds .3ds .cci .cia .iso .gcm .rvz .ciso .wia .gcz .wua .wud .wux, and .zip / .7z");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
        if (button("Close", ImVec2(sp(14.f), bh), false, true, ui::Icon::Close)) {
            AG.st = AddGames::Closed;
            ImGui::CloseCurrentPopup();
        }
    } else if (AG.st == AddGames::Importing) {
        uint64_t done = AG.fake_progress ? 3200ull << 20 : AG.job.bytes_done(), total = AG.fake_progress ? 4700ull << 20 : AG.job.bytes_total();
        int idx = AG.fake_progress ? 2 : AG.job.file_index(), cnt = AG.fake_progress ? 3 : AG.job.file_count();
        std::string cur = AG.fake_progress ? "Super Smash Bros. Melee (USA).iso" : AG.job.current_name();
        text_wrapped_disabled("Copying into your games folder. Big GameCube and 3DS images can take a minute. You can keep using RetroHub.");
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
        float frac = total ? (float)((double)done / (double)total) : 0.f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ui::ACCENT);
        ImGui::ProgressBar(frac, ImVec2(-1, sp(2.5f)), "");
        ImGui::PopStyleColor();
        ImGui::Text("File %d of %d:  %s", std::max(1, idx), cnt, cur.c_str());
        ImGui::TextDisabled("%s of %s", bytes_text(done).c_str(), bytes_text(total).c_str());
        ImGui::Dummy(ImVec2(0, sp(2.f)));
        if (!AG.fake_progress && button("Cancel", ImVec2(sp(14.f), bh), false, true, ui::Icon::Close)) AG.job.cancel();
    } else {   // Done
        std::string text = ag::summary_text(AG.result);
        ImGui::PushTextWrapPos(0.f);
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos);
            std::string ln = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            bool good = ln.compare(0, 5, "Added") == 0;
            bool bad = ln.compare(0, 9, "Could not") == 0;
            ImGui::PushStyleColor(ImGuiCol_Text, good ? ui::GOOD : bad ? ui::BAD : ui::TEXT2);
            ImGui::TextWrapped("%s", ln.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, sp(0.4f)));
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
        if (button("Done", ImVec2(sp(16.f), bh), true)) { AG.st = AddGames::Closed; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(0, sp(1.f));
        if (button("Add more", ImVec2(sp(16.f), bh), false, true, ui::Icon::Plus)) AG.st = AddGames::Choose;
    }
    ImGui::EndPopup();
}

void add_games_shutdown() { AG.job.cancel(); AG.job.join(); }

void draw_add_games() {
    if (const char* sh = std::getenv("HUB_SHOT_ADDGAMES")) {   // screenshot aid
        if (!AG.shot_tried && A.scanned_once) {
            AG.shot_tried = true;
            std::string m = sh;
            if (m == "dialog") AG.st = AddGames::Choose;
            else if (m == "progress") { AG.fake_progress = true; AG.st = AddGames::Importing; }
            else if (m == "results" || m == "toast") {
                auto mk = [](const char* t, ag::Outcome o, bool other = false) { ag::Item i; i.title = t; i.outcome = o; i.other = other; i.src = t; return i; };
                AG.result = {mk("Super Smash Bros. Melee", ag::Outcome::Added), mk("Pokemon Emerald", ag::Outcome::Added),
                             mk("Mario Kart 64", ag::Outcome::Added), mk("Zelda: Ocarina of Time", ag::Outcome::Duplicate)};
                AG.st = AddGames::Done;
                if (m == "toast") {
                    AG.st = AddGames::Closed;
                    push_toast(ag::summary_text(AG.result), ui::GOOD, 60.0);
                }
            }
        }
    }
    if (!AG.drops.empty()) {   // a drop arrives as one event per path: handle them together
        std::vector<std::string> d;
        d.swap(AG.drops);
        add_games_paths(d);
    }
    if (AG.st == AddGames::Importing && !AG.fake_progress && AG.job.done()) finish_import();
    if (!AG.queue.empty() && AG.st != AddGames::Importing) {
        std::vector<std::string> f;
        f.swap(AG.queue);
        start_import(std::move(f));
    }
    if (AG.rescan_pending && !A.scanning) { AG.rescan_pending = false; start_scan(); }
    if (AG.st != AddGames::Closed) add_games_modal();
}
