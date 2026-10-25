#include "app.h"
#include "username_logic.h"

/* ---------- Usernames: picker widget, claim flow, banner + one-time prompt, Settings > Account block ----------
 * Pure rules: net/username.h and username_logic.h (unit tested). The picker is shared by the setup wizard (Name step),
 * Settings > Account > Change username and the "pick a new name" prompt. Typing is debounced, then the friends socket asks
 * the server whether the name is free; the answer decides the status line ("available" / "taken" / "not allowed").
 * Everything works with the on-screen keyboard and the pad (osk::input; suggestions are normal focusable buttons).
 *
 * Screenshot aids (headless, no network): HUB_SHOT_USERNAME=empty|invalid|checking|available|taken|notallowed|offline|claiming
 *   [HUB_SHOT_USERNAME_TEXT=<text>], HUB_SHOT_USERNAME_PROMPT=1 (the one-time prompt), HUB_SHOT_USERNAME_BANNER=1 (the banner),
 *   HUB_SHOT_USERNAME_CHANGE=1 (Settings > Account change panel open), HUB_SHOT_USERNAME_CLAIMED=<name> (pretend this PC owns it). */

using namespace hubui::un;

namespace {

struct UnState {
    Checker chk;
    char buf[24] = {0};
    bool claiming = false;
    int claim_ref = 0;
    std::string err;                 // last claim failure
    std::string last_claim;          // name being claimed
    bool claimed_event = false;      // a claim succeeded (wizard advances)
    bool needs_new = false;          // server flagged this older account
    bool prompt_open = false, prompted = false, banner_hidden = false;
    bool change_open = false;        // Settings > Account change panel
    bool was_connected = false;
    bool seeded = false, shot = false;
    double t_start = 0;
} UN;

void set_text(const std::string& t) {
    std::snprintf(UN.buf, sizeof UN.buf, "%s", t.c_str());
    UN.chk.set_input(UN.buf, ImGui::GetTime());
}

void seed_shot() {
    if (UN.seeded) return;
    UN.seeded = true;
    UN.t_start = ImGui::GetTime();
    if (!g_headless) return;
    if (const char* c = std::getenv("HUB_SHOT_USERNAME_CLAIMED")) { A.st.name = c; A.st.username_claimed = true; UN.chk.mine_key = hubnet::username_key(c); }
    const char* s = std::getenv("HUB_SHOT_USERNAME");
    UN.prompt_open = std::getenv("HUB_SHOT_USERNAME_PROMPT") != nullptr;
    UN.change_open = std::getenv("HUB_SHOT_USERNAME_CHANGE") != nullptr;
    if (!s) return;
    UN.shot = true;
    std::string m = s;
    const char* tx = std::getenv("HUB_SHOT_USERNAME_TEXT");
    std::string text = tx ? tx : (m == "taken" ? "kai" : m == "notallowed" ? "admin" : m == "invalid" ? "k a!" : m == "empty" ? "" : "kai_64");
    set_text(text);
    Checker& c = UN.chk;
    if (m == "available") c.state = Avail::Available;
    else if (m == "taken") { c.state = Avail::Taken; c.suggestions = hubnet::username_suggestions(text, 3); }
    else if (m == "notallowed") { c.state = Avail::NotAllowed; c.suggestions = hubnet::username_suggestions(text, 5); }
    else if (m == "checking") c.state = Avail::Checking;
    else if (m == "offline") c.state = Avail::Offline;
    else if (m == "claiming") { c.state = Avail::Available; UN.claiming = true; }
}

ImU32 tone_col(Tone t) {
    switch (t) {
        case Tone::Good: return ui::GOOD;
        case Tone::Bad: return ui::BAD;
        case Tone::Warn: return ui::WARN;
        default: return ui::TEXT3;
    }
}

}  // namespace

