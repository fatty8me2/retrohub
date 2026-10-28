#include "app.h"
#include "friends_ui.h"
#include "profile.h"

/* ---------- Online ---------- */

/* ---------- Smash match picker UI ---------- */

bool match_tile(const char* id, const std::string& label, ImVec2 sz, bool sel, ImU32 tint) {
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("t", sz, ImGuiButtonFlags_EnableNav);
    bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    float rd = 8 * S, pad = 8 * S;
    dl->AddRectFilled(p0, p1, sel ? with_alpha(tint, 0.35f) : (hov ? COL_PANEL2 : COL_PANEL), rd);
    dl->AddRect(p0, p1, sel ? tint : (hov ? ui::veil(60/255.f) : ui::HAIRLINE), rd, 0, sel ? 2 * S : 1.f);
    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    dl->PushClipRect(p0 + ImVec2(pad * 0.5f, 0), p1 - ImVec2(pad * 0.5f, 0), true);
    dl->AddText(ImVec2(p0.x + std::max(pad, (sz.x - ts.x) * 0.5f), p0.y + (sz.y - ts.y) * 0.5f), sel ? IM_COL32_WHITE : ui::TEXT, label.c_str());
    dl->PopClipRect();
    return clicked;
}

/* Cards in remix mode: "Fighter  <pill>  costume N" in place of the owned-games row. */
void member_pick_row(int slot) {
    hubui::MemberPick none;
    const hubui::MemberPick* mp = &none;
    if (slot == room_my_slot()) mp = &A.my_pick;
    else { auto it = A.picks.find(slot); if (it != A.picks.end()) mp = &it->second; }
    int n = locals_of(slot);
    std::string lab = hubui::pick_label(*mp, n);
    bool rnd = mp->p[0].fighter < 0 && (n < 2 || mp->p[1].fighter < 0);
    ImU32 col = rnd ? IM_COL32(70, 72, 86, 255) : (mp->p[0].fighter > 0x0B ? mix(COL_REMIX, IM_COL32_BLACK, 0.25f) : mix(COL_ACCENT, IM_COL32_BLACK, 0.15f));
    ImGui::TextDisabled("Fighter");
    ImGui::SameLine(0, 8 * S);
    pill(lab.c_str(), col);
    if (n < 2 && mp->p[0].fighter >= 0) {
        ImGui::SameLine(0, 8 * S);
        ImGui::TextDisabled("costume %d", mp->p[0].costume + 1);
    }
}

