/* Command palette (Ctrl+K, L3 on a pad, or View + Y while the hub is focused) and type-to-search for the library.
 * Searches games, friends, pages / settings and actions with fuzzy matching (palette_logic.h). Opens only from a user action. */
#include "convenience.h"
#include "libview_ui.h"
#include "friends_ui.h"
#include "hack_ui.h"
#include "osk.h"
#include "palette_logic.h"
#include "party_ui.h"
#include "settings_tabs.h"

namespace {

using hubpal::Item;
using hubpal::Kind;

struct Palette {
    bool want_open = false, open = false, focus_input = false;
    char buf[64] = {0};
    int sel = 0;
    std::vector<Item> items;
    std::vector<int> shown;
    std::string last_q = "\x01";
    // View+Y: the View (Back) press already toggled the friends panel; remember the state from before it
    double back_at = -10;
    size_t built_lib = 0;
} P;

void add(Kind k, const char* title, const std::string& sub, const std::string& id) { P.items.push_back({k, title, sub, id}); }

void build_items() {
    P.items.clear();
    add(Kind::Action, "Host a room", "Play Online", "act:host");
    add(Kind::Action, "Join a room with a code", "Play Online", "act:join");
    add(Kind::Action, "Rescan games", "Look for new games", "act:rescan");
    add(Kind::Action, "Party mode", "Couch party", "act:party");
    add(Kind::Action, "Tournament", "Brackets", "act:tournament");
    add(Kind::Action, "Add a hack", "ROM hack or translation for the selected game", "act:hack");
    add(Kind::Action, "Add games", "", "act:addgames");
    add(Kind::Action, "Find my games", "", "act:findgames");
    add(Kind::Action, "Run setup again", "", "act:setup");
    add(Kind::Action, "Copy diagnostics", "Zip on the Desktop", "set:4");
    add(Kind::Action, "Test my connection", "Settings > Online", "set:1");
    add(Kind::Action, "Check for updates", "", "page:about");
    add(Kind::Setting, "Home", "", "page:home");
    add(Kind::Setting, "All Games", "", "page:all");
    add(Kind::Setting, "Discover", "Recommended games for each console", "page:discover");
    add(Kind::Setting, "Play Online", "", "page:online");
    add(Kind::Setting, "Saves", "", "page:saves");
    add(Kind::Setting, "Controllers", "", "page:controllers");
    add(Kind::Setting, "Settings: General", "", "set:0");
    add(Kind::Setting, "Settings: Online", "Discord, connection test", "set:1");
    add(Kind::Setting, "Settings: Video & Games", "", "set:2");
    add(Kind::Setting, "Settings: Account", "", "set:3");
    add(Kind::Setting, "Help and FAQ", "Greyed-out games, BIOS, controllers", "set:4");
    add(Kind::Setting, "Updates", "", "page:about");
    for (auto& g : A.lib) add(Kind::Game, "", std::string(sys_short(g)), "game:" + g.key), P.items.back().title = g.title;
    for (auto& f : fr_friends()) add(Kind::Friend, "", "Friend", "friend:" + f.uid), P.items.back().title = f.name;
}

void run(const std::string& id) {
    auto starts = [&](const char* p) { return id.rfind(p, 0) == 0; };
    if (id == "act:host" || id == "act:join" || id == "page:online") A.page = Page::Online;
    else if (id == "act:rescan") { if (!A.scanning) start_scan(); }
    else if (id == "act:party") party_couch_open();
    else if (id == "act:tournament") bracket_open();
    else if (id == "act:hack") {
        const hub::GameEntry* e = find_entry(A.sel_game);
        if (e && hack_can_add(*e)) hack_add_open(*e);
        else { A.page = Page::Library; A.view = LibView::All; set_toast("Pick a game first, then choose Add a hack.", false, 5.0); }
    }
    else if (id == "act:addgames") add_games_open();
    else if (id == "act:findgames") find_games_open();
    else if (id == "act:setup") wizard_open();
    else if (id == "page:home") { A.page = Page::Library; A.view = LibView::Home; A.search_buf[0] = 0; }
    else if (id == "page:all") { A.page = Page::Library; A.view = LibView::All; }
    else if (id == "page:saves") A.page = Page::Saves;
    else if (id == "page:controllers") A.page = Page::Controllers;
    else if (id == "page:discover") A.page = Page::Discover;
    else if (id == "page:about") A.page = Page::About;
    else if (starts("set:")) settings_open(std::atoi(id.c_str() + 4));
    else if (starts("game:")) {
        A.page = Page::Library;
        A.view = LibView::All;
        A.sel_game = id.substr(5);
        A.show_details = true;   // details, never a launch
    }
    else if (starts("friend:")) fr_open_chat(id.substr(7));
    mark_dirty();
}

const char* kind_tag(Kind k) { return k == Kind::Game ? "GAME" : k == Kind::Friend ? "FRIEND" : k == Kind::Setting ? "PAGE" : "ACTION"; }

}  // namespace

void palette_open() { P.want_open = true; }
bool palette_active() { return P.open; }

/* Library: a typed character that starts a search (not F / I / S, which stay shortcuts; "/" always works). */
bool type_to_search_pending() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.KeyCtrl || io.KeyAlt || P.open || !A.focused || wizard_state() != -1) return false;   // typing to search is fine while a game runs: it only filters the library
    if (ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return false;
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
        if (hubpal::starts_search((unsigned)io.InputQueueCharacters[i])) return true;
    return false;
}

