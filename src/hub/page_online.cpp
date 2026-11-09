#include "app.h"
#include "gb_cart.h"
#include "winutil.h"
#include "friends_ui.h"
#include "party_ui.h"
#include "profile.h"
#include "online_caps.h"
#include "remote_play.h"

/* "Use host's save" is one room flag (A.share_host_save, synced to guests), but its starting value depends on the system (hubcaps::share_host_save_default):
 * the host's explicit choice is remembered per system, and the flag follows the selected game's system. A host who never touches it sees the old behaviour. */
static std::map<int, int> g_share_choice;   // System -> 0/1, only when the host ticked/unticked it
static bool share_flag_for(hub::System s) {
    auto it = g_share_choice.find((int)s);
    const bool eff = hubcaps::share_host_save_effective(s, it == g_share_choice.end() ? -1 : it->second);
    if (A.share_host_save != eff) { A.share_host_save = eff; A.hello_dirty = true; }
    return eff;
}

/* Play Online: friends who are in a joinable room right now (same presence data and join path as the friends panel; no extra traffic). */
static void online_friend_rooms_panel(float w) {
    std::vector<const hubnet::Friend*> rooms;
    for (const hubnet::Friend& f : fr_friends())
        if (hubui::friend_can_join(fr_row(f)) && hubnet::valid_room_code(f.status.lobby_code)) rooms.push_back(&f);
    begin_panel("frooms", ImVec2(w, 0));
    section_title("Friends' rooms");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (rooms.empty()) text_wrapped_disabled("None of your friends are in an open room right now. When one hosts, it shows up here.");
    int shown = 0;
    for (const hubnet::Friend* f : rooms) {
        if (++shown > 6) break;
        ImGui::PushID(f->uid.c_str());
        std::string game = f->status.game_title.empty() ? std::string("a game") : f->status.game_title;
        std::string line = f->name + "  -  " + game;
        if (f->status.max_players > 0) line += "  (" + std::to_string(f->status.players) + "/" + std::to_string(f->status.max_players) + ")";
        float bw = sp(10.f), bh = sp(3.5f);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float tw = ImGui::GetContentRegionAvail().x - bw - sp(1.f);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + (bh - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::TextUnformatted(ui::ellipsize(ImGui::GetFont(), ImGui::GetFontSize(), line, tw).c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x + tw + sp(1.f), p.y));
        if (button("Join", ImVec2(bw, bh), true, A.proc == nullptr)) fr_join_lobby(f->status.lobby_code, f->status.game_id, f->status.game_title, f->name);
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::PopID();
    }
    end_panel();
}

void page_online_home() {
    page_header("Play Online", "Host a room and share the code, or join a friend's room.");
    float avail = ImGui::GetContentRegionAvail().x;
    float gap = 20 * S;
    bool two = avail > 760 * S;
    float w = two ? (avail - gap) / 2 : avail;
    const bool access_ok = fr_access() == hubnet::Access::Ok;
    if (!access_ok) {   // the owner has not approved this account yet / it is blocked: say so instead of reconnecting forever
        begin_panel("access_note", ImVec2(avail, 0), ui::mix(ui::PANEL, fr_access() == hubnet::Access::Pending ? ui::WARN : ui::BAD, 0.16f));
        section_title(hubnet::access_title(fr_access()));
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::TextWrapped("%s", hubnet::access_message(fr_access()).c_str());
        end_panel();
        ImGui::Dummy(ImVec2(0, sp(1.f)));
    }

    // the two cards sit side by side: both are at least as tall as the taller one was last frame (measured below), so they match
    static float host_nat = 0.f, join_nat = 0.f;
    auto natural_h = [] { return ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + ImGui::GetStyle().WindowPadding.y + 2.f; };
    const float same_h = two ? std::max(host_nat, join_nat) : 0.f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, same_h), ImVec2(FLT_MAX, FLT_MAX));
    begin_panel("host", ImVec2(w, 0));
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Host a room");
    ImGui::PopFont();
    text_wrapped_disabled("Create a room, pick a game everyone owns, and share the 6-letter code.");
    ImGui::Dummy(ImVec2(0, 8 * S));
    if (button("Host Room", ImVec2(-1, sp(7.f)), true, A.proc == nullptr && access_ok, ui::Icon::Globe)) enter_room(true, hubnet::new_room_code());
    if (!group_last_label().empty()) {
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        if (button(group_last_label().c_str(), ImVec2(-1, sp(5.f)), false, A.proc == nullptr, ui::Icon::Globe)) group_invite_last();
    }
    host_nat = natural_h();
    end_panel();

    if (two) ImGui::SameLine();

    ImGui::SetNextWindowSizeConstraints(ImVec2(0, same_h), ImVec2(FLT_MAX, FLT_MAX));
    begin_panel("join", ImVec2(w, 0));
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Join a room");
    ImGui::PopFont();
    text_wrapped_disabled("Type the 6-character code your friend gave you, or paste it here from a message or an invite link. Joining puts you in their room right away.");
    ImGui::Dummy(ImVec2(0, 8 * S));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%s", A.join_buf.c_str());
    ImGui::PushFont(ui::F.h2);
    ImGui::SetNextItemWidth(w * 0.5f);
    bool enter = osk::input("##code", "ABC234", buf, 7, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_EnterReturnsTrue, osk::Kind::Room);
    ImGui::PopFont();
    A.join_buf = hubui::normalize_code(buf);
    bool valid = hubnet::valid_room_code(A.join_buf);
    ImGui::SameLine();
    bool go = button("Join", ImVec2(120 * S, 0), true, valid && A.proc == nullptr && access_ok);
    if (!A.join_buf.empty() && !valid) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_WARN), "Codes are 6 letters/digits (no I, O, 0, 1).");
    if ((go || (enter && valid)) && access_ok) enter_room(false, A.join_buf);
    join_nat = natural_h();
    end_panel();

    ImGui::Dummy(ImVec2(0, sp(1.f)));
    {   // Plan 4 P3: a party night for the people on this PC's couch - no room, no network. One compact row: a small button and a caption.
        const float bh = sp(4.5f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (button(party_couch_active() ? "Continue the couch party" : "Couch party", ImVec2(0, bh), party_couch_active(), A.proc == nullptr, ui::Icon::Gamepad)) party_couch_open();
        const float bx = ImGui::GetItemRectMax().x;
        ImGui::GetWindowDrawList()->AddText(ImVec2(bx + sp(1.5f), p.y + (bh - ImGui::GetFontSize()) * 0.5f), ui::TEXT3,
                                            ui::ellipsize(ImGui::GetFont(), ImGui::GetFontSize(), "Everyone on this PC takes turns: games, score, a winner. No room needed.", std::max(sp(10.f), p.x + avail - bx - sp(1.5f))).c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + bh));
        ImGui::Dummy(ImVec2(0, 0));
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    online_friend_rooms_panel(avail);
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    begin_panel("howto");
    ImGui::SetNextItemOpen(!A.st.played_online, ImGuiCond_Once);   // veterans get it collapsed; the header still opens it
    bool howto_open = ImGui::CollapsingHeader("How it works");
    if (howto_open) {
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
    }
    end_panel();
}

