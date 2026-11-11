/* Settings > Help: FAQ, plus the "Finish setup" card on Home while first-run steps are open. */
#include "convenience.h"
#include "help_logic.h"

namespace {

struct Faq { const char* q; const char* a; };

const Faq kFaq[] = {
    {"Why is a game greyed out?",
     "A greyed-out game is one a friend has that you do not. Only its name is shared, never the game file. Put your own copy in one of your game folders "
     "(Settings > General > Add games) and it becomes a normal tile. Turn friends' games off under Settings > Online or with the Friends' games chip."},
    {"My game is missing from the library",
     "RetroHub looks in the folders listed under Settings > General. Add the folder, then press Rescan (or press Ctrl+K and type rescan). Zipped and 7z games are unpacked automatically. "
     "Find my games can search your PC for game files."},
    {"Where are my saves?",
     "Your saves live in one folder on this PC, the save vault. Open the Saves page and choose Open vault folder to see it. "
     "With your RetroHub account, your saves are also backed up to the cloud after you play, and they follow you to any PC where you sign in "
     "(Settings > Account > Add this account to another PC). If two PCs changed the same save, the newer one is used. "
     "On the Saves page you can also export all your saves to a zip file, or copy them to a folder you choose."},
    {"A game says it needs a BIOS",
     "PlayStation and PlayStation 2 games need a console BIOS file that you supply. Put it next to your games, or choose its folder under Settings > Video & Games. "
     "PlayStation can also use the built-in open BIOS (less compatible)."},
    {"My controller does not work or the buttons are wrong",
     "Open Controllers in the sidebar to see every pad and remap buttons. If nothing shows up, unplug and replug it, or run Settings > General > Run setup again. "
     "Hints on screen follow your pad: Xbox, PlayStation or Switch labels. The keyboard and mouse always work."},
    {"I cannot connect to friends or rooms",
     "Run Settings > Online > Test my connection. If it says UDP looks blocked, check your firewall or VPN. Choose Allow when Windows asks about RetroHub, n64run and retrorun "
     "(the button under Test my connection asks it for you now, in front of RetroHub). Games fall back to the relay when a direct connection fails, which only adds a little delay. "
     "Everyone in a room needs the same RetroHub version."},
    {"What are ROM hacks and translations?",
     "Open a game, then choose Add a hack (or press Ctrl+K and type hack). Pick the patch file; RetroHub applies it to a copy and keeps your original untouched. "
     "Hacks show up as their own tile with a HACK tag."},
    {"What is Party mode?",
     "A playlist of games for a room or a couch, with a score and a winner each round. Open Play Online and choose Couch party, or host a room and turn Party mode on. "
     "Press Ctrl+K and type party to jump there."},
    {"How do I find anything quickly?",
     "Just start typing on Home or All Games to search (F, I and S keep their shortcuts; press / to start with those letters). Press Ctrl+K (or click the left stick on a controller) "
     "for the command palette: games, friends, settings and actions."},
    {"Where do I send a problem report?",
     "Use Copy diagnostics below. It saves a zip with your logs and version numbers on the Desktop. Your account files are never included."},
};

}  // namespace

void help_tab_ui() {
    begin_panel("set_help");
    section_title("Help");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    for (auto& f : kFaq) {
        if (ImGui::CollapsingHeader(f.q)) {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextUnformatted(f.a);
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
        }
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::Separator();
    diag_settings_ui();
    end_panel();
}

/* ---- scan result: one quiet toast when a scan ends, and the line the empty Home card keeps ---- */

namespace {
std::string g_scan_last_line;     // "Found 12 games in 2 folders." / "Looked in ...: no game files found." ("" until a scan with folders ended)
double g_scan_started = 0.0;
std::string home_dir() { const char* h = std::getenv("USERPROFILE"); return h ? h : ""; }
}  // namespace

/* Called every frame from the library top bar (the only place a scan is shown). */
void scan_result_tick() {
    static bool inited = false, prev = false;
    static int completed = 0;
    if (!inited) { inited = true; prev = A.scanning; if (prev) g_scan_started = ImGui::GetTime(); }
    const bool now_scanning = A.scanning;
    if (now_scanning && !prev) g_scan_started = ImGui::GetTime();
    if (!now_scanning && prev) {
        ++completed;
        std::vector<std::string> paths;
        for (const hub::GameEntry& e : A.lib) if (e.key != kNativeKey && !e.path.empty()) paths.push_back(e.path);
        const hubhelp::ScanSummary s = hubhelp::scan_summary(paths, A.st.rom_folders, home_dir());
        g_scan_last_line = s.text;
        /* the start-up scan of a library that has games stays quiet; every later scan, and an empty one, says what it found. Never while a game runs. */
        const bool quiet = completed == 1 && !s.none;
        if (!s.text.empty() && !quiet && !game_active()) push_toast(s.text, s.none ? ui::ACCENT : ui::GOOD, 6.0);
    }
    prev = now_scanning;
}
const std::string& scan_last_line() { return g_scan_last_line; }
double scan_elapsed_s() { return A.scanning ? std::max(0.0, ImGui::GetTime() - g_scan_started) : 0.0; }
std::string scan_home_dir() { return home_dir(); }

void finish_setup_card() {
    static bool dismissed = false;   // this run only: it returns next time while steps stay open
    if (dismissed || wizard_state() != -1) return;
    hubhelp::SetupInputs in;
    in.setup_done = A.st.setup_done;
    in.have_games = !A.lib.empty() || !A.st.rom_folders.empty();
    in.username_claimed = A.st.username_claimed;
    in.net_probe_done = A.st.net_probe_done;
    in.have_pad = !A.pads.empty();
    {   // Home's "Add your game folders" card is showing: it already asks for games, so this card does not
        bool own_empty = true;
        for (const hub::GameEntry& e : A.lib) if (e.key != kNativeKey) { own_empty = false; break; }
        in.empty_card_showing = own_empty && !A.scanning;
    }
    std::vector<std::string> steps = hubhelp::open_steps(in);
    if (steps.empty()) return;
    if (hubhelp::finish_card_compact(ImGui::GetIO().DisplaySize.y, ui::sp(1.f) / 8.f)) {   // a short window: one line, never cut off by the bottom bar
        begin_panel("finish_setup", ImVec2(0, 0));
        const float bw1 = ui::sp(18.f), bw2 = ui::sp(10.f), gap = ui::sp(1.f);
        const float text_w = std::max(ui::sp(10.f), ImGui::GetContentRegionAvail().x - bw1 - bw2 - gap * 2);
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(ui::F.bold);
        const std::string line = ui::ellipsize(ui::F.bold, ui::F.bold->FontSize, "Finish setup:  " + hubhelp::join_steps(steps), text_w);
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - bw1 - bw2 - gap));
        if (button("Continue setup", ImVec2(bw1, 0), true, true, ui::Icon::ChevronR)) wizard_open();
        ImGui::SameLine(0, gap);
        if (button("Not now", ImVec2(bw2, 0))) dismissed = true;
        end_panel();
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        return;
    }
    begin_panel("finish_setup", ImVec2(0, 0));
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Finish setup");
    ImGui::PopFont();
    for (size_t i = 0; i < steps.size(); ++i) text_wrapped_disabled(std::to_string(i + 1) + ". " + steps[i]);   // short numbered steps, one per line
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (button("Continue setup", ImVec2(0, 0), true, true, ui::Icon::ChevronR)) wizard_open();
    ImGui::SameLine();
    if (button("Not now", ImVec2(0, 0))) dismissed = true;
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
}
