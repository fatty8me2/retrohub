#include "app.h"

void page_online_home() {
    page_header("Play Online", "Host a room and share the code, or join a friend's room.");
    float avail = ImGui::GetContentRegionAvail().x;
    float gap = 20 * S;
    bool two = avail > 760 * S;
    float w = two ? (avail - gap) / 2 : avail;

    begin_panel("host", ImVec2(w, 0));
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Host a room");
    ImGui::PopFont();
    text_wrapped_disabled("Create a room, pick a game everyone owns, and share the 6-letter code.");
    ImGui::Dummy(ImVec2(0, 8 * S));
    if (button("Host Room", ImVec2(-1, sp(7.f)), true, A.proc == nullptr, ui::Icon::Globe)) enter_room(true, hubnet::new_room_code());
    end_panel();

    if (two) ImGui::SameLine();

    begin_panel("join", ImVec2(w, 0));
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Join a room");
    ImGui::PopFont();
    text_wrapped_disabled("Type the code your friend gave you.");
    ImGui::Dummy(ImVec2(0, 8 * S));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%s", A.join_buf.c_str());
    ImGui::PushFont(ui::F.h2);
    ImGui::SetNextItemWidth(w * 0.5f);
    bool enter = ImGui::InputTextWithHint("##code", "ABC234", buf, 7, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopFont();
    A.join_buf = hubui::normalize_code(buf);
    bool valid = hubnet::valid_room_code(A.join_buf);
    ImGui::SameLine();
    bool go = button("Join", ImVec2(120 * S, 0), true, valid && A.proc == nullptr);
    if (!A.join_buf.empty() && !valid) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_WARN), "Codes are 6 letters/digits (no I, O, 0, 1).");
    if (go || (enter && valid)) enter_room(false, A.join_buf);
    end_panel();

    ImGui::Dummy(ImVec2(0, sp(1.f)));
    begin_panel("howto");
    section_title("How it works");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    int online_games = 0;
    for (auto& e : A.lib) if (supports_online(e)) ++online_games;
    static const char* steps[] = {"Host a room (or join with a code) - everyone in the room sees each other's games.",
                                  "The host picks a game that everybody owns, then everyone presses Ready.",
                                  "The host presses Launch - the game starts on every PC at the same moment."};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < 3; ++i) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = sp(1.75f);
        dl->AddCircleFilled(ImVec2(p.x + r, p.y + r + sp(0.25f)), r, with_alpha(ui::ACCENT, 0.25f), 24);
        char num[4] = {(char)('1' + i), 0};
        ImVec2 ts = ui::F.bold->CalcTextSizeA(ui::F.bold->FontSize, FLT_MAX, 0.f, num);
        dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(p.x + r - ts.x * 0.5f, p.y + r + sp(0.25f) - ts.y * 0.5f), ui::TEXT, num);
        ImGui::SetCursorScreenPos(ImVec2(p.x + r * 2 + sp(1.5f), p.y));
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted(steps[i]);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
    ImGui::Text("You have %d game%s that can be played online.", online_games, online_games == 1 ? "" : "s");
    ImGui::PopStyleColor();
    end_panel();
}