void match_panel(bool host, float width) {
    (void)width;
    const bool remix = match_remix_roster();
    auto fighters = hubui::fighter_choices(remix);
    begin_panel("matchpanel");
    section_title(remix ? "Smash Remix match" : "Match");
    ImGui::Dummy(ImVec2(0, 2 * S));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = 8 * S;

    // --- my fighter(s)
    int locals = std::max(1, std::min(2, A.my_locals));
    if (A.pick_edit >= locals) A.pick_edit = 0;
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Your fighter");
    ImGui::PopFont();
    if (locals == 2) {   // the 2nd local player picks their own
        for (int k = 0; k < 2; ++k) {
            ImGui::SameLine(0, k ? gap : 18 * S);
            std::string lab = "Player " + std::to_string(k + 1) + ": " + hubui::fighter_name(A.my_pick.p[k].fighter);
            if (match_tile(("pe" + std::to_string(k)).c_str(), lab, ImVec2(170 * S, 28 * S), A.pick_edit == k, COL_ACCENT)) A.pick_edit = k;
        }
    }
    hubui::MatchPick& cur = A.my_pick.p[A.pick_edit];
    {   // costume row
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Costume");
        int nc = cur.fighter >= 0 ? hubui::fighter_costumes(cur.fighter) : 0;
        if (nc == 0) {
            ImGui::SameLine(0, gap);
            ImGui::TextDisabled("random");
        }
        for (int c = 0; c < nc; ++c) {
            ImGui::SameLine(0, c ? 4 * S : gap);
            if (match_tile(("co" + std::to_string(c)).c_str(), std::to_string(c + 1), ImVec2(34 * S, 28 * S), cur.costume == c, COL_ACCENT)) {
                cur.costume = c;
                send_my_pick();
            }
        }
    }
    ImGui::Dummy(ImVec2(0, 2 * S));
    {   // fighter grid: Random, then Vanilla / Remix groups
        const float min_w = 112 * S;
        int cols = std::max(1, (int)((avail + gap) / (min_w + gap)));
        float tw = (avail - gap * (cols - 1)) / cols;
        ImVec2 tsz(tw, 30 * S);
        auto choose = [&](int id) {
            cur.fighter = id;
            cur.costume = 0;
            send_my_pick();
        };
        if (match_tile("fr", "Random", ImVec2(tw, 30 * S), cur.fighter < 0, ui::TEXT2)) choose(-1);
        for (int grp = 0; grp < 2; ++grp) {
            bool any = false;
            for (auto& f : fighters) any = any || f.remix == (grp == 1);
            if (!any) continue;
            ImGui::Dummy(ImVec2(0, 2 * S));
            ImGui::TextDisabled("%s", grp ? "REMIX" : "VANILLA");
            int i = 0;
            for (auto& f : fighters) {
                if (f.remix != (grp == 1)) continue;
                if (i % cols) ImGui::SameLine(0, gap);
                if (match_tile(("f" + std::to_string(f.id)).c_str(), f.name, tsz, cur.fighter == f.id, grp ? COL_REMIX : COL_ACCENT)) choose(f.id);
                ++i;
            }
        }
    }

    // --- stage + stocks (host decides; everyone sees)
    ImGui::Dummy(ImVec2(0, 6 * S));
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Stage");
    ImGui::PopFont();
    ImGui::SameLine(0, 14 * S);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_ACCENT), "%s", hubui::stage_name(A.rules.stage).c_str());
    if (host) {
        auto stages = hubui::stage_choices(remix);
        const float min_w = 150 * S;
        int cols = std::max(1, (int)((avail - 14 * S + gap) / (min_w + gap)));
        float tw = (avail - 14 * S - gap * (cols - 1)) / cols;
        ImGui::BeginChild("stagelist", ImVec2(0, 210 * S), ImGuiChildFlags_None);
        auto choose = [&](int id) {
            A.rules.stage = id;
            send_rules();
        };
        if (match_tile("sr", "Random", ImVec2(tw, 30 * S), A.rules.stage < 0, ui::TEXT2)) choose(-1);
        for (int grp = 0; grp < 2; ++grp) {
            bool any = false;
            for (auto& s : stages) any = any || s.remix == (grp == 1);
            if (!any) continue;
            ImGui::Dummy(ImVec2(0, 2 * S));
            ImGui::TextDisabled("%s", grp ? "REMIX" : "VANILLA");
            int i = 0;
            for (auto& s : stages) {
                if (s.remix != (grp == 1)) continue;
                if (i % cols) ImGui::SameLine(0, gap);
                if (match_tile(("s" + std::to_string(s.id)).c_str(), s.name, ImVec2(tw, 30 * S), A.rules.stage == s.id, grp ? COL_REMIX : COL_ACCENT)) choose(s.id);
                ++i;
            }
        }
        ImGui::EndChild();
    }
    ImGui::Dummy(ImVec2(0, 4 * S));
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Stocks");
    ImGui::PopFont();
    if (host) {
        for (int k = 1; k <= 5; ++k) {
            ImGui::SameLine(0, k == 1 ? 14 * S : 4 * S);
            if (match_tile(("st" + std::to_string(k)).c_str(), std::to_string(k), ImVec2(40 * S, 28 * S), A.rules.stocks == k, COL_ACCENT)) {
                A.rules.stocks = k;
                send_rules();
            }
        }
    } else {
        ImGui::SameLine(0, 14 * S);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_ACCENT), "%d", A.rules.stocks);
    }
    ImGui::Dummy(ImVec2(0, 2 * S));
    text_wrapped_disabled(host ? "Anyone still on Random, and a Random stage, is rolled for everyone when you press Launch."
                               : "The host picks the stage and stocks. Anyone on Random is rolled at launch.");
    end_panel();
    ImGui::Dummy(ImVec2(0, 4 * S));
}