bool un_claimed() { return A.st.username_claimed; }
std::string un_text() { return UN.chk.trimmed(); }
bool un_claiming() { return UN.claiming; }
bool un_take_claimed() { bool v = UN.claimed_event; UN.claimed_event = false; return v; }
bool un_needs_new() { return UN.needs_new; }
Avail un_state() { return UN.chk.state; }
void un_open_prompt() { UN.prompt_open = true; UN.banner_hidden = false; }
void un_open_change() { UN.change_open = true; }
bool un_text_valid() { return !UN.chk.trimmed().empty() && hubnet::username_ok(UN.chk.trimmed()); }

/* Seed the box for the wizard / prompt: the Windows account name, cleaned up, when nothing was typed yet. */
void un_seed_text() {
    seed_shot();
    if (UN.chk.text.empty() && !UN.shot) set_text(A.st.username_claimed ? A.st.name : hubnet::username_sanitize(A.st.name));
}

/* Sends the claim for the name in the box (only valid once it shows "available"). Returns false when it could not start. */
bool un_claim_start() {
    if (UN.claiming) return false;
    const std::string name = UN.chk.trimmed();
    if (!hubnet::username_ok(name)) return false;
    UN.err.clear();
    if (g_headless || FR.fake) {   // screenshot runs: pretend
        UN.claiming = true;
        return true;
    }
    UN.claim_ref = FR.started ? FR.cli.claim_username(name) : 0;
    if (UN.claim_ref == 0) { UN.chk.state = Avail::Offline; UN.err = "Can't reach the server right now."; return false; }
    UN.claiming = true;
    UN.last_claim = name;
    return true;
}

/* Per frame (from fr_tick): connection edges and the debounced availability check. */
void un_tick() {
    seed_shot();
    if (g_headless) return;
    const bool conn = fr_connected();
    if (conn != UN.was_connected) {
        UN.was_connected = conn;
        if (conn) UN.chk.on_connection_back(ImGui::GetTime());
        else { UN.chk.on_connection_lost(); UN.claiming = false; }
    }
    if (UN.chk.want_send(ImGui::GetTime(), conn)) UN.chk.sent(FR.cli.check_username(UN.chk.trimmed()));
}

void un_on_event(const hubnet::Event& e) {
    using K = hubnet::Event::Kind;
    if (e.kind == K::UsernameChecked) {
        UN.chk.on_result(e.ref, e.ok, e.text, e.suggestions);
    } else if (e.kind == K::UsernameResult) {
        if (e.ref != UN.claim_ref && UN.claim_ref != 0) return;
        UN.claiming = false;
        UN.claim_ref = 0;
        if (e.ok) {
            const bool changed = A.st.username_claimed && A.st.name != e.name;
            A.st.name = e.name;
            A.st.username_claimed = true;
            UN.chk.mine_key = hubnet::username_key(e.name);
            UN.needs_new = false;
            UN.claimed_event = true;
            UN.prompt_open = false;
            UN.change_open = false;
            UN.err.clear();
            mark_dirty();
            push_toast(changed ? "Your username is now " + e.name + "." : "You're " + e.name + " now. Welcome!", ui::GOOD, 5.0);
        } else {
            UN.err = claim_error_text(e.text);
            UN.chk.state = (e.text == "invalid" || e.text == "reserved" || e.text == "not-allowed") ? Avail::NotAllowed : Avail::Taken;
            UN.chk.suggestions = e.suggestions.empty() ? hubnet::username_suggestions(UN.last_claim) : e.suggestions;
        }
    } else if (e.kind == K::UsernameChanged) {
        if (e.ok && !e.name.empty()) {
            if (A.st.name != e.name || !A.st.username_claimed) { A.st.name = e.name; A.st.username_claimed = true; mark_dirty(); }
            UN.chk.mine_key = hubnet::username_key(e.name);
            UN.needs_new = false;
        } else {
            UN.needs_new = FR.cli.needs_new_name();
            if (UN.needs_new && A.st.username_claimed) { A.st.username_claimed = false; mark_dirty(); }
            if (UN.needs_new && hubui::un::should_prompt(true, UN.prompted)) { UN.prompted = true; un_seed_text(); UN.prompt_open = true; }
        }
    }
}

