#include "app.h"
#include "findgames.h"
#include "ui_layout_logic.h"
#include "ui_sheet.h"

/* ---------- "Find my games": first-run folder finder (scan on a worker thread, then a checklist) ----------
 * Shown once when rom_folders is empty and the player has not dismissed it; re-opened from Settings > ROM folders.
 * The scan only reads directory listings (see src/common/findgames.h). Screenshot aids:
 * HUB_SHOT_FINDGAMES=results|scanning opens it on its own, HUB_FINDGAMES_HOME=<dir> points the scan at a fake profile.
 * A screenshot run (headless) NEVER scans this PC: without HUB_FINDGAMES_HOME it shows made-up folders and reads no folder at all. */

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

/* A folder's counts as separate pieces ("N64 3", "GBA 2", "7 zips / 7z / rar"), so a row can wrap between them. */
std::vector<std::string> count_pieces(const hub::fg::Counts& c) {
    std::vector<std::string> v;
    for (int i = 0; i < hub::fg::kSysCount; ++i)
        if (c.n[i]) v.push_back(std::string(hub::fg::sys_label(i)) + " " + std::to_string(c.n[i]));
    if (c.archives) v.push_back(std::to_string(c.archives) + (c.archives == 1 ? " zip / 7z / rar" : " zips / 7z / rar"));
    return v;
}

