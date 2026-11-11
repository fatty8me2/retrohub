#include <algorithm>
#include <cstring>

#include "app.h"
#include "bracket_logic.h"
#include "elo_logic.h"
#include "friends_ui.h"
#include "group_ui.h"
#include "osk.h"

/* Brackets and the friends ladder page (Plan 4 P7-P8). Logic: bracket_logic.h / elo_logic.h (unit tested).
 * Rooms: the host owns the bracket and broadcasts `bk_state` on change only; guests view it. Couch: no room needed, names typed
 * or picked from friends; local Smash results map controller port 1 / 2 to the two players of the current match.
 * Nothing here opens by itself: the page opens only from a user action (bracket_open), so it never covers a running game. */

using namespace hubui::bracket;

namespace {

struct BkUi {
    bool open = false, want_open = false;
    Bracket b;
    bool have = false;
    // setup
    std::vector<Entrant> entrants;
    int fmt = 0;            // 0 single, 1 double
    bool reset = true;
    int seeding = 0;        // 0 rating, 1 random, 2 manual
    char name_buf[32] = {};
    std::string note;
    bool fake = false, shot_champion = false;
    // network bookkeeping
    bool prev_host = false;
    size_t prev_members = 0;
    bool prev_room = false, asked = false;
};
BkUi BK;

bool in_room() { return A.in_room && room_connected(); }
std::string low(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

void broadcast() {
    if (!in_room() || !is_host() || !BK.have) return;
    std::string j = BK.b.to_json();
    if (!j.empty()) A.lobby.send_msg(j);
}
void send_clear() {
    if (in_room() && is_host()) A.lobby.send_msg("{\"kind\":\"bk_off\"}");
}
bool may_edit() { return !in_room() || is_host(); }

bool has_name(const std::string& n) {
    for (auto& e : BK.entrants) if (low(e.name) == low(n)) return true;
    return false;
}
void add_entrant(const std::string& name, const std::string& uid) {
    std::string n = name;
    while (!n.empty() && n.back() == ' ') n.pop_back();
    if (n.empty()) return;
    if (n.size() > kMaxName) n.resize(kMaxName);
    if (BK.entrants.size() >= (size_t)kMaxEntrants) { BK.note = "A bracket holds at most 16 players."; return; }
    if (has_name(n)) { BK.note = n + " is already in."; return; }
    Entrant e; e.name = n; e.uid = uid;
    e.rating = uid.empty() ? 1000 : ladder_rating_of(uid);
    BK.entrants.push_back(e);
    BK.note.clear();
}
void start_bracket() {
    if (BK.entrants.size() < 2) { BK.note = "Add at least 2 players."; return; }
    auto sd = seed_entrants(BK.entrants, BK.seeding == 0 ? Seeding::Rating : BK.seeding == 1 ? Seeding::Random : Seeding::Manual, (uint32_t)wall_ms());
    BK.b = Bracket();
    BK.b.init(BK.fmt ? Format::Double : Format::Single, BK.reset, sd);
    BK.have = true;
    broadcast();
}
void close_bracket() {
    BK.have = false;
    BK.b = Bracket();
    BK.entrants.clear();
    send_clear();
}

/* Seat -> lobby slot, by the member's display name (the host's room list). -1 when not in the room. */
int slot_of_seat(int seat) {
    std::string n = low(BK.b.seat_name(seat));
    for (auto& m : room_members()) if (low(m.name) == n) return m.slot;
    return -1;
}

void apply_result(const hubui::results::RoundResult& r) {
    int c = BK.b.current();
    if (c < 0) return;
    const MState& m = BK.b.state(c);
    int sa, sb;
    if (r.local) { sa = 0; sb = 1; }   // couch: player 1 (port 1) = first listed, player 2 = second
    else { sa = slot_of_seat(m.a.seat); sb = slot_of_seat(m.b.seat); if (sa < 0 || sb < 0) return; }
    if (BK.b.report_result(r, sa, sb)) broadcast();
}

// ---------------------------------------------------------------- drawing
const float kBoxW = 17.f, kBoxH = 5.2f;

void draw_match_box(ImDrawList* dl, ImVec2 p, int id, bool current) {
    const Bracket& b = BK.b;
    const MState& m = b.state(id);
    float w = sp(kBoxW), h = sp(kBoxH);
    ImU32 bg = m.done ? ui::PANEL : ui::PANEL2;
    dl->AddRectFilled(p, p + ImVec2(w, h), bg, sp(0.75f));
    if (current) dl->AddRect(p, p + ImVec2(w, h), ui::ACCENT, sp(0.75f), 0, 2.f);
    for (int s = 0; s < 2; ++s) {
        const Slot& sl = s ? m.b : m.a;
        std::string nm = !sl.known ? "TBD" : sl.seat < 0 ? "BYE" : b.seat_name(sl.seat);
        bool won = m.done && !m.bye && sl.seat >= 0 && sl.seat == m.winner;
        bool lost = m.done && !m.bye && sl.seat >= 0 && sl.seat != m.winner;
        ImU32 col = won ? ui::GOOD : lost ? ui::TEXT3 : (sl.known && sl.seat >= 0 ? ui::TEXT : ui::TEXT3);
        float y = p.y + sp(0.5f) + s * sp(2.2f);
        dl->AddText(ui::F.body, ui::F.body->FontSize, ImVec2(p.x + sp(0.75f), y), col,
                    ui::ellipsize(ui::F.body, ui::F.body->FontSize, nm, w - sp(3.f)).c_str());
        if (won) ui::icon(dl, ui::Icon::Check, ImVec2(p.x + w - sp(1.2f), y + sp(0.9f)), sp(1.3f), ui::GOOD);
    }
    dl->AddLine(p + ImVec2(sp(0.5f), h * 0.5f), p + ImVec2(w - sp(0.5f), h * 0.5f), ui::mix(ui::PANEL3, ui::BG, 0.f));
}

void draw_tree() {
    const Bracket& b = BK.b;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int cur = b.current();
    // columns: W rounds, then G; L rounds on a second band
    int wr = 0, lr = 0;
    for (auto& n : b.nodes()) { if (n.br == 'W') wr = std::max(wr, n.round); if (n.br == 'L') lr = std::max(lr, n.round); }
    float colw = sp(kBoxW + 3.f), rowh = sp(kBoxH + 1.2f);
    int w_rows = std::max(1, b.size() / 2);
    int l_rows = lr ? std::max(1, b.size() / 4) : 0;
    float wh = w_rows * rowh, lh = l_rows * rowh;
    ImVec2 o = ImGui::GetCursorScreenPos();
    int cols = std::max(wr, lr) + (b.format() == Format::Double ? 2 : 0);
    auto count = [&](char br, int round) { int c = 0; for (auto& n : b.nodes()) if (n.br == br && n.round == round) ++c; return c; };
    std::vector<ImVec2> pos(b.nodes().size());
    for (auto& n : b.nodes()) {
        float x, y;
        if (n.br == 'W') { int c = count('W', n.round); x = (n.round - 1) * colw; y = (n.idx + 0.5f) / c * wh - sp(kBoxH) * 0.5f; }
        else if (n.br == 'L') { int c = count('L', n.round); x = (n.round - 1) * colw; y = wh + sp(3.f) + (n.idx + 0.5f) / c * lh - sp(kBoxH) * 0.5f; }
        else { x = (std::max(wr, lr) + n.round - 1) * colw; y = (wh + (lr ? sp(3.f) + lh : 0.f)) * 0.5f - sp(kBoxH) * 0.5f; }
        pos[(size_t)n.id] = o + ImVec2(x, y);
    }
    for (auto& n : b.nodes()) {   // connectors, then boxes
        if (n.br == 'G' && n.round == 2 && b.state(n.id).skipped) continue;
        for (const Src* s : {&n.a, &n.b}) {
            if (s->k != Src::Win) continue;
            ImVec2 a = pos[(size_t)s->v] + ImVec2(sp(kBoxW), sp(kBoxH) * 0.5f), c = pos[(size_t)n.id] + ImVec2(0, sp(kBoxH) * 0.5f);
            float mx = (a.x + c.x) * 0.5f;
            dl->AddPolyline(std::vector<ImVec2>{a, ImVec2(mx, a.y), ImVec2(mx, c.y), c}.data(), 4, ui::PANEL3, 0, 1.5f);
        }
    }
    for (auto& n : b.nodes()) {
        if (n.br == 'G' && n.round == 2 && b.state(n.id).skipped) continue;
        bool dead = b.state(n.id).bye && b.state(n.id).winner < 0;
        if (dead) continue;
        draw_match_box(dl, pos[(size_t)n.id], n.id, n.id == cur);
        if (n.id == cur || n.round == 1) {}
    }
    if (wr) dl->AddText(ui::F.cap, ui::F.cap->FontSize, o + ImVec2(0, -sp(1.2f)), ui::TEXT3, b.format() == Format::Double ? "Winners bracket" : "Bracket");
    if (lr) dl->AddText(ui::F.cap, ui::F.cap->FontSize, o + ImVec2(0, wh + sp(0.6f)), ui::TEXT3, "Losers bracket");
    float total_h = wh + (lr ? sp(3.f) + lh : 0.f);
    ImGui::Dummy(ImVec2((cols + 0.5f) * colw, total_h + sp(1.f)));
}

void draw_setup() {
    text_wrapped_disabled("Pick 2 to 16 players. Results from Smash Classic and Super Smash Bros. on Nintendo 64 fill in the bracket by themselves. For other games, you pick the winner.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    // players card
    begin_panel("bk_players");
    section_title("Players (" + std::to_string(BK.entrants.size()) + " of 16)");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    int rm = -1;
    const float rh = sp(5.f);
    for (size_t i = 0; i < BK.entrants.size(); ++i) {
        ImGui::PushID((int)i);
        auto& e = BK.entrants[i];
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetCursorScreenPos();
        const float rw = ImGui::GetContentRegionAvail().x;
        dl->AddRectFilled(o, o + ImVec2(rw, rh), ui::PANEL2, sp(1.5f));
        std::string t = std::to_string(i + 1) + ".  " + e.name + (e.uid.empty() ? "" : "   (rating " + std::to_string(e.rating) + ")");
        dl->AddText(ui::F.bold, ui::F.bold->FontSize, o + ImVec2(sp(2.f), (rh - ui::F.bold->FontSize) * 0.5f), ui::TEXT, t.c_str());
        float x = o.x + rw - sp(0.5f) - sp(12.f);
        ImGui::SetCursorScreenPos(ImVec2(x, o.y + sp(0.5f)));
        if (button("Remove", ImVec2(sp(12.f), rh - sp(1.f)))) rm = (int)i;
        if (BK.seeding == 2 && i > 0) {
            x -= sp(0.75f) + sp(8.f);
            ImGui::SetCursorScreenPos(ImVec2(x, o.y + sp(0.5f)));
            if (button("Up", ImVec2(sp(8.f), rh - sp(1.f)))) std::swap(BK.entrants[i], BK.entrants[i - 1]);
        }
        ImGui::SetCursorScreenPos(o + ImVec2(0, rh + sp(0.75f)));
        ImGui::Dummy(ImVec2(rw, 0));
        ImGui::PopID();
    }
    if (rm >= 0) BK.entrants.erase(BK.entrants.begin() + rm);
    if (BK.entrants.empty()) { ImGui::TextDisabled("Nobody yet. Type a name or pick a friend below."); ImGui::Dummy(ImVec2(0, sp(0.5f))); }
    ImGui::SetNextItemWidth(sp(28.f));
    bool enter = osk::input("##bkname", "Type a name", BK.name_buf, sizeof BK.name_buf, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, sp(1.f));
    if (button("Add", ImVec2(sp(12.f), sp(4.5f))) || enter) { add_entrant(BK.name_buf, ""); BK.name_buf[0] = 0; }
    if (in_room()) {
        ImGui::SameLine(0, sp(1.f));
        if (button("Add everyone in the room", ImVec2(0, sp(4.5f))))
            for (auto& m : room_members()) {
                std::string uid;
                if (m.slot == room_my_slot()) { add_entrant(m.name, ""); continue; }
                for (auto& f : fr_friends()) if (f.status.lobby_code == A.code && low(f.name) == low(m.name)) uid = f.uid;   // uid only for a friend present in this room
                add_entrant(m.name, uid);
            }
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::PushFont(ui::F.bold); ImGui::TextUnformatted("Friends"); ImGui::PopFont();
    int shown = 0;
    for (auto& f : fr_friends()) {
        if (has_name(f.name) || shown >= 12) continue;
        ImGui::PushID(f.uid.c_str());
        if (shown % 4) ImGui::SameLine(0, sp(1.f));
        if (button(f.name.c_str(), ImVec2(sp(16.f), sp(4.5f)))) add_entrant(f.name, f.uid);
        ImGui::PopID();
        ++shown;
    }
    if (shown == 0) ImGui::TextDisabled("No more friends to add.");
    end_panel();
    // ladder card
    auto& l = ladder_entries();
    if (l.size() > 1) {
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        begin_panel("bk_ladder");
        section_title("Friends ladder");
        ImGui::TextDisabled("Ratings from your Smash matches. They decide who plays whom when you seed by rating.");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        for (size_t i = 0; i < l.size() && i < 5; ++i)
            ImGui::Text("#%d   %s   %d   (%d games)", (int)i + 1, l[i].me ? "You" : l[i].name.c_str(), l[i].rating, l[i].games);
        end_panel();
    }
    // format card
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    begin_panel("bk_format");
    section_title("How to run it");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    static const char* fm[] = {"Single elimination", "Double elimination"};
    BK.fmt = ui::segmented("##bkfmt", fm, 2, BK.fmt);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    static const char* sd[] = {"Seed by rating", "Random", "As listed"};
    BK.seeding = ui::segmented("##bksd", sd, 3, BK.seeding);
    if (BK.fmt == 1) { ImGui::Dummy(ImVec2(0, sp(0.5f))); ImGui::Checkbox("Grand final reset (the player from the losers bracket must win twice)", &BK.reset); }
    if (!BK.note.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::BAD), "%s", BK.note.c_str());
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (button("Start bracket", ImVec2(sp(20.f), sp(5.f)), true, BK.entrants.size() >= 2)) start_bracket();
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
}

void draw_champion() {
    int ch = BK.b.champion();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = sp(16.f);
    dl->AddRectFilled(p, p + ImVec2(w, h), ui::mix(ui::PANEL2, ui::ACCENT, 0.18f), sp(1.5f));
    ui::icon(dl, ui::Icon::Star, p + ImVec2(w * 0.5f, sp(3.f)), sp(4.f), ui::GOOD);
    const char* lab = "CHAMPION";
    ImFont* f = ui::F.h2;
    float tw = f->CalcTextSizeA(f->FontSize * 1.4f, 1e9f, 0, lab).x;
    dl->AddText(f, f->FontSize * 1.4f, p + ImVec2((w - tw) * 0.5f, sp(5.5f)), ui::TEXT, lab);
    std::string nm = BK.b.seat_name(ch);
    float nw = f->CalcTextSizeA(f->FontSize * 2.f, 1e9f, 0, nm.c_str()).x;
    dl->AddText(f, f->FontSize * 2.f, p + ImVec2(std::max(sp(1.f), (w - nw) * 0.5f), sp(9.5f)), ui::GOOD, nm.c_str());
    ImGui::Dummy(ImVec2(w, h + sp(1.f)));
}

void draw_page() {
    const Bracket& b = BK.b;
    bool edit = may_edit();
    if (BK.have && b.finished()) draw_champion();
    else if (BK.have) {
        int c = b.current(), nx = b.next();
        if (c >= 0) {
            const MState& m = b.state(c);
            ImGui::PushFont(ui::F.h2);
            ImGui::Text("%s: %s vs %s", b.match_label(c).c_str(), b.seat_name(m.a.seat).c_str(), b.seat_name(m.b.seat).c_str());
            ImGui::PopFont();
            if (nx >= 0) ImGui::TextDisabled("Next: %s vs %s", b.seat_name(b.state(nx).a.seat).c_str(), b.seat_name(b.state(nx).b.seat).c_str());
            if (in_room()) ImGui::TextDisabled(edit ? "Start the match as usual. Others in the room can Watch it." : "The host starts the match. You can Watch it from the room.");
            else ImGui::TextDisabled("Same PC: player 1 = %s, player 2 = %s. Smash Classic results fill this in.", b.seat_name(m.a.seat).c_str(), b.seat_name(m.b.seat).c_str());
            if (edit) {
                ImGui::Dummy(ImVec2(0, sp(0.5f)));
                std::string la = b.seat_name(m.a.seat) + " won", lb = b.seat_name(m.b.seat) + " won";
                if (button(la.c_str(), ImVec2(sp(20.f), sp(5.f)), true)) { if (BK.b.report(c, m.a.seat)) { broadcast(); } }
                ImGui::SameLine();
                if (button(lb.c_str(), ImVec2(sp(20.f), sp(5.f)), true)) { if (BK.b.report(c, m.b.seat)) { broadcast(); } }
            }
        }
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
    }
    if (BK.have && edit) {
        if (button("Undo last result", ImVec2(sp(18.f), sp(4.5f)), false, b.can_undo())) { BK.b.undo(); broadcast(); }
        ImGui::SameLine();
        if (button(b.finished() ? "New bracket" : "Cancel bracket", ImVec2(sp(18.f), sp(4.5f)))) close_bracket();
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
    }
    if (BK.have) {
        ImGui::BeginChild("bk_tree", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
        draw_tree();
        ImGui::EndChild();
    } else if (edit) {
        ImGui::BeginChild("bk_setup", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
        draw_setup();
        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("Waiting for the host to start a bracket...");
    }
}

}  // namespace

// ---------------------------------------------------------------- hooks
void bracket_open() { BK.want_open = true; }

void bracket_on_msg(int from_slot, const hubui::Json& j) {
    const std::string kind = j.str("kind");
    if (kind == "bk_state") {
        if (is_host()) return;   // the host's own state wins; nobody else may overwrite it
        if (BK.b.accept_remote(from_slot, json::stringify(j))) BK.have = true;
    } else if (kind == "bk_off") {
        if (from_slot == 0 && !is_host()) { BK.have = false; BK.b = Bracket(); }
    } else if (kind == "bk_req") {
        if (is_host()) broadcast();
    }
}

void bracket_on_result(const hubui::results::RoundResult& r) {
    if (!BK.have || BK.b.finished() || !may_edit()) return;
    apply_result(r);
}

void bracket_tick() {
    bool room = in_room();
    bool host = room && is_host();
    size_t n = room ? room_members().size() : 0;
    if (room && host) {
        if (BK.have && (!BK.prev_host || n != BK.prev_members)) {
            if (!BK.prev_host && BK.prev_room) BK.b.bump_for_host_change();   // we just became host: our copy must beat the old host's rev
            broadcast();
        }
    } else if (room && !host && BK.prev_room && !BK.asked) {
        A.lobby.send_msg("{\"kind\":\"bk_req\"}");
        BK.asked = true;
    }
    if (!room) { BK.asked = false; if (BK.prev_room && !BK.fake) { BK.have = false; BK.b = Bracket(); } }   // left the room: a guest copy goes away
    BK.prev_host = host; BK.prev_members = n; BK.prev_room = room;
}

void bracket_draw() {
    if (BK.want_open) { BK.want_open = false; BK.open = true; ImGui::OpenPopup("Tournament bracket"); }
    if (!BK.open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(150.f), io.DisplaySize.x * 0.94f), io.DisplaySize.y * 0.92f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(2.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool vis = ImGui::BeginPopupModal("Tournament bracket", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!vis) { if (!ImGui::IsPopupOpen("Tournament bracket")) BK.open = false; return; }
    ImGui::PushFont(ui::F.title);
    ImGui::TextUnformatted("Tournament");
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - sp(14.f) + ImGui::GetCursorPosX());
    bool back = button("Close", ImVec2(sp(14.f), sp(4.5f)), true, true, ui::Icon::Close);
    if (ui::modal_back()) back = true;
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    draw_page();
    if (back) { BK.open = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void bracket_shot_init() {
    const char* m = std::getenv("HUB_SHOT_BRACKET");
    if (!m) return;
    std::string s = m;
    BK.fake = true;
    BK.want_open = true;
    rivals_fake_seed();
    auto ents = [](std::initializer_list<const char*> names) {
        std::vector<Entrant> v; int r = 1100;
        for (auto n : names) { Entrant e; e.name = n; e.rating = r; r -= 25; v.push_back(e); }
        return v;
    };
    if (s == "ladder") {
        BK.entrants = ents({"Mia", "Kai", "camer", "Mickey", "Sam"});
        BK.entrants[0].uid = "miauid"; BK.entrants[1].uid = "kaiuid";
        return;
    }
    if (s == "champion") {
        BK.b.init(Format::Single, true, ents({"Mia", "Kai", "camer", "Mickey"}));
        while (BK.b.current() >= 0) { const MState& st = BK.b.state(BK.b.current()); BK.b.report(BK.b.current(), std::min(st.a.seat, st.b.seat)); }
        BK.have = true;
        return;
    }
    BK.b.init(Format::Double, true, ents({"Mia", "Kai", "camer", "Mickey", "Sam", "Zed", "Ana", "Bo"}));
    for (int i = 0; i < 6 && BK.b.current() >= 0; ++i) { const MState& st = BK.b.state(BK.b.current()); BK.b.report(BK.b.current(), i == 2 ? std::max(st.a.seat, st.b.seat) : std::min(st.a.seat, st.b.seat)); }
    BK.have = true;
}