void host_launch_selected() {
    if (launch_blocked("host_launch_selected")) return;
    const hub::GameEntry* rg = find_room_entry(A.selected_game);
    if (!rg) return;
    if (is_hack(*rg)) {   // ROM hack: every PC plays its own patched copy. Until the host's is ready (still preparing, or failed with a notice) nothing is announced
        std::string patched;
        if (!hack_ensure_rom(*rg, &patched)) return;
    }
    if (remote_play_entry(*rg) && !rplay_precheck()) return;   // Wii U remote play: probe the direct paths first; poll_rplay re-enters here when they all work
    GRP.again = false;
    std::vector<hubui::PeerInfo> peers = peer_list();
            int delay = A.st.delay_auto ? 0 : A.st.input_delay;   // 0 = auto, same for everyone
            bool share = hubcaps::share_host_save_supported(rg->system) && share_flag_for(rg->system) && !is_kanto(*rg);
            unsigned seed = std::random_device{}();
            ++A.launch_counter;
            hubui::PortPlan plan = hubui::build_ports(member_pads());   // built once; guests use it verbatim
            hubui::MatchSpec spec;   // Randoms are rolled here, once, from the launch seed
            bool with_match = rg->system == hub::System::N64 && match_active();   // the room picked fighters / stage / rules in the Smash match picker: n64run gets a --match spec
            gcnp::Spec gspec; bool gc_ok = true;
            if (with_match) spec = hubui::resolve_match(plan.ports, all_picks(), A.rules, seed, match_remix_roster());
            std::string np_extra;
            if (rg->system == hub::System::N3DS) { hubnp3ds::Plan np; np.expected = (int)A.lobby.members().size(); { std::vector<std::string> apps; for (auto& pi : peers) apps.push_back(pi.hello ? pi.app : std::string()); if (hubnp3ds::room_password_supported(apps)) np.password = hubnp3ds::new_room_password(); } hubnp3ds::set_pending_host_password(np.password);   /* mixed versions: no password, as before */ np_extra = hubnp3ds::plan_json_fragment(np); }
            if (rg->system == hub::System::GC) {
                if (!gc_host_spec(*rg, (int)peers.size(), &gspec)) gc_ok = false;
                else np_extra = gcnp::launch_fragment(gspec);
            }
            if ((rg->system == hub::System::GB || rg->system == hub::System::GBC || rg->system == hub::System::GBA) && hub::link_cable_game(rg->catalog_id))
                np_extra += std::string(",\"np_link\":{\"core\":\"") + (rg->system == hub::System::GBA ? "mgba" : "sameboy") + "\",\"consoles\":2}";
            if (rg->system == hub::System::NDS) np_extra += ",\"np_ds\":" + hubui::np_ds_json();
            if (remote_play_entry(*rg)) np_extra += hubrp::launch_fragment((int)peers.size());
            np_extra += hubui::host_launch_fragment(gs_values_for(*rg));
            np_extra += cheats_launch_fragment(*rg);
            if (gc_ok) {
                bool same_app = true;   // N64 drop-in changes what the game sees from boot (all four pads plugged in): every PC must run this exact build
                for (auto& pi : peers) if (pi.slot != 0 && hubui::guest_version_action(HUB_VERSION_STR, pi.app, true) != hubui::GuestVersionAction::None) same_app = false;
                const bool dropin_adv = (rg->system == hub::System::NES || rg->system == hub::System::SNES || (rg->system == hub::System::N64 && same_app && hubui::n64_dropin_game(rg->catalog_id))) && !share && !with_match && cheats_launch_fragment(*rg).empty();   // !with_match: with a --match spec only the founders' ports are human (match.cpp write_vs); every other port is "none", so a drop-in joiner there would have no fighter
                std::string room_gb_core = A.st.gb_core;   /* Gambatte cannot keep a cartridge clock in step between PCs: such a game uses SameBoy online (guests follow the launch message) */
                if ((rg->system == hub::System::GB || rg->system == hub::System::GBC) && !hub::link_cable_game(rg->catalog_id) && room_gb_core == "gambatte") {
                    uint8_t ct = 0;
                    if (gbcart::read_cart_type(winutil::to_wide(rg->path), &ct) && gbcart::gb_cart_has_clock(ct)) {
                        room_gb_core = gbcart::online_gb_core(room_gb_core, true);
                        set_notice(rg->title + " has a clock that Gambatte can't keep in step online, so this game uses SameBoy online.", 10.0);
                    }
                }
                std::string lmsg = hubui::launch_json(A.selected_game, A.launch_counter, delay, seed, room_gb_core, share, &plan, with_match ? &spec : nullptr, np_extra, dropin_adv, dropin_adv && rg->system == hub::System::N64 ? 2 : 3);   /* the joiner would be refused with a shared host save or cheats: do not advertise a free port then */
                { hubui::Json lj; g_room_dropin = json::parse(lmsg, &lj) && lj.num("dropin", 0) >= 1; }   // host launch_online() gets no launch_msg
                shot_trace("host-msg", lmsg);
                A.lobby.send_msg(lmsg);
                launch_online(*rg, A.launch_counter, delay, true, room_gb_core, share, &plan, nullptr, with_match ? &spec : nullptr);
                g_room_dropin = false;   // set per launch, never left over for another
                /* OF-M8: the launch message is out before launch_online() validates anything. If nothing started - and nothing is starting - the
                 * guests' runners would wait for a game that never comes: tell them (they show the reason and close a runner that is already up). */
                if (!g_fake_room && room_connected() && !session_active())
                    A.lobby.send_msg("{\"kind\":\"np_abort\",\"reason\":\"the game did not start on the host's PC\"}");
            } else --A.launch_counter;        }

