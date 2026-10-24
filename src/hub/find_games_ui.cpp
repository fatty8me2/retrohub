#include "app.h"
#include "findgames.h"

/* ---------- "Find my games": first-run folder finder (scan on a worker thread, then a checklist) ----------
 * Shown once when rom_folders is empty and the player has not dismissed it; re-opened from Settings > ROM folders.
 * The scan only reads directory listings (see src/common/findgames.h). Screenshot aids:
 * HUB_SHOT_FINDGAMES=results|scanning opens it on its own, HUB_FINDGAMES_HOME=<dir> points the scan at a fake profile. */

namespace {

struct FindGames {
    enum St { Idle, Scanning, Results } st = Idle;
    bool auto_tried = false, fake_scan = false, first_run = true;
    hub::fg::ScanRunner run;   // worker thread + control; joins on destruction
    std::vector<hub::fg::Folder> found;
    std::vector<char> tick;
    std::vector<std::string> extra;   // folders the player added by hand
    std::vector<char> extra_tick;
};
FindGames FG;

}  // namespace

void find_games_shutdown() { FG.run.stop(); }   // app exit: cancel + join a scan still running

namespace {

bool already_added(const std::string& p) {
    std::string n = hub::fg::norm_path(p);
    for (const std::string& f : A.st.rom_folders) if (hub::fg::norm_path(f) == n) return true;
    return false;
}

void finish_scan() {
    if (FG.run.th.joinable()) FG.run.join();
    FG.tick.assign(FG.found.size(), 0);
    for (size_t i = 0; i < FG.found.size(); ++i) FG.tick[i] = FG.found[i].ticked && !already_added(FG.found[i].path);
    FG.st = FindGames::Results;
}

std::string counts_text(const hub::fg::Counts& c) {
    std::string s;
    for (int i = 0; i < hub::fg::kSysCount; ++i)
        if (c.n[i]) s += (s.empty() ? "" : "   ") + std::string(hub::fg::sys_label(i)) + " " + std::to_string(c.n[i]);
    if (c.archives) s += (s.empty() ? "" : "   ") + std::to_string(c.archives) + (c.archives == 1 ? " zip / 7z" : " zips / 7z");
    return s;
}

/* "C:\Users\Me\Desktop\Emulation\ROMs" -> name "ROMs" + parent shortened to "...\<folder>" (the last two segments; callers show the full path on hover). */
void split_label(const std::string& p, float w, std::string* name, std::string* parent) {
    std::string t = p;
    while (t.size() > 3 && (t.back() == '\\' || t.back() == '/')) t.pop_back();
    size_t cut = t.find_last_of("\\/");
    *name = cut == std::string::npos ? t : t.substr(cut + 1);
    if (name->empty()) *name = t;
    std::string full = cut == std::string::npos || cut == 0 ? std::string() : t.substr(0, cut);
    /* keep only the last two segments ("...\Emulation\ROMs"): the full path is shown on hover */
    size_t cut2 = full.find_last_of("\\/");
    std::string up = cut2 == std::string::npos ? full : full.substr(cut2 + 1);
    *parent = (cut2 == std::string::npos || cut2 == 0) ? full : "...\\" + up;
    (void)w;
}

void close_flow(bool done) {
    if (FG.run.th.joinable()) { FG.run.stop(); }
    FG.st = FindGames::Idle;
    if (done && !A.st.find_games_done) { A.st.find_games_done = true; mark_dirty(); }
}

}   // namespace

void find_games_open() {
    if (FG.st == FindGames::Scanning) return;
    FG.first_run = A.st.rom_folders.empty();
    FG.found.clear();
    FG.tick.clear();
    FG.extra.clear();
    FG.extra_tick.clear();
    FG.run.ctl.cancel = false;
    FG.run.ctl.done = false;
    FG.run.ctl.dirs = 0;
    FG.run.ctl.games = 0;
    FG.st = FindGames::Scanning;
    if (FG.run.th.joinable()) FG.run.join();
    FG.run.start([] { FG.found = hub::fg::scan_roots(hub::fg::default_roots(), &FG.run.ctl); });
}

