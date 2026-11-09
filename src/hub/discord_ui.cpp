/* Discord Rich Presence: builds the presence from the hub's state once a second and hands it to the pipe thread.
 * Opt-in: Settings > Online, off by default. The Join button is a second opt-in (it shares the room code with Discord friends). */
#include "convenience.h"
#include "discord_logic.h"
#include "invite.h"

#include <ctime>

namespace {

struct DiscordUi {
    double next = 0;
    unsigned nonce = 0;
    std::string last_key;
    int64_t play_start = 0;
    std::string play_title;
} D;

const char* console_key(hub::System s) {
    switch (s) {
        case hub::System::N64: return "n64";
        case hub::System::GBA: return "gba";
        case hub::System::GB: return "gb";
        case hub::System::GBC: return "gbc";
        case hub::System::GC: return "gc";
        case hub::System::N3DS: return "3ds";
        case hub::System::WiiU: return "wiiu";
        case hub::System::NDS: return "nds";
        case hub::System::PS2: return "ps2";
        default: return "";
    }
}

hubdiscord::Presence build() {
    hubdiscord::Presence p;
    if (A.proc || game_active()) {
        p.playing = true;
        p.title = A.proc_title;
        for (auto& g : A.lib)
            if (g.title == A.proc_title) { p.console = hub::console_name(g); p.console_key = console_key(g.system); break; }
        if (p.title.empty()) p.playing = false;
        if (p.playing && D.play_title != p.title) { D.play_title = p.title; D.play_start = (int64_t)std::time(nullptr); }
        p.start_unix = D.play_start;
    } else {
        D.play_title.clear();
    }
    if (A.in_room && !A.code.empty()) {
        p.in_room = true;
        p.room_code = A.code;
        p.party_size = (int)room_members().size();
        p.party_max = std::max(p.party_size, 4);
        p.invite_url = U(invite_web_link(std::wstring(A.code.begin(), A.code.end()), std::wstring(A.st.name.begin(), A.st.name.end())));
    }
    return p;
}

}  // namespace

void discord_tick() {
    if (g_headless) return;
    const double now = ImGui::GetTime();
    if (now < D.next) return;
    D.next = now + 1.0;
    hubdiscord::Opts o;
    o.enabled = A.st.discord_presence;
    o.join = A.st.discord_join;
    std::string json;
    if (o.enabled) {
        hubdiscord::Presence p = build();
        // key without the nonce: send only when something visible changed
        std::string probe = hubdiscord::activity_json(o, p, 0, "0");
        if (probe == D.last_key) return;
        D.last_key = probe;
        json = probe.empty() ? "" : hubdiscord::activity_json(o, p, (int)GetCurrentProcessId(), std::to_string(++D.nonce));
    } else {
        if (D.last_key.empty()) return;   // never enabled this run: stay completely idle
        D.last_key.clear();
    }
    discord_ipc_set(json);
}

void discord_settings_ui() {
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    settings_label("Discord");
    if (ImGui::Checkbox("Show what I'm playing on Discord", &A.st.discord_presence)) mark_dirty();
    text_wrapped_disabled("Your Discord profile shows the game, the console, how long you have played and the party size. Off by default. "
                          "Nothing is shared unless Discord is running on this PC.");
    ImGui::BeginDisabled(!A.st.discord_presence);
    if (ImGui::Checkbox("Add a Join button for my room", &A.st.discord_join)) mark_dirty();
    ImGui::EndDisabled();
    text_wrapped_disabled("Anyone who can see your Discord status can use the button to join your room, because it carries your room's invite link. Off by default.");
    if (!hubdiscord::client_id_valid(hubdiscord::kClientId)) {
        text_wrapped_disabled("Not available in this build yet: the RetroHub Discord application is not set up.");
    } else if (A.st.discord_presence) {
        std::string s = discord_ipc_status();
        text_wrapped_disabled(s.empty() ? "Waiting for something to show (start a game or join a room)." : s);
    }
}