void palette_draw() {
    ImGuiIO& io = ImGui::GetIO();
    if (g_headless) {   // screenshot aid: HUB_SHOT_PALETTE=<query> opens the palette with that text (after the scan and a few frames)
        static bool shot_done = false;
        const char* sq = std::getenv("HUB_SHOT_PALETTE");
        if (sq && !shot_done && A.scanned_once && ImGui::GetFrameCount() > 10) {
            shot_done = true;
            P.want_open = true;
            std::snprintf(P.buf, sizeof P.buf, "%s", sq);
        }
    }
    const bool kbd_free = (A.focused || g_headless) && wizard_state() == -1 && !P.open &&
                          !ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);   // Ctrl+K works during a game (the user alt-tabbed to the hub)
    const bool free_to_open = (A.focused || g_headless) && !A.proc && !game_active() && wizard_state() == -1 && !P.open &&
                              !ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // View + Y (Back held, Y pressed): global_input toggles the friends panel on View RELEASE and skips it after a combo, so nothing to undo
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false)) P.back_at = ImGui::GetTime();
    const bool view_y = ImGui::IsKeyDown(ImGuiKey_GamepadBack) && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) && ImGui::GetTime() - P.back_at < 3.0;
    if ((kbd_free && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) || (free_to_open && ((ImGui::IsKeyPressed(ImGuiKey_GamepadL3, false) && !lib_cases_active()) || view_y))) {   // the pad combos belong to the game (View+Y is its quick menu)
        P.want_open = true;
    }
    if (P.want_open && !P.open) {
        P.want_open = false;
        if (!free_to_open) return;
        P.open = true;
        if (!(g_headless && P.buf[0])) P.buf[0] = 0;
        P.sel = 0;
        P.last_q = "\x01";
        P.focus_input = !A.pad_mode;   // on a pad the field would pop the on-screen keyboard at once: let the player pick from the list or press A on the field
        P.built_lib = A.lib.size();
        build_items();
        ImGui::OpenPopup("##palette");
    }
    P.want_open = false;
    if (!P.open) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = std::min(sp(70.f), vp->Size.x * 0.94f), maxh = std::min(sp(60.f), vp->Size.y * 0.8f);
    ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->Pos.y + vp->Size.y * 0.14f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, maxh));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.f), sp(2.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.f));
    bool open = ImGui::BeginPopupModal("##palette", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar(2);
    if (!open) { P.open = false; return; }
    if (P.built_lib != A.lib.size()) { build_items(); P.built_lib = A.lib.size(); P.last_q = "#rebuild#"; }   // the library finished scanning while the palette was open
    if (ui::modal_back()) { P.open = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }

    if (P.focus_input) { ImGui::SetKeyboardFocusHere(); P.focus_input = false; }
    ImGui::SetNextItemWidth(-1);
    osk::input("##palq", "Search games, friends, settings, actions...", P.buf, sizeof P.buf);
    const std::string q = P.buf;
    if (q != P.last_q) { P.last_q = q; P.shown = hubpal::rank(q, P.items, 12); P.sel = 0; }
    const int n = (int)P.shown.size();
    if (g_headless) shot_trace_once("palette", q + " items=" + std::to_string(P.items.size()) + " shown=" + std::to_string(n) + " lib=" + std::to_string(A.lib.size()));
    if (io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && n) P.sel = (P.sel + 1) % n;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && n) P.sel = (P.sel + n - 1) % n;
    }
    std::string pick;
    if (io.WantTextInput && n && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) pick = P.items[P.shown[P.sel]].id;
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    const float rh = sp(4.5f);
    for (int i = 0; i < n; ++i) {
        const Item& it = P.items[P.shown[i]];
        ImGui::PushID(i);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::Selectable("##row", i == P.sel, 0, ImVec2(0, rh));
        if (ImGui::IsItemHovered() && io.MouseDelta.x + io.MouseDelta.y != 0.f) P.sel = i;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cy = p0.y + rh * 0.5f;
        ui::pill_at(dl, ImVec2(p0.x + sp(1.f), cy - ui::F.cap->FontSize * 0.5f - sp(0.5f)), kind_tag(it.kind), ui::PANEL3, ui::TEXT2);
        dl->AddText(ui::F.body, ui::F.body->FontSize, ImVec2(p0.x + sp(9.f), cy - ui::F.body->FontSize * 0.5f), ui::TEXT,
                    ui::ellipsize(ui::F.body, ui::F.body->FontSize, it.title, w * 0.5f).c_str());
        if (!it.sub.empty()) {
            const float sw = ui::F.cap->CalcTextSizeA(ui::F.cap->FontSize, FLT_MAX, 0.f, it.sub.c_str()).x;
            dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(p0.x + w - sp(5.f) - std::min(sw, w * 0.3f), cy - ui::F.cap->FontSize * 0.5f), ui::TEXT3,
                        ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, it.sub, w * 0.3f).c_str());
        }
        if (clicked) pick = it.id;
        ImGui::PopID();
    }
    if (n == 0) text_wrapped_disabled("Nothing matches. Try a game name, a friend, or words like host, rescan, party.");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float x = ui::key_hint(dl, p, A.pad_mode, "A", "Enter", "Go");
        ui::key_hint(dl, ImVec2(p.x + x + sp(2.f), p.y), A.pad_mode, "B", "Esc", "Close");
        ImGui::Dummy(ImVec2(0, ui::F.cap->FontSize + sp(1.f)));
    }
    if (!pick.empty()) { P.open = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); run(pick); return; }
    ImGui::EndPopup();
}