/* The box, the live status line and the suggestion chips. width 0 = fill. Returns true when Enter was pressed in the box. */
bool un_picker(const char* id, float width) {
    seed_shot();
    ImGui::PushID(id);
    bool enter = false;
    ImGui::SetNextItemWidth(width > 0 ? width : sp(34.f));
    if (UN.chk.text.empty() && UN.buf[0] == 0) un_seed_text();
    enter = osk::input("##uname", "your username", UN.buf, sizeof UN.buf, ImGuiInputTextFlags_EnterReturnsTrue);
    if (std::string(UN.buf) != UN.chk.text) UN.chk.set_input(UN.buf, ImGui::GetTime());
    ImGui::SameLine(0, sp(1.f));
    ImGui::TextDisabled("%zu/%d", hubnet::username_trim(UN.buf).size(), hubnet::kUsernameMax);

    // status line
    const Avail a = UN.chk.state;
    const std::string st = UN.claiming ? "Claiming..." : (!UN.err.empty() && (a == Avail::Taken || a == Avail::NotAllowed) ? UN.err : status_text(UN.chk));
    const ImU32 col = tone_col(UN.claiming ? Tone::Neutral : status_tone(a));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float lh = ImGui::GetTextLineHeightWithSpacing();
    const bool good = !UN.claiming && (a == Avail::Available || a == Avail::Yours);
    float ix = 0;
    if (good) { ui::icon(dl, ui::Icon::Check, ImVec2(p.x + sp(1.0f), p.y + lh * 0.5f), sp(1.8f), col); ix = sp(2.4f); }
    ImGui::Dummy(ImVec2(0, lh));
    if (!st.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + ix, p.y));
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::PushTextWrapPos(p.x + (width > 0 ? width : sp(46.f)));
        ImGui::TextUnformatted(st.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    if (shows_suggestions(a) && !UN.chk.suggestions.empty() && !UN.claiming) {
        ImGui::TextDisabled("Try one of these:");
        for (size_t i = 0; i < UN.chk.suggestions.size(); ++i) {
            ImGui::PushID((int)i);
            if (i) ImGui::SameLine(0, sp(1.f));
            if (ui::button(UN.chk.suggestions[i].c_str(), ImVec2(0, sp(4.25f)))) set_text(UN.chk.suggestions[i]);
            ImGui::PopID();
        }
    }
    ImGui::PopID();
    return enter;
}

/* Settings > Account: the username, Change username (with the hold rule), then the caller continues with link codes. */
void un_account_block() {
    seed_shot();
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Username");
    ImGui::PopFont();
    if (A.st.username_claimed) {
        ImGui::PushFont(ui::F.h2);
        ImGui::TextUnformatted(A.st.name.c_str());
        ImGui::PopFont();
        text_wrapped_disabled("Your username is your account. Friends find you by it and your cloud saves follow it, on every PC you add.");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("You haven't claimed a username yet.");
        ImGui::PopStyleColor();
        text_wrapped_disabled("Until you do, anyone could take the name you want. It only takes a moment.");
    }
    if (!UN.change_open) {
        if (ui::button(A.st.username_claimed ? "Change username" : "Pick a username", ImVec2(0, 0), !A.st.username_claimed, true, ui::Icon::Gear)) {
            UN.change_open = true;
            un_seed_text();
        }
    } else {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (UN.chk.mine_key.empty() && A.st.username_claimed) UN.chk.mine_key = hubnet::username_key(A.st.name);
        const bool enter = un_picker("acct", sp(34.f));
        const NameAction act = name_action(UN.chk.state, false, UN.claiming);
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        const bool can = act == NameAction::Claim;
        if (ui::button(name_action_label(act == NameAction::Disabled ? NameAction::Claim : act, UN.claiming).c_str(), ImVec2(sp(18.f), sp(5.f)), true, can) || (enter && can)) un_claim_start();
        ImGui::SameLine();
        if (ui::button("Cancel", ImVec2(sp(12.f), sp(5.f)))) { UN.change_open = false; UN.err.clear(); }
        ImGui::PushTextWrapPos(0.f);
        text_wrapped_disabled(hold_rule_text());
        ImGui::PopTextWrapPos();
    }
}