/* Why the chosen game cannot start right now ("" = fine). Shared by the READY and LAUNCH buttons. */
static std::string room_block_reason(bool host, const hub::GameEntry* rg, const std::vector<hubui::PeerInfo>& peers, hubui::GuestVersionAction* gact) {
    *gact = hubui::GuestVersionAction::None;
    if (!host) {   // version mismatch blocks READY even before a game is picked (same rule as ready_block_reason_now)
        std::string host_app;
        bool host_hello = false;
        for (auto& p : peers) if (p.slot == 0) { host_app = p.app; host_hello = p.hello; }
        if (!allow_version_mismatch()) {
            auto act = hubui::guest_version_action(HUB_VERSION_STR, host_app, host_hello);
            *gact = act;
            if (act != hubui::GuestVersionAction::None) {
                std::string r = ready_block_reason_now();
                if (!r.empty()) return r;
            }
        }
    }
    if (!rg) return "";
    if (A.proc) return "You're in a game right now.";
    auto sel = hubui::game_selectable(A.selected_game, max_players_for(*rg), peers, 0, remote_play_entry(*rg));   // remote play: guests need no copy
    if (!sel.ok) {
        std::string why = player_limit_why(*rg);   // only what "(max N)" does not already say
        return (!why.empty() && sel.reason.compare(0, 11, "too many pl") == 0) ? sel.reason + ". " + why : sel.reason;
    }
    if (host && remote_play_entry(*rg) && is_remote_stand_in(*rg)) return "You need your own copy of " + rg->title + " to host remote play.";
    if (host && remote_play_entry(*rg)) { std::string b = rplay_block_reason(); if (!b.empty()) return b; }
    std::string host_app;
    bool host_hello = false;
    for (auto& p : peers) if (p.slot == 0) { host_app = p.app; host_hello = p.hello; }
    if (!host) { std::string b = emulator_online_block(); if (!b.empty()) return b; }   // Dolphin / Azahar missing on this PC: READY stays off with that reason
    if (rg->system == hub::System::PS1) { std::string b = ps1_online_block(); if (!b.empty()) return b; }   // host and guest alike
    if (rg->system == hub::System::PS2) { std::string b = ps2_online_block(*rg); if (!b.empty()) return b; }   // core download / BIOS / unpack, host and guest alike
    if (host) {
        auto vg = hubui::version_gate(host_app, peers, allow_version_mismatch());
        if (!vg.ok) return vg.reason;
        if (is_kanto(*rg)) { std::string b = kt_launch_block(*rg); if (!b.empty()) return b; }
        { std::string b = cheats_launch_block(*rg, peers); if (!b.empty()) return b; }
        if (rg->system == hub::System::N3DS && gc_needs_unpack(*rg)) return "Unpacking " + rg->title + " (needed for 3DS online)...";
        if (rg->system == hub::System::WiiU && gc_needs_unpack(*rg) && !is_remote_stand_in(*rg)) return "Unpacking " + rg->title + " (needed to stream it)...";
        if (rg->system == hub::System::GC) {
            if (gc_needs_unpack(*rg)) return "Unpacking " + rg->title + " (needed for GameCube online)...";
            if (GCO.active) return "A GameCube game is already running.";
        }
        return "";
    }
    return "";
}