static void find_games_modal() {
    ImGui::OpenPopup("Find my games");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(80.f), ImGui::GetIO().DisplaySize.x * 0.94f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal("Find my games", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleVar(2);
    if (!open) return;

    ImGui::PushFont(ui::F.title);
    ImGui::TextUnformatted("Find my games");
    ImGui::PopFont();
    const char* skip_label = FG.first_run ? "Skip" : "Close";
    const ImVec2 bsz(sp(22.f), sp(5.5f));

    if (FG.st == FindGames::Scanning) {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        text_wrapped_disabled("Looking through the usual places on this PC (Desktop, Documents, Downloads, Games and ROMs folders, "
                              "your emulators' game folders). Only folder listings are read; nothing is opened, moved or uploaded.");
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, sp(2.f)), "");
        ImGui::Text("%d folders checked, %d game files found so far", FG.run.ctl.dirs.load(), FG.run.ctl.games.load());
        std::string cur;
        { std::lock_guard<std::mutex> lk(FG.run.ctl.mu); cur = FG.run.ctl.current; }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextDisabled("%s", cur.size() > 90 ? ("..." + cur.substr(cur.size() - 87)).c_str() : cur.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, sp(2.f)));
        if (button("Stop and show what was found", ImVec2(sp(30.f), bsz.y), true)) FG.run.ctl.cancel = true;
        ImGui::SameLine();
        if (button(skip_label, bsz)) close_flow(true);
    } else {
        const bool any = !FG.found.empty() || !FG.extra.empty();
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (!FG.found.empty())
            text_wrapped_disabled("These folders hold game files. Tick the ones to add; RetroHub shows everything inside them. "
                                  "Your files stay where they are.");
        else
            text_wrapped_disabled("No game files turned up in the usual places. If your games live somewhere else, add that folder yourself.");
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        if (any) {
            float rows = (float)(FG.found.size() + FG.extra.size());
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::PANEL);
            ImGui::BeginChild("fg_list", ImVec2(0, std::min(sp(56.f), rows * sp(14.f) + sp(2.f))), ImGuiChildFlags_Borders);
            for (size_t i = 0; i < FG.found.size(); ++i) {
                ImGui::PushID((int)i);
                const bool have = already_added(FG.found[i].path);
                ImGui::BeginDisabled(have);
                bool t = FG.tick[i] != 0;
                if (ImGui::Checkbox("##t", &t)) FG.tick[i] = t;
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::PushFont(ui::F.bold);
                std::string nm, par;
                split_label(FG.found[i].path, ImGui::GetContentRegionAvail().x - sp(2.f), &nm, &par);
                ImGui::TextUnformatted(nm.c_str());
                ImGui::PopFont();
                ImGui::SameLine(0, sp(2.f));
                ImGui::TextUnformatted(counts_text(FG.found[i].c).c_str());
                if (have) { ImGui::SameLine(0, sp(2.f)); ImGui::TextDisabled("already in your library"); }
                if (!par.empty()) ImGui::TextDisabled("%s", par.c_str());
                ImGui::EndGroup();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", FG.found[i].path.c_str());
                ImGui::PopID();
            }
            for (size_t i = 0; i < FG.extra.size(); ++i) {
                ImGui::PushID(1000 + (int)i);
                bool t = FG.extra_tick[i] != 0;
                if (ImGui::Checkbox("##t", &t)) FG.extra_tick[i] = t;
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::PushFont(ui::F.bold);
                std::string nm, par;
                split_label(FG.extra[i], ImGui::GetContentRegionAvail().x - sp(2.f), &nm, &par);
                ImGui::TextUnformatted(nm.c_str());
                ImGui::PopFont();
                ImGui::SameLine(0, sp(2.f));
                ImGui::TextDisabled("added by you");
                if (!par.empty()) ImGui::TextDisabled("%s", par.c_str());
                ImGui::EndGroup();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", FG.extra[i].c_str());
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, sp(1.f)));
        }
        int n_ticked = 0;
        for (size_t i = 0; i < FG.found.size(); ++i) n_ticked += FG.tick[i] && !already_added(FG.found[i].path);
        for (size_t i = 0; i < FG.extra.size(); ++i) n_ticked += FG.extra_tick[i];
        char lbl[48];
        std::snprintf(lbl, sizeof lbl, n_ticked == 1 ? "Add %d folder" : "Add %d folders", n_ticked);
        if (any && button(lbl, bsz, true, n_ticked > 0, ui::Icon::Folder)) {
            int added = 0;
            auto add = [&](const std::string& p) {
                if (!already_added(p)) { A.st.rom_folders.push_back(p); ++added; }
            };
            for (size_t i = 0; i < FG.found.size(); ++i) if (FG.tick[i]) add(FG.found[i].path);
            for (size_t i = 0; i < FG.extra.size(); ++i) if (FG.extra_tick[i]) add(FG.extra[i]);
            if (added) { mark_dirty(); start_scan(); set_toast("Scanning your games..."); }
            close_flow(true);
        }
        if (any) ImGui::SameLine();
        {
            std::string p = osk::folder("fg_extra", button("Add another folder...", ImVec2(sp(26.f), bsz.y), !any, true, ui::Icon::Plus));
            if (!p.empty()) { FG.extra.push_back(p); FG.extra_tick.push_back(1); }
        }
        ImGui::SameLine();
        if (button(skip_label, ImVec2(sp(14.f), bsz.y))) close_flow(true);
    }
    ImGui::EndPopup();
}

void draw_find_games() {
    if (!FG.auto_tried) {   // first run: offer the finder once the (empty) library has loaded
        const char* shot = std::getenv("HUB_SHOT_FINDGAMES");
        if (shot && A.scanned_once) {
            FG.auto_tried = true;
            if (std::string(shot) == "scanning") {   // screenshot aid: a frozen mid-scan state (no worker)
                FG.first_run = true;
                FG.st = FindGames::Scanning;
                FG.run.ctl.dirs = 842;
                FG.run.ctl.games = 37;
                std::lock_guard<std::mutex> lk(FG.run.ctl.mu);
                FG.run.ctl.current = "C:\\Users\\Player\\Documents\\Emulation\\ROMs\\Nintendo 64";
            } else find_games_open();
        } else if (!shot && !g_headless && A.scanned_once) {
            FG.auto_tried = true;
            if (A.st.setup_done && A.st.rom_folders.empty() && !A.st.find_games_done) find_games_open();
        }
    }
    if (FG.st == FindGames::Scanning && FG.run.ctl.done) finish_scan();
    if (FG.st != FindGames::Idle) find_games_modal();
}