/* Banner (no confirmed username yet) and the one-time prompt modal. Called from draw_overlays(). */
void draw_username_overlays() {
    seed_shot();
    if (wizard_state() != -1) return;   // the wizard has its own Name step
    const bool show_shot = g_headless && std::getenv("HUB_SHOT_USERNAME_BANNER");
    if (g_headless && !show_shot && !UN.prompt_open && !ImGui::IsPopupOpen("Pick your username##un")) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    const bool online = fr_connected();
    const std::string bt = banner_text(A.st.username_claimed, UN.needs_new, online);
    const bool want_banner = !bt.empty() && !UN.banner_hidden && !UN.prompt_open && (show_shot || (A.st.setup_done && ImGui::GetTime() - UN.t_start > 4.0));
    if (want_banner) {
        const float w = std::min(sp(88.f), vp->Size.x - sp(4.f)), h = sp(7.5f);
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f + sp(14.f), vp->Pos.y + vp->Size.y - sp(9.f)), ImGuiCond_Always, ImVec2(0.5f, 1.f));
        ImGui::SetNextWindowSize(ImVec2(w, h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.5f), sp(1.f)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::PANEL2);
        ImGui::PushStyleColor(ImGuiCol_Border, ui::ACCENT);
        if (ImGui::Begin("##un_banner", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::PushTextWrapPos(w - sp(30.f));
            ImGui::SetCursorPosY((h - ImGui::GetTextLineHeight() * 2.f) * 0.5f);
            ImGui::TextWrapped("%s", bt.c_str());
            ImGui::PopTextWrapPos();
            ImGui::SetCursorPos(ImVec2(w - sp(25.f), (h - sp(4.25f)) * 0.5f));
            if (ui::button("Pick username", ImVec2(sp(14.f), sp(4.25f)), true)) { un_seed_text(); UN.prompt_open = true; }
            ImGui::SameLine(0, sp(0.5f));
            if (ui::icon_button("un_hide", ui::Icon::Close, sp(4.25f), false, "Later")) UN.banner_hidden = true;
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    if (UN.prompt_open) { ImGui::OpenPopup("Pick your username##un"); UN.prompt_open = false; UN.change_open = false; }
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(2.5f)));
    bool open = true;
    if (ImGui::BeginPopupModal("Pick your username##un", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (ui::modal_back()) ImGui::CloseCurrentPopup();
        ImGui::PushTextWrapPos(sp(46.f));
        ImGui::PushFont(ui::F.h2);
        ImGui::TextUnformatted(UN.needs_new ? "Time for a username" : "Pick your username");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::TextWrapped("%s", UN.needs_new
            ? "RetroHub now gives everyone a unique username. It replaces the old display name: friends find you by it and your cloud saves follow it to any PC. Your friends and saves stay exactly as they are."
            : "Your username is your RetroHub account. Friends find you by it and your cloud saves follow it to any PC. Only you can use it.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        const bool enter = un_picker("prompt", sp(34.f));
        const NameAction act = name_action(UN.chk.state, false, UN.claiming);
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        const bool can = act == NameAction::Claim;
        if (ui::button(name_action_label(act == NameAction::Disabled ? NameAction::Claim : act, UN.claiming).c_str(), ImVec2(sp(18.f), sp(5.f)), true, can) || (enter && can)) un_claim_start();
        ImGui::SameLine();
        if (ui::button("Later", ImVec2(sp(12.f), sp(5.f)))) ImGui::CloseCurrentPopup();
        if (UN.claimed_event && A.st.username_claimed) { UN.claimed_event = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}