/* Party mode (Plan 4): the room banner's rules, for code that is not the banner. */

/* Why the host's Launch button is disabled right now ("" and *can_launch = true when it is enabled): the same rule as the room banner. */
std::string room_host_launch_block(bool* can_launch) {
    if (can_launch) *can_launch = false;
    if (!is_host()) return "Only the host can launch.";
    const std::vector<hubui::PeerInfo> peers = peer_list();
    const hub::GameEntry* rg = find_room_entry(A.selected_game);
    hubui::GuestVersionAction gact;
    const std::string block = room_block_reason(true, rg, peers, &gact);
    std::vector<hubui::RoomMember> mem;
    const int me = room_my_slot();
    for (auto& p : peers) {
        auto it = A.ready.find(p.slot);
        const bool rd = p.slot == 0 || (p.slot == me ? A.my_ready : (it != A.ready.end() && it->second));
        mem.push_back({p.name, rd, p.slot == 0, p.slot == me});
    }
    const hubui::RoomActionState st = hubui::room_action_state(true, rg != nullptr, rg ? rg->title : "", block, A.my_ready, mem);
    const bool ok = st.button_enabled && st.is_launch;
    if (can_launch) *can_launch = ok;
    return ok ? std::string() : (!st.detail.empty() ? st.detail : st.headline);
}

/* "" or why this PC cannot press READY for the room's game right now (missing game / BIOS, version, ...). */
std::string room_guest_ready_block() {
    if (is_host()) return "";
    const std::vector<hubui::PeerInfo> peers = peer_list();
    const hub::GameEntry* rg = find_room_entry(A.selected_game);
    hubui::GuestVersionAction gact;
    return room_block_reason(false, rg, peers, &gact);
}