void member_card(const hubui::PeerInfo* p, int slot, ImVec2 size) {
    ImGui::BeginChild(("m" + std::to_string(slot)).c_str(), size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetWindowPos(), p1 = p0 + size;
    static const ImU32 slotc[4] = {IM_COL32(250, 150, 40, 255), IM_COL32(70, 170, 245, 255), IM_COL32(205, 95, 225, 255), IM_COL32(125, 205, 80, 255)};
    if (!p) {
        dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 40), 14 * S, 0, 1.5f * S);
        const char* t = "Open slot";
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(p0 + (size - ts) * 0.5f, IM_COL32(255, 255, 255, 80), t);
        ImGui::EndChild();
        return;
    }
    dl->AddRectFilled(p0, p1, COL_PANEL, 14 * S);
    ImVec2 cc = p0 + ImVec2(38 * S, 40 * S);
    dl->AddCircleFilled(cc, 24 * S, slotc[slot & 3], 32);
    char init[2] = {(char)std::toupper((unsigned char)(p->name.empty() ? '?' : p->name[0])), 0};
    ImVec2 its = ImGui::CalcTextSize(init);
    dl->AddText(ui::F.h2, ui::F.h2->FontSize, cc - ImVec2(its.x * 0.6f, its.y * 0.62f), IM_COL32(20, 20, 25, 255), init);
    {   // profile avatar + status line (S1)
        hub::profile::Profile rp;
        if (slot == room_my_slot()) rp = fr_my_profile();
        else { auto it = A.room_prof.find(slot); if (it != A.room_prof.end()) { rp.avatar = it->second.avatar; rp.status = it->second.status; } }
        if (hub::profile::valid_avatar(rp.avatar)) fr_profile_avatar_disc(dl, cc, 24 * S, rp.avatar, slotc[slot & 3]);
        if (!rp.status.empty()) {
            float nx = p0.x + 74 * S + ui::F.h2->CalcTextSizeA(ui::F.h2->FontSize, FLT_MAX, 0.f, p->name.c_str()).x + 12 * S;
            float room = p1.x - 14 * S - nx;
            if (room > 40 * S) dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(nx, p0.y + 14 * S + (ui::F.h2->FontSize - ui::F.cap->FontSize) * 0.6f), ui::TEXT2, ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, rp.status, room).c_str());
        }
    }
    dl->AddText(ui::F.h2, ui::F.h2->FontSize, ImVec2(p0.x + 74 * S, p0.y + 14 * S), IM_COL32_WHITE, p->name.c_str());
    ImGui::SetCursorScreenPos(p0 + ImVec2(74 * S, 46 * S));
    bool host = slot == 0;
    bool rd = host || A.ready[slot];
    if (slot == room_my_slot()) rd = host || A.my_ready;
    if (host) pill("HOST", COL_ACCENT);
    else pill(rd ? "Ready" : "Not ready", rd ? COL_GOOD : IM_COL32(70, 72, 86, 255));
    // connection quality to this member (room round trip; "relay" for relay-only peers): drawn as the first chip of the
    // games row below, so it never collides with the Ready/HOST pill or the "Local players" column
    std::string ping_lab;
    ImU32 ping_col = 0;
    {
        int rtt = -1; bool relay = false;
        if (room_ping_of(slot, &rtt, &relay)) {
            hubui::PingClass pc = hubui::ping_class(rtt, relay);
            if (pc != hubui::PingClass::Unknown) {
                ping_col = pc == hubui::PingClass::Great ? COL_GOOD : pc == hubui::PingClass::Ok ? ui::WARN : pc == hubui::PingClass::Poor ? COL_BAD : IM_COL32(70, 72, 86, 255);
                ping_lab = std::string(hubui::ping_class_label(pc)) + " \xC2\xB7 " + hubui::ping_text(rtt, relay);
            }
        }
    }
    if (p->hello) {   // RetroHub version, bottom-right of the card; red when it differs from ours
        std::string theirs = p->app.empty() ? "version unknown" : hubui::app_version_label(p->app);
        bool differs = p->app.empty() || hubui::compare_app_versions(p->app, HUB_VERSION_STR) != 0;
        ImVec2 ts = ImGui::CalcTextSize(theirs.c_str());
        /* Right-aligned in the gap between the local-player controls and the console chips (which start at y=88). */
        dl->AddText(ImVec2(p1.x - ts.x - 14 * S, p0.y + 72 * S), differs ? COL_BAD : ui::TEXT2, theirs.c_str());
    }
    {   // local players (1-2 controllers on this PC)
        int n = locals_of(slot);
        bool mine = slot == room_my_slot();
        ImGui::SetCursorScreenPos(ImVec2(p1.x - 150 * S, p0.y + 10 * S));
        ImGui::PushID(slot);
        ImGui::TextDisabled("Local players");
        ImGui::SetCursorScreenPos(ImVec2(p1.x - 150 * S, p0.y + 30 * S));
        if (mine) {
            if (ImGui::RadioButton("1", n == 1)) { A.my_locals = 1; A.hello_dirty = true; }
            ImGui::SameLine();
            if (ImGui::RadioButton("2", n == 2)) { A.my_locals = 2; A.hello_dirty = true; }
        } else {
            pill(n == 2 ? "2 players" : "1 player", IM_COL32(70, 72, 86, 255));
        }
        ImGui::PopID();
    }
    // owned games
    ImGui::SetCursorScreenPos(p0 + ImVec2(16 * S, 88 * S));
    if (match_active()) {
        member_pick_row(slot);
    } else if (!p->hello) {
        ImGui::TextDisabled("checking games...");
    } else {
        bool first = true;
        /* Chips flow left to right and wrap inside the card, so none are clipped at the right edge. */
        ImFont* cf = ui::F.cap ? ui::F.cap : ImGui::GetFont();
        const float chip_x0 = p0.x + 16 * S, chip_max = p1.x - 12 * S;
        float chip_x = chip_x0, chip_y = p0.y + 88 * S;
        const float chip_row_h = cf->FontSize + ui::sp(1) + 4 * S;
        auto chip = [&](const char* text, ImU32 bg) {
            float w = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, text).x + 2 * ui::sp(1.25f);
            if (chip_x > chip_x0 && chip_x + w > chip_max) { chip_x = chip_x0; chip_y += chip_row_h; }
            ImGui::SetCursorScreenPos(ImVec2(chip_x, chip_y));
            pill(text, bg);
            chip_x += w + 6 * S;
        };
        if (!ping_lab.empty()) chip(ping_lab.c_str(), ping_col);
        if (!p->keys.empty()) {   // v2 peers: games per system; keys without a known system prefix count as "Other"
            std::map<int, int> per;
            int other = 0;
            for (auto& k : p->keys) {
                size_t c = k.find(':');
                hub::System s = c == std::string::npos ? hub::System::Unknown : hub::system_from_short(k.substr(0, c));
                if (s == hub::System::Unknown) ++other; else ++per[(int)s];
            }
            for (auto& kv : per) {
                ImU32 a, b;
                system_colors((hub::System)kv.first, &a, &b);
                char lab[48];
                std::snprintf(lab, sizeof lab, "%s %d", sys_badge((hub::System)kv.first), kv.second);
                first = false;
                chip(lab, mix(a, IM_COL32_BLACK, 0.15f));
            }
            if (other) {
                char lab[32];
                std::snprintf(lab, sizeof lab, "OTHER %d", other);
                first = false;
                chip(lab, IM_COL32(70, 72, 86, 255));
            }
        } else {
            for (auto& g : hub::catalog()) {
                if (!p->games.count(g.id)) continue;
                ImU32 a, b;
                game_colors(g.id, &a, &b);
                first = false;
                chip(short_name(g.id), mix(a, IM_COL32_BLACK, 0.15f));
            }
        }
        if (first) ImGui::TextDisabled("no supported games");
    }
    ImGui::EndChild();
}