void page_online_room() {
    // Header: big code + actions
    ImGui::PushFont(ui::F.big);
    std::string spaced;
    for (size_t i = 0; i < A.code.size(); ++i) { if (i) spaced += ' '; spaced += A.code[i]; }
    ImGui::PushStyleColor(ImGuiCol_Text, COL_ACCENT);
    ImGui::TextUnformatted(spaced.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine(0, 30 * S);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, 8 * S));
    if (button("Copy code", ImVec2(150 * S, 44 * S), true)) { SDL_SetClipboardText(A.code.c_str()); set_notice("Room code copied.", 3); }
    ImGui::SameLine();
    if (button("Copy invite link", ImVec2(200 * S, 44 * S), true)) {
        std::wstring link = invite_web_link(W(A.code));
        if (!link.empty() && invite_copy_text(link)) set_toast("Invite link copied", true, 3.0);
        else set_notice("Could not copy the invite link.", 4);
    }
    if (button("Leave room", ImVec2(150 * S, 44 * S))) { leave_room(); ImGui::EndGroup(); return; }
    ImGui::SameLine();
    fr_invite_button_and_picker(200 * S, 44 * S);
    ImGui::EndGroup();

    if (g_fake_room && A.selected_game.empty() && !A.lib.empty()) {
        for (auto& e : A.lib)
            if (supports_online(e)) { A.selected_game = room_id(e); if (e.title.find("Kart") != std::string::npos) break; }
    }
    if (!g_fake_room && A.lobby.failed()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_BAD), "%s", A.lobby.status().c_str());
        return;
    }
    if (!room_connected()) {
        ImGui::TextDisabled("Connecting...  %s", A.lobby.status().c_str());
        return;
    }

    auto peers = peer_list();
    bool host = is_host();
    float total_w = ImGui::GetContentRegionAvail().x;
    float chat_w = std::max(280 * S, total_w * 0.34f);
    float left_w = total_w - chat_w - 20 * S;
    float h = ImGui::GetContentRegionAvail().y;
    if (h < 420 * S) h = 420 * S;

    ImGui::BeginChild("roomleft", ImVec2(left_w, h), ImGuiChildFlags_NavFlattened);

    // member cards (4 slots)
    float gap = 14 * S;
    int cols = left_w > 560 * S ? 2 : 1;
    float card_w = left_w - (match_active() ? ImGui::GetStyle().ScrollbarSize + 2 * S : 0.f);   // the match picker makes this column scroll
    float mw = (card_w - gap * (cols - 1)) / cols;
    std::map<int, const hubui::PeerInfo*> by_slot;
    for (auto& p : peers) by_slot[p.slot] = &p;
    for (int s = 0; s < 4; ++s) {
        if (s % cols) ImGui::SameLine(0, gap);
        member_card(by_slot.count(s) ? by_slot[s] : nullptr, s, ImVec2(mw, 146 * S));   // room for two chip rows when the console chips wrap
    }

    ImGui::Dummy(ImVec2(0, 4 * S));
    if (!host && !allow_version_mismatch()) {
        std::string host_app;
        bool host_hello = false;
        for (auto& p : peers) if (p.slot == 0) { host_app = p.app; host_hello = p.hello; }
        auto act = hubui::guest_version_action(HUB_VERSION_STR, host_app, host_hello);
        if (act == hubui::GuestVersionAction::UpdateNow) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_BAD), "The host is on %s and you have %s - you need to update to play.",
                               hubui::app_version_label(host_app).c_str(), hubui::app_version_label(HUB_VERSION_STR).c_str());
            if (hubupdate::downloading()) {
                float pr = hubupdate::download_progress();
                ImGui::ProgressBar(pr < 0 ? 0.f : pr, ImVec2(260 * S, 0));
                text_wrapped_disabled(hubupdate::status().c_str());
            } else {
                if (button("Update now", ImVec2(200 * S, 40 * S), true, !hubupdate::checking())) {
                    hubupdate::check_async(true);
                    A.room_update_pending = true;
                    A.room_update_none = false;
                }
                if (A.room_update_pending && !hubupdate::checking()) {
                    A.room_update_pending = false;
                    if (hubupdate::update_available()) hubupdate::start_update();
                    else if (!hubupdate::check_failed()) A.room_update_none = true;
                }
                if (hubupdate::check_failed() && !hubupdate::status().empty()) text_wrapped_disabled(hubupdate::status().c_str());
                else if (A.room_update_none) text_wrapped_disabled("No newer release is published yet. Ask the host which version they're on.");
            }
        } else if (act == hubui::GuestVersionAction::Waiting) {
            text_wrapped_disabled("Checking versions...");
        } else if (act == hubui::GuestVersionAction::HostNeedsUpdate) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_BAD), "Host needs to update (host is on %s, you have %s).",
                               host_app.empty() ? "an old version" : hubui::app_version_label(host_app).c_str(),
                               hubui::app_version_label(HUB_VERSION_STR).c_str());
        }
    }
    if (!host) {
        bool before = A.my_ready;
        if (ImGui::Checkbox("I'm ready", &A.my_ready) && before != A.my_ready) A.hello_dirty = true;
    }
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted(host ? "Choose a game" : "Host's game");
    ImGui::PopFont();

    // game picker: games every member owns (plus the current pick); the rest are summarised
    {
        std::vector<const hub::GameEntry*> cands;
        std::set<std::string> seen;
        for (auto& e : A.lib)
            if (supports_online(e) && seen.insert(room_id(e)).second) cands.push_back(&e);
        std::sort(cands.begin(), cands.end(), [](const hub::GameEntry* a, const hub::GameEntry* b) {
            if (is_native(*a) != is_native(*b)) return is_native(*a);
            std::string x = a->title, y = b->title;
            for (auto& c : x) c = (char)std::tolower((unsigned char)c);
            for (auto& c : y) c = (char)std::tolower((unsigned char)c);
            return x != y ? x < y : a->key < b->key;
        });
        int hidden = 0;
        for (auto* ep : cands) {
            const hub::GameEntry& g = *ep;
            std::string rid = room_id(g);
            auto sel = hubui::game_selectable(rid, max_players_for(g), peers, 0);
            bool chosen = A.selected_game == rid;
            if (!sel.ok && !chosen) { ++hidden; continue; }
            if (!host && !chosen) continue;
            ImGui::PushID(rid.c_str());
            ImVec2 sz(left_w, 54 * S);
            bool can_click = host && sel.ok;
            if (!can_click) ImGui::BeginDisabled();
            bool clicked = ImGui::InvisibleButton("row", sz, ImGuiButtonFlags_EnableNav);
            bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            if (!can_click) ImGui::EndDisabled();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
            ImU32 a, b;
            entry_colors(g, &a, &b);
            dl->AddRectFilled(p0, p1, chosen ? with_alpha(a, 0.35f) : (hov ? COL_PANEL2 : COL_PANEL), 12 * S);
            if (chosen) dl->AddRect(p0, p1, b, 12 * S, 0, 2 * S);
            dl->AddRectFilled(p0 + ImVec2(10 * S, 10 * S), p0 + ImVec2(16 * S, 44 * S), sel.ok ? b : IM_COL32(90, 92, 105, 255), 3 * S);
            std::string label = g.title + "   " + sys_badge(g.system);
            dl->AddText(p0 + ImVec2(30 * S, 13 * S), sel.ok ? IM_COL32_WHITE : IM_COL32(130, 132, 145, 255), label.c_str());
            if (!sel.ok) {
                ImVec2 ts = ImGui::CalcTextSize(sel.reason.c_str());
                dl->AddText(ImVec2(p1.x - ts.x - 16 * S, p0.y + 14 * S), mix(COL_BAD, IM_COL32_WHITE, 0.25f), sel.reason.c_str());
            } else if (chosen) {
                dl->AddText(ImVec2(p1.x - ImGui::CalcTextSize("Selected").x - 16 * S, p0.y + 14 * S), IM_COL32(255, 255, 255, 220), "Selected");
            }
            if (clicked && can_click) {
                A.selected_game = rid;
                A.st.last_game = rid;
                mark_dirty();
                A.lobby.send_msg("{\"kind\":\"select\",\"game\":\"" + json::escape(rid) + "\"}");
            }
            ImGui::PopID();
        }
        if (host && hidden > 0) ImGui::TextDisabled("%d more game(s) are hidden because not everyone in the room has them.", hidden);
        if (host && cands.empty()) ImGui::TextDisabled("No online-capable games in your library yet.");
    }

    ImGui::Dummy(ImVec2(0, 6 * S));
    const hub::GameEntry* rg = find_room_entry(A.selected_game);
    if (rg && rg->system == hub::System::N64) {
        if (host) {
            if (ImGui::Checkbox("Use host's save (unlocked characters/boards)", &A.share_host_save)) A.hello_dirty = true;
        } else {
            ImGui::TextDisabled("%s", A.share_host_save ? "Using the host's save (unlocked characters/boards)." : "Starting from an empty save.");
        }
    }
    if (rg && rg->system == hub::System::GC) ImGui::TextWrapped("GameCube online uses the host's save and memory card for everyone. Your own memory card is not changed.");
    if (match_active()) match_panel(host, left_w);
    if (host) {
        bool ok = false;
        std::string why;
        if (!rg) why = "Choose a game to start";
        else {
            auto s = hubui::game_selectable(A.selected_game, max_players_for(*rg), peers, 0);
            ok = s.ok;
            why = s.reason;
            if (ok) {
                std::string host_app;
                for (auto& p : peers) if (p.slot == 0) host_app = p.app;
                auto vg = hubui::version_gate(host_app, peers, allow_version_mismatch());
                if (!vg.ok) { ok = false; why = vg.reason; }
            }
            if (ok)
                for (auto& p : peers)
                    if (p.slot != 0 && !A.ready[p.slot]) { ok = false; why = p.name + " isn't ready yet"; break; }
        }
        if (A.proc) { ok = false; why = "In game..."; }
        if (rg && rg->system == hub::System::GC) {
            if (gc_needs_unpack(*rg)) { ok = false; why = "Unpacking " + rg->title + " (needed for GameCube online)..."; }
            else if (GCO.active) { ok = false; why = "In game..."; }
        }
        if (button("Launch", ImVec2(220 * S, 60 * S), true, ok)) {
            int delay = A.st.delay_auto ? 0 : A.st.input_delay;   // 0 = auto, same for everyone
            bool share = A.share_host_save && rg->system == hub::System::N64;
            unsigned seed = std::random_device{}();
            ++A.launch_counter;
            hubui::PortPlan plan = hubui::build_ports(member_pads());   // built once; guests use it verbatim
            hubui::MatchSpec spec;   // Randoms are rolled here, once, from the launch seed
            bool with_match = rg->system == hub::System::N64 && match_active();
            gcnp::Spec gspec; bool gc_ok = true;
            if (with_match) spec = hubui::resolve_match(plan.ports, all_picks(), A.rules, seed, match_remix_roster());
            std::string np_extra;
            if (rg->system == hub::System::N3DS) { hubnp3ds::Plan np; np.expected = (int)A.lobby.members().size(); np_extra = hubnp3ds::plan_json_fragment(np); }
            if (rg->system == hub::System::GC) {
                if (!gc_host_spec(*rg, (int)peers.size() + 1, &gspec)) gc_ok = false;
                else np_extra = gcnp::launch_fragment(gspec);
            }
            if ((rg->system == hub::System::GB || rg->system == hub::System::GBC) && hub::link_cable_game(rg->catalog_id))
                np_extra += ",\"np_link\":{\"core\":\"sameboy\",\"consoles\":2}";
            if (rg->system == hub::System::NDS) np_extra += ",\"np_ds\":" + hubui::np_ds_json();
            np_extra += hubui::host_launch_fragment(gs_values_for(*rg));
            if (gc_ok) {
                A.lobby.send_msg(hubui::launch_json(A.selected_game, A.launch_counter, delay, seed, A.st.gb_core, share, &plan, with_match ? &spec : nullptr, np_extra));
                launch_online(*rg, A.launch_counter, delay, true, A.st.gb_core, share, &plan, nullptr, with_match ? &spec : nullptr);
            } else --A.launch_counter;        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", ok ? "Everyone is ready." : why.c_str());
    } else {
        ImGui::TextDisabled("%s", rg ? ("Waiting for the host to start " + rg->title + "...").c_str() : "Waiting for the host to pick a game...");
    }
    ImGui::EndChild();

    // chat
    ImGui::SameLine(0, 20 * S);
    begin_panel("chat", ImVec2(chat_w, h));
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Chat");
    ImGui::PopFont();
    float input_h = ImGui::GetFrameHeight() + 8 * S;
    ImGui::BeginChild("chatlog", ImVec2(0, -input_h), ImGuiChildFlags_None);
    for (auto& l : A.chat) {
        if (l.who.empty()) { text_wrapped_disabled(l.text); continue; }   // system line (e.g. host change)
        ImGui::PushStyleColor(ImGuiCol_Text, COL_ACCENT);
        ImGui::TextUnformatted(l.who.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", l.text.c_str());
    }
    if (A.chat_scroll) { ImGui::SetScrollHereY(1.f); A.chat_scroll = false; }
    ImGui::EndChild();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##chat", "Say something...", A.chat_buf, sizeof A.chat_buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string t = A.chat_buf;
        A.chat_buf[0] = 0;
        if (!t.empty()) {
            A.lobby.send_msg("{\"kind\":\"chat\",\"text\":\"" + json::escape(t) + "\"}");
            A.chat.push_back({A.st.name, t});
            A.chat_scroll = true;
        }
        ImGui::SetKeyboardFocusHere(-1);
    }
    end_panel();
}

void page_online() {
    if (A.in_room) page_online_room();
    else page_online_home();
}