/* The unmissable top-of-room action: big READY toggle (guest) / LAUNCH (host), the state in words, and who is ready. */
static void room_banner(bool host, const hub::GameEntry* rg, const std::vector<hubui::PeerInfo>& peers) {
    hubui::GuestVersionAction gact;
    std::string block = room_block_reason(host, rg, peers, &gact);
    std::vector<hubui::RoomMember> mem;
    std::vector<bool> in_game;
    int me = room_my_slot();
    for (auto& p : peers) {
        auto ig = A.ingame.find(p.slot);
        in_game.push_back(ig != A.ingame.end() && ig->second);
        bool rd = p.slot == 0 || (p.slot == me ? A.my_ready : A.ready[p.slot]);
        mem.push_back({p.name, rd, p.slot == 0, p.slot == me});
    }
    hubui::RoomActionState st = hubui::room_action_state(host, rg != nullptr, rg ? rg->title : "", block, A.my_ready, mem);
    bool update_mode = !host && gact == hubui::GuestVersionAction::UpdateNow;
    bool dl_busy = update_mode && hubupdate::downloading();
    if (update_mode) {
        st.button_label = "UPDATE NOW";
        st.button_enabled = !hubupdate::checking() && !dl_busy;
        st.is_launch = false;
        st.ready_on = false;
    }
    if (A.room_update_pending && !hubupdate::checking()) {   // "Update now" was pressed: start the download once the check finished
        A.room_update_pending = false;
        if (hubupdate::update_available()) hubupdate::start_update();
        else if (!hubupdate::check_failed()) A.room_update_none = true;
    }
    group_decorate_room_action(update_mode, st.is_launch, st.ready_on, st.button_enabled, st.button_label, st.headline);
    std::string extra;
    if (update_mode && !dl_busy) {
        if (hubupdate::check_failed() && !hubupdate::status().empty()) extra = hubupdate::status();
        else if (A.room_update_none) extra = "No newer release is published yet. Ask the host which version they're on.";
    }

    if (extra.empty() && rg && remote_play_entry(*rg) && block.empty())   // remote play banner: who needs the game, and how it connects
        {
            std::string host_name;
            for (auto& m : room_members()) if (m.slot == 0) host_name = m.name;
            extra = host ? std::string("Remote play: you run the game and stream it to the others.") : hubcaps::remote_play_banner(host_name);
        }
    if (host && !rplay_status_line().empty()) extra = rplay_status_line();
    ImU32 tone = st.tone == hubui::RoomTone::Good ? ui::GOOD : st.tone == hubui::RoomTone::Bad ? ui::BAD : st.tone == hubui::RoomTone::Warn ? ui::WARN : ui::ACCENT;
    float W = ImGui::GetContentRegionAvail().x;
    float bw = std::min(340 * S, W * 0.34f), bh = 80 * S;
    const float tw = W - 24 * S - bw - 14 * S - 16 * S;   // the text column: left of the button column
    ImFont* hf = ui::F.title ? ui::F.title : ui::F.h2;
    const std::string det = dl_busy ? hubupdate::status() : (extra.empty() ? st.detail : extra);
    const auto lines = ui::wrap_lines(ImGui::GetFont(), ImGui::GetFontSize(), det, tw, 4);   // wraps (up to 4 lines), never truncates a reason
    // tall enough for the headline, every detail line AND the readiness row under them (a long detail used to run into it)
    float H = std::max(84 * S, 10 * S + hf->FontSize + 6 * S + lines.size() * (ImGui::GetFontSize() + 2 * S) + 14 * S);   // headline, reason and the button; who is ready lives on the member rows
    bh = std::min(bh, H - 16 * S);
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1 = p0 + ImVec2(W, H);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, mix(COL_PANEL, tone, 0.16f), 16 * S);
    dl->AddRect(p0, p1, with_alpha(tone, 0.75f), 16 * S, 0, 2 * S);
    dl->AddRectFilled(p0 + ImVec2(0, 14 * S), p0 + ImVec2(6 * S, H - 14 * S), tone, 3 * S);

    ImVec2 bp = ImVec2(p1.x - bw - 14 * S, p0.y + (H - bh) * 0.5f);
    float tx = p0.x + 24 * S;

    // headline + detail
    std::string head = ui::ellipsize(hf, hf->FontSize, st.headline, tw);
    dl->AddText(hf, hf->FontSize, ImVec2(tx, p0.y + 10 * S), ui::TEXT, head.c_str());
    float ly = p0.y + 10 * S + hf->FontSize + 6 * S;
    for (auto& l : lines) { dl->AddText(ImVec2(tx, ly), ui::TEXT2, l.c_str()); ly += ImGui::GetFontSize() + 2 * S; }
    // the button
    shot_trace_once("room-button", std::string(host ? "host " : "guest ") + A.selected_game + " label=[" + st.button_label + "] enabled=" + (st.button_enabled ? "1" : "0") + " launch=" + (st.is_launch ? "1" : "0") + (block.empty() ? "" : " block=[" + block + "]"));
    ImGui::SetCursorScreenPos(bp);
    static std::string focus_code;
    static int focus_tries = 0;
    if (focus_code != A.code) { focus_code = A.code; focus_tries = 40; }
    if (focus_tries > 0 && st.button_enabled) ImGui::SetKeyboardFocusHere();   // a disabled button cannot take it: the request would land on the next field (the picker search box)
    ImGui::BeginDisabled(!st.button_enabled);
    bool pressed = ImGui::InvisibleButton("##roomaction", ImVec2(bw, bh), ImGuiButtonFlags_EnableNav);
    if (ImGui::IsItemFocused()) focus_tries = 0; else if (focus_tries > 0) --focus_tries;
    bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    bool focus = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    ImGui::EndDisabled();
    float* hv = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), 0.f);
    ui::anim(*hv, (hov || focus) && st.button_enabled ? 1.f : 0.f, 16.f);
    ImVec2 b0 = bp, b1 = bp + ImVec2(bw, bh);
    if (held) { b0 += ImVec2(1, 1) * S; b1 -= ImVec2(1, 1) * S; }
    float rd = 14 * S;
    ImU32 bg, fg, edge = 0;
    ui::Icon ic = st.ready_on ? ui::Icon::Close : (st.is_launch ? ui::Icon::Play : ui::Icon::Check);
    if (!st.button_enabled && !st.ready_on) { bg = ui::PANEL3; fg = ui::TEXT3; }
    else if (st.ready_on) { bg = mix(ui::PANEL2, ui::GOOD, 0.18f + 0.15f * *hv); fg = ui::TEXT; edge = ui::GOOD; }
    else if (update_mode) { bg = mix(ui::WARN, IM_COL32_WHITE, 0.2f * *hv); fg = IM_COL32(25, 20, 8, 255); }
    else if (st.is_launch) { bg = mix(ui::ACCENT, IM_COL32_WHITE, 0.2f * *hv); fg = IM_COL32_WHITE; }
    else { bg = mix(ui::GOOD, IM_COL32_WHITE, 0.2f * *hv); fg = IM_COL32(8, 28, 16, 255); }
    if (st.button_enabled && !st.ready_on) ui::glow(dl, b0, b1, rd, bg, 0.55f + 0.45f * *hv);
    dl->AddRectFilled(b0, b1, bg, rd);
    if (edge) dl->AddRect(b0, b1, edge, rd, 0, 3 * S);
    ImFont* bf = ui::F.title ? ui::F.title : ui::F.h2;
    ImVec2 ts = bf->CalcTextSizeA(bf->FontSize, FLT_MAX, 0.f, st.button_label.c_str());
    bool with_icon = st.button_enabled || st.ready_on;
    float isz = 26 * S, cw = ts.x + (with_icon ? isz + 12 * S : 0);
    float x = b0.x + (b1.x - b0.x - cw) * 0.5f, cy = (b0.y + b1.y) * 0.5f;
    if (with_icon) { ui::icon(dl, ic, ImVec2(x + isz * 0.5f, cy), isz, fg); x += isz + 12 * S; }
    dl->AddText(bf, bf->FontSize, ImVec2(x, cy - ts.y * 0.5f), fg, st.button_label.c_str());
    if (focus) ui::focus_ring(dl, b0, b1, rd);

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + 12 * S));
    ImGui::Dummy(ImVec2(0, 0));
    if (!pressed || !st.button_enabled) return;
    if (update_mode) {
        hubupdate::check_async(true);
        A.room_update_pending = true;
        A.room_update_none = false;
    } else if (st.is_launch) { shot_trace("launch"); host_launch_selected(); }
    else { shot_trace("ready", A.my_ready ? "off" : "on"); A.my_ready = !A.my_ready; A.hello_dirty = true; }
}