/* Screenshot runs only: made-up results (a mixed folder with a long row of counts, and enough rows to scroll on a small window). */
void shot_fake_folders(std::vector<hub::fg::Folder>* out) {
    namespace f = hub::fg;
    struct R { const char* path; int sys[3][2]; int arch; bool tick; };
    static const R rows[] = {
        {"C:\\Users\\Player\\Documents\\Emulation\\Everything", {{f::N64, 12}, {f::SNES, 21}, {f::NES, 30}}, 7, true},
        {"C:\\Users\\Player\\Desktop\\N64 Collection", {{f::N64, 24}, {-1, 0}, {-1, 0}}, 0, true},
        {"C:\\Users\\Player\\Downloads\\Handhelds", {{f::GBA, 6}, {f::NDS, 5}, {f::N3DS, 2}}, 3, true},
        {"D:\\Games\\PlayStation 2", {{f::PS2, 8}, {-1, 0}, {-1, 0}}, 0, true},
        {"D:\\ROMs", {{f::SNES, 40}, {f::NES, 52}, {f::GB, 18}}, 12, true},
        {"C:\\Users\\Player\\Desktop\\Old backups\\GameCube discs", {{f::GC, 5}, {-1, 0}, {-1, 0}}, 0, false},
        {"C:\\Users\\Player\\Games\\Game Boy Color", {{f::GBC, 14}, {-1, 0}, {-1, 0}}, 0, true},
        {"C:\\Users\\Player\\Documents\\PS1 discs", {{f::PS1, 4}, {-1, 0}, {-1, 0}}, 0, true},
        {"E:\\Emulation\\Wii U", {{f::WIIU, 3}, {-1, 0}, {-1, 0}}, 0, true},
        {"C:\\Users\\Player\\Downloads\\New zips", {{-1, 0}, {-1, 0}, {-1, 0}}, 9, false},
    };
    out->clear();
    for (const R& r : rows) {
        f::Folder fo;
        fo.path = r.path;
        for (const auto& s : r.sys) if (s[0] >= 0) fo.c.n[s[0]] = s[1];
        fo.c.archives = r.arch;
        fo.ticked = r.tick;
        out->push_back(fo);
    }
    out->front().c.n[f::GBA] = 9;   // the first row also has Game Boy Advance, Game Boy Color, PlayStation and GameCube games
    out->front().c.n[f::GBC] = 4;
    out->front().c.n[f::PS1] = 3;
    out->front().c.n[f::GC] = 2;
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

/* One checklist row's text: the folder name, its count pieces beside it while they fit and wrapped under it otherwise (one long line ran
 * past the right edge and was cut off), a muted note, then the parent folder. `right` = the row's right edge (screen x). */
void row_text(const std::string& path, const std::vector<std::string>& pieces, const char* note, float right) {
    ImGui::BeginGroup();
    std::string nm, par;
    split_label(path, 0.f, &nm, &par);
    const float x0 = ImGui::GetCursorScreenPos().x, room = std::max(sp(4.f), right - x0);
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted(ui::ellipsize(ui::F.bold, ImGui::GetFontSize(), nm, room).c_str());
    ImGui::PopFont();
    float gap = sp(2.f);                                      // name -> first piece (as before)
    const float gap2 = ImGui::CalcTextSize("   ").x;          // between pieces: the three spaces the one-line text had
    auto piece = [&](const std::string& s, bool muted) {
        const float w = ImGui::CalcTextSize(s.c_str()).x;
        if (hubui::kit::fits_on_line(ImGui::GetItemRectMax().x, gap, w, right)) ImGui::SameLine(0.f, gap);
        const std::string t = ui::ellipsize(ImGui::GetFont(), ImGui::GetFontSize(), s, std::max(sp(4.f), right - ImGui::GetCursorScreenPos().x));
        if (muted) ImGui::TextDisabled("%s", t.c_str());
        else ImGui::TextUnformatted(t.c_str());
        gap = gap2;
    };
    for (const std::string& s : pieces) piece(s, false);
    if (note) { gap = sp(2.f); piece(note, true); }
    if (!par.empty()) ImGui::TextDisabled("%s", ui::ellipsize(ImGui::GetFont(), ImGui::GetFontSize(), par, room).c_str());
    ImGui::EndGroup();
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
    if (FG.run.th.joinable()) FG.run.join();
    if (g_headless) {   // a screenshot run never reads this PC's folders: a made-up profile (HUB_FINDGAMES_HOME) or made-up results
        const char* home = std::getenv("HUB_FINDGAMES_HOME");
        if (!home || !*home) {
            shot_fake_folders(&FG.found);
            shot_trace("findgames", "made-up folders (screenshot run: no folder is read)");
            finish_scan();
            return;
        }
        shot_trace("findgames", std::string("scan of the made-up profile ") + home);
    }
    FG.st = FindGames::Scanning;
    FG.run.start([] { FG.found = hub::fg::scan_roots(hub::fg::default_roots(), &FG.run.ctl); });
}

/* A ui::dialog (ui_sheet.h): the folder list is the dialog's scrolling middle (it used to be a nested list with its own scrollbar) and the
 * Add / Add another folder / Skip row stays put. Up while FG.st says so (open = nullptr); B stops a scan, or closes the results. */
static void find_games_modal() {
    ui::DialogOpts o;
    o.back_closes = false;
    o.keep_id = true;
    if (!ui::dialog_begin("Find my games", "Find my games", nullptr, ui::DialogSize::Large, o)) return;
    if (ui::modal_back()) {
        if (FG.st == FindGames::Scanning) FG.run.ctl.cancel = true;
        else { close_flow(true); ImGui::CloseCurrentPopup(); ui::dialog_end(); return; }
    }

    const char* skip_label = FG.first_run ? "Skip" : "Close";
    const ImVec2 bsz(sp(22.f), sp(5.5f));

    if (FG.st == FindGames::Scanning) {
        text_wrapped_disabled("Looking through the usual places on this PC (Desktop, Documents, Downloads, Games and ROMs folders, "
                              "your emulators' game folders). Only folder listings are read; nothing is opened, moved or uploaded.");
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, sp(2.f)), "");
        ImGui::TextWrapped("%d folders checked, %d game files found so far", FG.run.ctl.dirs.load(), FG.run.ctl.games.load());
        std::string cur;
        { std::lock_guard<std::mutex> lk(FG.run.ctl.mu); cur = FG.run.ctl.current; }
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("%s", cur.size() > 90 ? ("..." + cur.substr(cur.size() - 87)).c_str() : cur.c_str());
        ImGui::PopTextWrapPos();
        ui::dialog_buttons();
        if (button("Stop and show what was found", ImVec2(sp(30.f), bsz.y), true)) FG.run.ctl.cancel = true;
        ui::dialog_same_line(bsz.x);
        if (button(skip_label, bsz)) { close_flow(true); ImGui::CloseCurrentPopup(); }
    } else {
        const bool any = !FG.found.empty() || !FG.extra.empty();
        if (!FG.found.empty())
            text_wrapped_disabled("These folders hold game files. Tick the ones to add; RetroHub shows everything inside them. "
                                  "Your files stay where they are.");
        else
            text_wrapped_disabled("No game files turned up in the usual places. If your games live somewhere else, add that folder yourself.");
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        if (any) {   // the rows on a panel, drawn behind them once their height is known
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
            const ImVec2 lp0 = ImGui::GetCursorScreenPos();
            const float lw = ImGui::GetContentRegionAvail().x;
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            ImGui::Indent(sp(1.f));
            const float row_right = lp0.x + lw - sp(1.f);   // the panel's inner right edge (the same margin as on the left)
            for (size_t i = 0; i < FG.found.size(); ++i) {
                ImGui::PushID((int)i);
                const bool have = already_added(FG.found[i].path);
                ImGui::BeginDisabled(have);
                bool t = FG.tick[i] != 0;
                if (ImGui::Checkbox("##t", &t)) FG.tick[i] = t;
                ImGui::EndDisabled();
                ImGui::SameLine();
                row_text(FG.found[i].path, count_pieces(FG.found[i].c), have ? "already in your library" : nullptr, row_right);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", FG.found[i].path.c_str());
                ImGui::PopID();
            }
            for (size_t i = 0; i < FG.extra.size(); ++i) {
                ImGui::PushID(1000 + (int)i);
                bool t = FG.extra_tick[i] != 0;
                if (ImGui::Checkbox("##t", &t)) FG.extra_tick[i] = t;
                ImGui::SameLine();
                row_text(FG.extra[i], {}, "added by you", row_right);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", FG.extra[i].c_str());
                ImGui::PopID();
            }
            ImGui::Unindent(sp(1.f));
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            const ImVec2 lp1(lp0.x + lw, ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y);
            dl->ChannelsSetCurrent(0);
            dl->AddRectFilled(lp0, lp1, ui::PANEL, sp(1.f));
            dl->AddRect(lp0, lp1, ui::HAIRLINE, sp(1.f), 0, 1.f);
            dl->ChannelsMerge();
        }
        int n_ticked = 0;
        for (size_t i = 0; i < FG.found.size(); ++i) n_ticked += FG.tick[i] && !already_added(FG.found[i].path);
        for (size_t i = 0; i < FG.extra.size(); ++i) n_ticked += FG.extra_tick[i];
        char lbl[48];
        std::snprintf(lbl, sizeof lbl, n_ticked == 1 ? "Add %d folder" : "Add %d folders", n_ticked);
        ui::dialog_buttons();
        if (any && button(lbl, bsz, true, n_ticked > 0, ui::Icon::Folder)) {
            int added = 0;
            auto add = [&](const std::string& p) {
                if (!already_added(p)) { A.st.rom_folders.push_back(p); ++added; }
            };
            for (size_t i = 0; i < FG.found.size(); ++i) if (FG.tick[i]) add(FG.found[i].path);
            for (size_t i = 0; i < FG.extra.size(); ++i) if (FG.extra_tick[i]) add(FG.extra[i]);
            if (added) { mark_dirty(); start_scan(true); set_toast("Scanning your games..."); }
            close_flow(true);
            ImGui::CloseCurrentPopup();
        }
        if (any) ui::dialog_same_line(sp(26.f));
        {
            std::string p = osk::folder("fg_extra", button("Add another folder...", ImVec2(sp(26.f), bsz.y), !any, true, ui::Icon::Plus));
            if (!p.empty()) { FG.extra.push_back(p); FG.extra_tick.push_back(1); }
        }
        ui::dialog_same_line(sp(14.f));
        if (button(skip_label, ImVec2(sp(14.f), bsz.y))) { close_flow(true); ImGui::CloseCurrentPopup(); }
    }
    ui::dialog_end();
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
