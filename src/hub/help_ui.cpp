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

void finish_setup_card() {
    static bool dismissed = false;   // this run only: it returns next time while steps stay open
    if (dismissed || wizard_state() != -1) return;
    hubhelp::SetupInputs in;
    in.setup_done = A.st.setup_done;
    in.have_games = !A.lib.empty() || !A.st.rom_folders.empty();
    in.username_claimed = A.st.username_claimed;
    in.net_probe_done = A.st.net_probe_done;
    in.have_pad = !A.pads.empty();
    std::vector<std::string> steps = hubhelp::open_steps(in);
    if (steps.empty()) return;
    begin_panel("finish_setup", ImVec2(0, 0));
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Finish setup");
    ImGui::PopFont();
    std::string line;
    for (auto& s : steps) line += (line.empty() ? "" : "  -  ") + s;
    text_wrapped_disabled(line);
    if (button("Continue setup", ImVec2(0, 0), true, true, ui::Icon::ChevronR)) wizard_open();
    ImGui::SameLine();
    if (button("Not now", ImVec2(0, 0))) dismissed = true;
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
}