/* The lobby info, as a slim footer bar: the room code (small copy button), the invite buttons and a red Leave room. Returns true when the room was left. */
static bool room_footer(float fw) {
    const float H = 56 * S, bh = 36 * S;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, COL_PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, ui::HAIRLINE);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * S, (H - bh) * 0.5f - 1.f));
    ImGui::BeginChild("roomfoot", ImVec2(fw, H), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    bool left = false;
    const float y0 = ImGui::GetCursorPosY();
    // measure what each step needs and take the first that fits: full -> "Invite link" -> + icon for Invite friends -> no "ROOM" label (Leave room is never shortened)
    const float lw = 130 * S;
    auto bw = [&](const char* t, bool icon) { return ImGui::CalcTextSize(t).x + (icon ? sp(2.25f) + sp(1.f) : 0.f) + sp(5.f); };
    const float code_w = ui::F.h2->CalcTextSizeA(ui::F.h2->FontSize, FLT_MAX, 0.f, A.code.c_str()).x;
    const float room_w = ImGui::CalcTextSize("ROOM").x + 10 * S;
    const float avail = ImGui::GetContentRegionAvail().x - 4 * S;
    auto need = [&](bool short_link, bool icon_inv, bool room_lbl) {
        return (room_lbl ? room_w : 0.f) + code_w + 10 * S + bw("Copy", false) + 8 * S + bw(short_link ? "Invite link" : "Copy invite link", false) + 8 * S +
               (icon_inv ? bh : bw("Invite friends", true)) + 12 * S + lw;
    };
    bool short_link = false, icon_inv = false, room_lbl = true;
    if (need(false, false, true) > avail) { short_link = true;
        if (need(true, false, true) > avail) { icon_inv = true;
            if (need(true, true, true) > avail) room_lbl = false; } }
    if (room_lbl) {
        ImGui::SetCursorPosY(y0 + (bh - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::TextUnformatted("ROOM");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 10 * S);
    }
    ImGui::SetCursorPosY(y0 + (bh - ui::F.h2->FontSize) * 0.5f);
    ImGui::PushFont(ui::F.h2);
    ImGui::PushStyleColor(ImGuiCol_Text, COL_ACCENT);
    ImGui::TextUnformatted(A.code.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine(0, 10 * S);
    ImGui::SetCursorPosY(y0);   // (after SameLine, which resets Y to the text's line)
    if (button("Copy", ImVec2(0, bh))) { SDL_SetClipboardText(A.code.c_str()); set_notice("Room code copied.", 3); }
    ImGui::SameLine(0, 8 * S);
    ImGui::SetCursorPosY(y0);
    if (button(short_link ? "Invite link" : "Copy invite link", ImVec2(0, bh))) {
        std::wstring link = invite_web_link(W(A.code), W(A.st.name));
        if (!link.empty() && invite_copy_text(link)) set_toast("Invite link copied", true, 3.0);
        else set_notice("Could not copy the invite link.", 4);
    }
    ImGui::SameLine(0, 8 * S);
    ImGui::SetCursorPosY(y0);
    fr_invite_button_and_picker(0, bh, icon_inv);
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - lw);
    ImGui::SetCursorPosY(y0);
    {   // red: the palette's danger colour stands in for ACCENT while the button draws
        const ImU32 save = ui::ACCENT;
        ui::ACCENT = ui::BAD;
        if (button("Leave room", ImVec2(lw, bh), true)) left = true;
        ui::ACCENT = save;
    }
    ImGui::EndChild();
    if (left) leave_room();
    return left;
}

void page_online_room() {
    if (g_fake_room && !std::getenv("HUB_SHOT_NOGAME")) {   // screenshot aid: a scratch library has no games; fake a native one
        bool any = false;
        for (auto& e : A.lib) if (supports_online(e)) any = true;
        if (!any) { hub::GameEntry fe; fe.key = kNativeKey; fe.title = "Super Smash Bros. 64"; fe.system = hub::System::N64; fe.catalog_id = "smash64"; A.lib.push_back(fe); }
    }
    if (g_fake_room && A.selected_game.empty() && !A.lib.empty() && !std::getenv("HUB_SHOT_NOGAME")) {
        for (auto& e : A.lib)
            if (supports_online(e)) { A.selected_game = room_id(e); if (e.title.find("Kart") != std::string::npos) break; }
    }
    if (g_shot_rejoin || (!g_fake_room && A.lobby.failed())) {
        // banner-styled failure strip: reason on the left, Rejoin where the action button normally sits
        float W = ImGui::GetContentRegionAvail().x, H = 108 * S;
        ImVec2 p0 = ImGui::GetCursorScreenPos(), p1 = p0 + ImVec2(W, H);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, p1, mix(COL_PANEL, ui::BAD, 0.16f), 16 * S);
        dl->AddRect(p0, p1, with_alpha(ui::BAD, 0.75f), 16 * S, 0, 2 * S);
        dl->AddRectFilled(p0 + ImVec2(0, 14 * S), p0 + ImVec2(6 * S, H - 14 * S), ui::BAD, 3 * S);
        float bw = std::min(340 * S, W * 0.34f), bh = 80 * S;
        ImVec2 bp(p1.x - bw - 14 * S, p0.y + (H - bh) * 0.5f);
        float tx = p0.x + 24 * S, tw = bp.x - tx - 16 * S;
        ImFont* hf = ui::F.title ? ui::F.title : ui::F.h2;
        dl->AddText(hf, hf->FontSize, ImVec2(tx, p0.y + 10 * S), ui::TEXT, "Disconnected from the room");
        auto lines = ui::wrap_lines(ImGui::GetFont(), ImGui::GetFontSize(), g_shot_rejoin ? g_shot_rejoin_why : A.lobby.status(), tw, 2);
        float ly = p0.y + 10 * S + hf->FontSize + 6 * S;
        for (auto& l : lines) { dl->AddText(ImVec2(tx, ly), ui::TEXT2, l.c_str()); ly += ImGui::GetFontSize() + 2 * S; }
        ImGui::SetCursorScreenPos(bp);
        if (button("Rejoin room", ImVec2(bw, bh), true)) rejoin_room();   // same code and role
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + 12 * S));
        ImGui::Dummy(ImVec2(0, 0));
        room_footer(ImGui::GetContentRegionAvail().x);
        return;
    }
    if (!room_connected()) {
        ImGui::TextDisabled("Connecting...  %s", A.lobby.status().c_str());
        ImGui::Dummy(ImVec2(0, 8 * S));
        room_footer(ImGui::GetContentRegionAvail().x);
        return;
    }

    auto peers = peer_list();
    bool host = is_host();
    const hub::GameEntry* rg = find_room_entry(A.selected_game);
    room_game_hero(rg, host);   // TOP: the selected game, big, with its cover (guests: the host's pick)
    room_banner(host, rg, peers);   // and its call to action: Launch (host) / Ready (guest) with who is ready
    float total_w = ImGui::GetContentRegionAvail().x;
    float chat_w = std::max(280 * S, total_w * 0.34f);
    float left_w = total_w - chat_w - 20 * S;
    const float foot_h = 56 * S + 2 * S + ImGui::GetStyle().ItemSpacing.y * 2 + 4 * S;   // bar + the gaps the layout adds around it
    float h = ImGui::GetContentRegionAvail().y - foot_h;   // the footer bar sits under the columns
    if (h < 110 * S) h = 110 * S;   // short windows: the middle shrinks and scrolls, the footer bar stays on screen

    ImGui::BeginChild("roomleft", ImVec2(left_w, h), ImGuiChildFlags_NavFlattened);

    kt_room_panel(left_w - ImGui::GetStyle().ScrollbarSize, true);   // Kanto Together: live status + trade panel first
    // the players
    std::map<int, const hubui::PeerInfo*> by_slot;
    for (auto& p : peers) by_slot[p.slot] = &p;
    if (!match_active()) member_roster(by_slot, left_w - ImGui::GetStyle().ScrollbarSize);   // compact rows, all four fit above the footer
    else {   // the match picker needs the tall cards
        // member cards (4 slots)
        float gap = 14 * S;
        int cols = left_w > 560 * S ? 2 : 1;
        float card_w = left_w - (match_active() ? ImGui::GetStyle().ScrollbarSize + 2 * S : 0.f);   // the match picker makes this column scroll
        float mw = (card_w - gap * (cols - 1)) / cols;
        for (int s = 0; s < 4; ++s) {
            if (s % cols) ImGui::SameLine(0, gap);
            member_card(by_slot.count(s) ? by_slot[s] : nullptr, s, ImVec2(mw, 146 * S));   // room for two chip rows when the console chips wrap
        }


    }
    ImGui::Dummy(ImVec2(0, 4 * S));
    party_room_panel(left_w - ImGui::GetStyle().ScrollbarSize);   // Plan 4 Party mode / vote for the next game (nothing drawn for a guest while it is off)
    cheats_room_strip(rg, host);
    room_picker(host, left_w - ImGui::GetStyle().ScrollbarSize - 2 * S, peers);

    ImGui::Dummy(ImVec2(0, 6 * S));
    if (rg && !is_kanto(*rg) && hubcaps::share_host_save_supported(rg->system)) {
        if (host) {
            share_flag_for(rg->system);
            if (ImGui::Checkbox(hubcaps::share_host_save_label(rg->system), &A.share_host_save)) { g_share_choice[(int)rg->system] = A.share_host_save ? 1 : 0; A.hello_dirty = true; }
        } else {
            ImGui::TextDisabled("%s", hubcaps::share_host_save_state_text(rg->system, A.share_host_save));
        }
    }
    if (rg && !is_kanto(*rg) && hubctl::turbo_supported(hubctl::system_key(rg->system))) {   // turbo buttons are local-only input; off online unless the host allows it for everyone
        if (host) {
            if (ImGui::Checkbox("Allow turbo buttons for everyone", &A.room_turbo)) A.hello_dirty = true;
        } else if (A.room_turbo) ImGui::TextDisabled("The host allows turbo buttons in this room.");
    }
    if (rg && rg->system == hub::System::GC) {
        ImGui::TextWrapped("GameCube online uses the host's memory card for everyone. Guests' own cards are never touched.");
        if (host) ImGui::Checkbox("Keep this session's progress on my memory card", &A.gc_write_save);
    }
    if (match_active()) match_panel(host, left_w);
    kt_room_panel(left_w - ImGui::GetStyle().ScrollbarSize, false);
    if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 1.f) {   // more below: a soft fade at the bottom edge is the scroll cue
        ImDrawList* fdl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        const float fh = 56 * S, fw = ws.x - ImGui::GetStyle().ScrollbarSize;
        fdl->AddRectFilledMultiColor(ImVec2(wp.x, wp.y + ws.y - fh), ImVec2(wp.x + fw, wp.y + ws.y), with_alpha(ui::BG, 0.f), with_alpha(ui::BG, 0.f), ui::BG, ui::BG);
    }
    ImGui::EndChild();

    // chat
    ImGui::SameLine(0, 20 * S);
    static float chat_used = 0.f;   // the log's content height last frame: an empty chat stays a small box and grows with the messages
    const float chat_h = std::min(h, std::max(190 * S, std::min(h, chat_used + 118 * S)));
    begin_panel("chat", ImVec2(chat_w, chat_h));
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
        if (l.seq > 0) {   // reactions (S4): click the line to react, chips show the tally
            ImVec2 a0 = ImGui::GetItemRectMin(), a1 = ImGui::GetItemRectMax();
            ImGui::PushID((int)(&l - &A.chat[0]) + 7000);
            ImVec2 save = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(a0);
            if (ImGui::InvisibleButton("rx", ImVec2(std::max(1.f, ImGui::GetContentRegionAvail().x), std::max(1.f, a1.y - a0.y)))) ImGui::OpenPopup("rxp");
            ImGui::SetCursorScreenPos(save);
            if (ImGui::BeginPopup("rxp")) {
                for (size_t ri = 0; ri < 6; ri++) {
                    if (ri) ImGui::SameLine();
                    if (ImGui::Button(hub::profile::kReactionLabels[ri])) {
                        auto it = l.rx.find(A.st.name);
                        bool on = !(it != l.rx.end() && it->second == hub::profile::kReactions[ri]);
                        if (on) l.rx[A.st.name] = hub::profile::kReactions[ri]; else l.rx.erase(A.st.name);
                        A.lobby.send_msg(std::string("{\"kind\":\"react\",\"to\":") + std::to_string(l.slot) + ",\"n\":" + std::to_string(l.seq) +
                                         ",\"e\":\"" + hub::profile::kReactions[ri] + "\",\"on\":" + (on ? "1" : "0") + "}");
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            if (!l.rx.empty()) {
                std::string chips;
                for (size_t ri = 0; ri < 6; ri++) {
                    int n = 0;
                    for (auto& kv : l.rx) if (kv.second == hub::profile::kReactions[ri]) n++;
                    if (n) chips += std::string(chips.empty() ? "" : "  ") + hub::profile::kReactionLabels[ri] + (n > 1 ? " x" + std::to_string(n) : std::string());
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
                ImGui::TextUnformatted(("    " + chips).c_str());
                ImGui::PopStyleColor();
            }
        }
    }
    if (A.chat_scroll) { ImGui::SetScrollHereY(1.f); A.chat_scroll = false; }
    chat_used = ImGui::GetCursorPosY();
    ImGui::EndChild();
    static bool had_game = false;
    const bool launched = A.proc && !had_game;
    had_game = A.proc != nullptr;
    const bool chat_blocked = osk::hub_input_blocked();
    if (chat_blocked || launched) {   // a game is running / the hub is unfocused: the chat box must not keep the keyboard
        ImGui::ClearActiveID();
        A.chat_buf[0] = 0;
    }
    ImGui::SetNextItemWidth(-1);
    if (osk::input("##chat", "Say something...", A.chat_buf, sizeof A.chat_buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string t = A.chat_buf;
        A.chat_buf[0] = 0;
        if (osk::chat_may_send(t, osk::hub_input_blocked())) {
            int seq = ++A.chat_seq;
            A.lobby.send_msg("{\"kind\":\"chat\",\"n\":" + std::to_string(seq) + ",\"text\":\"" + json::escape(t) + "\"}");
            A.chat.push_back({A.st.name, t, room_my_slot(), seq, {}});
            A.chat_scroll = true;
        }
        if (!osk::hub_input_blocked()) ImGui::SetKeyboardFocusHere(-1);
    }
    end_panel();
    ImGui::Dummy(ImVec2(0, 2 * S));
    room_footer(total_w);
}

void page_online() {
    if (A.in_room) page_online_room();
    else page_online_home();
}

