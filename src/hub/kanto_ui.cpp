#include "app.h"
#include "kanto_pad.h"
#include <shlobj.h>
#include "winutil.h"

/* Kanto Together inside RetroHub (see src/common/kanto.h and docs/KANTO_TOGETHER.md).
 * Library card -> first Play installs + builds the game -> rooms: the host's Launch starts the package's launcher HIDDEN (kanto::Session),
 * its ROOM CODE goes to the guest in an `np_kt` lobby message and the guest's RetroHub runs `join` by itself. */

namespace {

void klog(const std::string& msg) { kanto::write_log(hub::data_dir() + "logs", msg); }


struct Kt {
    kanto::Prep prep;
    kanto::Session sess;
    bool play_wait = false;       // library Play pressed: when the prep is done, point to a room
    bool prep_reported = false;
    bool show_install = false;
    std::string prep_sel;         // room selection the last prefetch was made for
    bool host = false;
    int launch_no = 0;
    bool arm_guest = false;
    bool last_join_failed = false;
    std::string guest_code;       // the host's latest code (np_kt)
    std::string cur_code;         // the code this session joined with
    int sent_gen = 0;
    size_t sent_members = 0;
    kanto::Phase last_phase = kanto::Phase::Idle;
    bool howto = false, intro = false;
    bool fake = false, fake_block = false;   // fake_block: screenshot aid, apply the real launch block to a fake session
    kanto::State fake_state;
    std::map<int, kanto::PkgVer> peer_ver;   // lobby slot -> that player's installed package version (np_kt_ver); cleared when the room changes
    std::string sent_ver;         // package id last broadcast
    size_t ver_members = 0;
    std::string prep_pkg_id;      // content id of the package the current prep was made for
    std::string probe_path;       // package path the last update probe looked at
    std::string probe_id;         // ... and its content id
    std::string peer_fail;        // host: "<friend> can't start Kanto Together: <reason>"
    std::string join_error;       // guest: the last join/start problem (panel offers Retry / a new code)
    char code_buf[16] = {0};
    std::string fake_prep;        // screenshot aid: pretend prep message
    float fake_prog = -1.f;
    hub::GameEntry entry;
};
Kt KT;

bool env_on(const char* n) { const char* v = std::getenv(n); return v && *v && std::string(v) != "0"; }
std::string env_str(const char* n) { const char* v = std::getenv(n); return v ? v : ""; }

kanto::Package package_from(const std::string& path) {
    kanto::Package p;
    if (!kanto::inspect_zip(path, &p) && !kanto::inspect_dir(path, &p)) { p = kanto::Package(); p.path = path; }   // zip or extracted folder (else install() reports it)
    return p;
}

std::vector<kanto::RomCandidate> rom_candidates() {
    std::vector<kanto::RomCandidate> v;
    for (auto& g : A.lib) {
        if (g.system != hub::System::GBA || is_kanto(g)) continue;
        /* FireRed by serial or catalog id; a zipped/odd entry without any serial is matched by SHA-1 alone (kanto::pick_rom) */
        bool maybe = g.serial.empty() || g.catalog_id == "pokefirered" || g.serial == "BPRE";
        if (!maybe) continue;
        std::error_code ec;
        uint64_t sz = (uint64_t)std::filesystem::file_size(std::filesystem::path(W(g.path)), ec);
        v.push_back({g.path, g.serial, g.catalog_id, ec ? 0 : sz});
    }
    return v;
}

/* one staged folder per role: a host session and a guest session on the same PC never share a .sav or a trade journal */
std::string scratch_saves(bool host) { return no_slash(hub::data_dir()) + "\\netplay\\kanto\\saves_" + kanto::role_name(host); }

bool prep_ready() { return KT.prep.ok(); }

/* tell the host (fixed reason codes only: no raw error text, no paths) */
void send_fail(const std::string& reason) {
    if (!room_connected()) return;
    A.lobby.send_msg("{\"kind\":\"np_kt_fail\",\"reason\":\"" + reason + "\"}");
    A.lobby.send_msg("{\"kind\":\"chat\",\"text\":\"[Kanto Together] I can't start it: " + json::escape(kanto::fail_sentence(reason)) + ".\"}");
}

/* My installed package version (only meaningful once the prep is done: the installed copy is what runs). */
kanto::PkgVer my_ver() { return KT.prep.ok() ? kanto::read_pkg_ver(KT.prep.dir()) : kanto::PkgVer(); }

/* The other player in the (2-player) room, or -1. */
int partner_slot() {
    int me = room_my_slot();
    for (auto& m : room_members()) if (m.slot != me) return m.slot;
    return -1;
}

/* Seamless updates: a newer package in the library (zip replaced / new build dropped in a game folder or Downloads) than the one the
 * finished prep was made for -> forget the prep so the next ensure_prep reinstalls over the old copy (saves live in the vault, untouched). */
void check_package_update(const hub::GameEntry& e) {
    if (KT.fake || KT.prep.busy() || !KT.prep.ok() || KT.sess.active() || A.proc) return;
    if (e.path != KT.probe_path) {
        KT.probe_path = e.path;
        KT.probe_id = package_from(e.path).content_id;
    }
    if (!KT.probe_id.empty() && !KT.prep_pkg_id.empty() && KT.probe_id != KT.prep_pkg_id) {
        klog("package update: " + KT.prep_pkg_id + " -> " + KT.probe_id + " (reinstalling)");
        KT.prep.reset();
        KT.prep_reported = false;
        KT.sent_ver.clear();
    }
}

void ensure_prep(const hub::GameEntry& e) {
    if (g_headless || KT.fake || KT.prep.busy()) return;
    check_package_update(e);
    if (KT.prep.ok()) return;
    if (KT.prep.failed()) return;   // stays failed until Retry / another selection
    KT.prep.reset();
    KT.prep_reported = false;
    kanto::Package pk = package_from(e.path);
    KT.prep_pkg_id = pk.content_id;
    KT.probe_path = e.path;
    KT.probe_id = pk.content_id;
    KT.prep.start(pk, hub::data_dir(), rom_candidates(), env_str("RETROHUB_KT_DESKTOP"));
}

bool mute_tests() { return env_on("RETRORUN_MUTE") || env_on("RETROHUB_KT_MUTE") || env_on("SSB64_MUTE"); }

bool start_session(const hub::GameEntry& e, bool host, const std::string& code) {
    if (g_headless) { set_notice("Headless test run: launch skipped."); return false; }
    if (A.proc || KT.sess.active()) { set_notice("A game is already running."); return false; }
    if (kanto::Session::stopping() > 0) { set_notice("The previous Kanto Together session is still closing - try again in a moment."); return false; }
    kanto::LaunchOpts o;
    o.host = host;
    o.code = code;
    o.dir = KT.prep.dir();
    o.rom = kanto::rom_path(o.dir);
    o.saves = scratch_saves(host);
    o.sound = !mute_tests();
    o.relay = env_str("RETROHUB_KT_RELAY");
    o.desktop = env_str("RETROHUB_KT_DESKTOP");
    if (std::string busy = kanto::running_instance_block(); !busy.empty()) { set_notice("Kanto Together: " + busy, 14.0); klog("start refused: another instance"); return false; }
    { hubctl::Store pst; hubctl::load_store(hub::data_dir(), "", &pst); kanto::prepare_pad(o.dir, hubctl::resolve(pst, "retro")); }   // Xbox pad bindings into the package mGBA (write once per profile)
    std::string vault = no_slash(hub::saves::battery_dir(sys_short(e), e.key));
    kanto::stage_saves(vault, o.saves, host);   // vault (per-role .sav + trade journal) -> this role's staged folder (newest wins)
    std::string err;
    if (!KT.sess.start(o, &err)) {
        set_notice("Kanto Together: " + err, 12.0);
        if (!host) { KT.join_error = "The game could not be started on this PC."; send_fail("start"); }
        return false;
    }
    KT.join_error.clear();
    KT.last_join_failed = false;
    klog(std::string("start ") + (host ? "HOST" : "GUEST") + " launch=" + std::to_string(KT.launch_no) + " relay=" + (o.relay.empty() ? "(default)" : o.relay));
    KT.sess.set_line_sink([](const std::string& l) { klog(l.compare(0, 9, "[launch] ") == 0 && l.find("mGBA.exe") != std::string::npos ? std::string("| [launch] (mGBA command line)") : "| " + l); });
    A.proc = KT.sess.dup_handle();
    A.job_h = nullptr;
    A.proc_title = e.title;
    A.run = run_info_for(e, false);   // a real save: play time + vault like an offline session
    A.run.start_unix = unix_now();
    A.run.start_tick = GetTickCount64();
    A.run.pad_exit = true;
    A.run.room = host ? false : true;   // a guest follows the host: the session is gone when the room host leaves
    A.run.pad_exit_fn = []() { KT.sess.stop(); };
    std::string saves = o.saves;
    const bool was_host = host;
    A.run.after_exit = [vault, saves, was_host]() {
        klog("session ended (launcher exited)");
        KT.sess.stop();                    // the launcher already exited (mGBA closed and flushed): this only releases handles
        kanto::flush_saves(saves, vault, was_host);   // staged .sav + journal -> vault (per role), before the sync runs
        KT.arm_guest = false;
        KT.cur_code.clear();
        KT.guest_code.clear();             // a code of a finished session must never auto-start the next one
        if (was_host && room_connected()) A.lobby.send_msg("{\"kind\":\"np_kt\",\"code\":\"\",\"launch\":" + std::to_string(KT.launch_no) + "}");
    };
    KT.last_phase = kanto::Phase::Starting;
    KT.cur_code = code;
    KT.entry = e;
    if (!A.st.kt_intro_seen) KT.intro = true;
    return true;
}

}  // namespace

bool kt_session_active() { return KT.sess.active(); }

bool is_kanto(const hub::GameEntry& e) { return e.key == kanto::kKey; }

void kt_scan_append(std::vector<hub::GameEntry>* out, const std::vector<std::string>& folders) {
    std::vector<kanto::Package> pk = kanto::find_packages(folders);
    /* a package the user just downloaded and has not moved yet: Downloads, top level only, kanto-named zips/folders only */
    {
        PWSTR dl = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &dl)) && dl) {
            for (auto& p : kanto::find_packages({winutil::to_utf8(dl)}, 1)) {
                bool dup = false;
                for (auto& q : pk) if (q.path == p.path) dup = true;
                if (!dup) pk.push_back(p);
            }
        }
        if (dl) CoTaskMemFree(dl);
    }
    std::string path;
    uint64_t size = 0;
    int bi = kanto::pick_package(pk);   // newest build; a zip and its extracted folder are the same build
    if (bi >= 0) { path = pk[bi].path; size = pk[bi].size; }
    if (path.empty() && env_on("HUB_SHOT_KT")) path = "C:\\Games\\KantoTogether-ROM.zip";   // screenshot aid
    if (path.empty()) return;
    hub::GameEntry e;
    e.key = kanto::kKey;
    e.system = hub::System::GBA;
    e.title = kanto::kTitle;
    e.path = path;
    e.size = size;
    e.catalog_id = kanto::kCatalogId;
    e.issue = kanto::kIssue;
    out->push_back(e);
}

void kt_open_howto() { KT.howto = true; }

void kt_leave_room() {
    KT.arm_guest = false;
    KT.guest_code.clear();
    KT.peer_ver.clear();
    KT.sent_ver.clear();
    KT.ver_members = 0;
    if (KT.prep.failed()) KT.prep.reset();
}

void kt_play(const hub::GameEntry& e) {
    if (KT.sess.active() && KT.sess.running()) { set_notice("Kanto Together is already running."); return; }
    if (KT.fake) { KT.show_install = true; return; }
    KT.play_wait = true;
    KT.show_install = true;
    if (KT.prep.failed()) KT.prep.reset();
    ensure_prep(e);
    if (!KT.prep.busy() && !KT.prep.ok() && !KT.prep.failed() && g_headless) set_notice("Headless test run: install skipped.");
}

std::string kt_launch_block(const hub::GameEntry& e) {
    (void)e;
    if (KT.fake && !KT.fake_block) return "";
    int n = (int)room_members().size();
    if (n != 2) return "Kanto Together is for exactly 2 players (" + std::to_string(n) + " in the room)";
    if (A.proc || KT.sess.active()) return "In game...";
    if (KT.prep.busy()) return KT.prep.message();
    if (KT.prep.failed()) return KT.prep.message();
    if (!KT.prep.ok()) return "Preparing Kanto Together...";
    /* both players must run the same package build, or the start-up barriers switch off and the old behaviour returns */
    if (!KT.fake && room_connected()) {
        int ps = partner_slot();
        auto it = KT.peer_ver.find(ps);
        if (it != KT.peer_ver.end()) {
            kanto::VerMatch m = kanto::compare_versions(my_ver(), it->second);
            if (m != kanto::VerMatch::Same && m != kanto::VerMatch::Unknown) return kanto::version_block_text(m, member_name(ps));
        }
    }
    return "";
}

void kt_on_ver(int slot, const std::string& id, const std::string& built) {
    if (slot == room_my_slot() || !kanto::valid_ver_id(id)) return;
    kanto::PkgVer v;
    v.id = id;
    v.built = built.substr(0, 40);
    KT.peer_ver[slot] = v;
    klog("np_kt_ver from slot " + std::to_string(slot) + " id=" + id.substr(0, 10) + " built=" + v.built);
}

void kt_launch(const hub::GameEntry& e, int launch_no, bool is_host) {
    KT.launch_no = launch_no;
    KT.host = is_host;
    KT.entry = e;
    if (is_host) {
        KT.peer_fail.clear();
        if (!prep_ready()) { set_notice("Kanto Together is not ready yet: " + KT.prep.message(), 10.0); return; }
        if (std::string vb = kt_launch_block(e); !vb.empty() && vb.find("Kanto Together package") != std::string::npos) {   // version mismatch (the Launch button already blocks it; this is the net)
            set_notice(vb, 12.0);
            A.lobby.send_msg("{\"kind\":\"np_abort\",\"reason\":\"" + json::escape(vb) + "\"}");
            return;
        }
        KT.sent_gen = 0;
        KT.sent_members = 0;
        if (start_session(e, true, "")) push_toast("Starting Kanto Together - your partner joins automatically.", ui::GOOD, 6.0);
        return;
    }
    KT.arm_guest = true;
    KT.guest_code.clear();   // the code for THIS launch follows in an np_kt message; an old one is stale
    KT.join_error.clear();
    ensure_prep(e);
    push_toast("Joining the host's Kanto Together...", ui::GOOD, 6.0);
}

void kt_on_code(const std::string& code, int launch_no, const std::string& pkg_id) {
    klog("np_kt received code=" + (code.empty() ? std::string("(none)") : code) + " launch=" + std::to_string(launch_no));
    if (code.empty()) { KT.guest_code.clear(); return; }   // the host's session ended
    if (kanto::valid_ver_id(pkg_id) && KT.prep.ok()) {      // the host's package build rides on the code message: refuse a mismatch
        kanto::PkgVer host_v; host_v.id = pkg_id;
        kanto::PkgVer mine = my_ver();
        auto it = KT.peer_ver.find(0);
        if (it != KT.peer_ver.end() && it->second.id == pkg_id) host_v.built = it->second.built;
        kanto::VerMatch m = kanto::compare_versions(mine, host_v);
        if (m == kanto::VerMatch::Different || m == kanto::VerMatch::MineOlder || m == kanto::VerMatch::MineNewer) {
            std::string reason = kanto::version_reason(m);
            if (!reason.empty()) {
                KT.join_error = m == kanto::VerMatch::MineOlder ? "Your Kanto Together package is older than the host's - update it (replace the package zip; RetroHub reinstalls it)."
                              : m == kanto::VerMatch::MineNewer ? "The host has an older Kanto Together package - ask them to update."
                                                                : "You and the host have different Kanto Together packages.";
                klog("version mismatch with host: " + reason);
                KT.arm_guest = false;
                send_fail(reason);
                return;
            }
        }
    }
    std::string c = kanto::normalize_code(code);
    if (kanto::valid_code(c)) { KT.guest_code = c; KT.join_error.clear(); }
}

void kt_on_peer_fail(int slot, const std::string& reason) {
    if (!is_host()) return;
    std::string name = member_name(slot);
    KT.peer_fail = name + " can't start Kanto Together: " + kanto::fail_sentence(kanto::valid_fail_reason(reason) ? reason : "");
    push_toast(KT.peer_fail, ui::WARN, 12.0);
    if (KT.sess.active() && KT.sess.state().phase != kanto::Phase::Connected) KT.sess.stop();   // stop waiting for a partner who cannot come
}

void kt_on_request(int slot) {   // a guest asks for the code again (join failed / typo)
    (void)slot;
    if (is_host() && KT.sess.active()) KT.sent_gen = -1;
}

void kt_tick() {
    if (KT.fake) return;
    /* what the room has selected: warm up the install + patched game so Launch never waits */
    if (room_connected() && !A.selected_game.empty()) {
        if (A.selected_game != KT.prep_sel) {
            KT.prep_sel = A.selected_game;
            if (KT.prep.failed()) KT.prep.reset();
        }
        if (A.selected_game == kanto::kCatalogId)
            if (const hub::GameEntry* g = find_room_entry(A.selected_game)) ensure_prep(*g);
    }
    /* tell the room which package build is installed here (re-sent when the member list or the build changes) */
    if (room_connected() && A.selected_game == kanto::kCatalogId && KT.prep.ok()) {
        kanto::PkgVer mv = my_ver();
        size_t nm = room_members().size();
        if (nm != KT.ver_members) { KT.ver_members = nm; KT.peer_ver.clear(); KT.sent_ver.clear(); }   // member list changed: everybody announces again
        std::string key = mv.id;
        if (key != KT.sent_ver && !mv.id.empty()) {
            KT.sent_ver = key;
            A.lobby.send_msg("{\"kind\":\"np_kt_ver\",\"id\":\"" + mv.id + "\",\"built\":\"" + json::escape(mv.built) + "\",\"bar\":" + (kanto::has_barriers(KT.prep.dir()) ? "1" : "0") + "}");
        }
    }
    if (KT.prep.failed() && !KT.prep_reported) {
        KT.prep_reported = true;
        if (KT.play_wait || KT.arm_guest) set_notice(KT.prep.message(), 20.0);
        if (KT.arm_guest) {
            std::string why = KT.prep.reason().empty() ? "install" : KT.prep.reason();
            KT.join_error = "Kanto Together could not be prepared on this PC: " + kanto::fail_sentence(why).substr(0) + ".";
            send_fail(why);
            KT.arm_guest = false;
        }
        KT.play_wait = false;
        KT.show_install = false;
    }
    if (KT.play_wait && KT.prep.ok()) {
        KT.play_wait = false;
        KT.show_install = false;
        push_toast("Kanto Together is ready. It is an online game: host a room (or join your friend's) and pick it.", ui::GOOD, 10.0);
        A.pending_room_game = kanto::kCatalogId;
        A.page = Page::Online;
        KT.howto = !A.st.kt_intro_seen;
    }
    if (KT.sess.active()) {
        KT.sess.update();
        const kanto::State& st = KT.sess.state();
        if (st.join_failed != KT.last_join_failed) {   // "Could not join": the bridge keeps retrying, so this is only a hint, never the end
            KT.last_join_failed = st.join_failed;
            if (st.join_failed && !KT.host) {
                klog("join not accepted yet: " + st.last_error);
                push_toast("Kanto Together: could not join yet (" + st.last_error + ") - still trying.", ui::WARN, 8.0);
                KT.join_error = "Could not join: " + st.last_error;
            }
        }
        if (st.phase != KT.last_phase) {
            klog(std::string("phase ") + kanto::phase_name(KT.last_phase) + " -> " + kanto::phase_name(st.phase));
            if (st.phase == kanto::Phase::Connected) push_toast("Kanto Together: your partner is connected.", ui::GOOD, 5.0);
            else if (st.phase == kanto::Phase::Failed) {
                push_toast("Kanto Together: " + st.detail, ui::WARN, 10.0);
                if (!KT.host) KT.join_error = st.detail;
            }
            if (st.phase == kanto::Phase::Connected) KT.join_error.clear();
            KT.last_phase = st.phase;
        }
        if (KT.host && room_connected()) {
            size_t members = A.lobby.members().size();
            if (!st.code.empty() && (st.code_gen != KT.sent_gen || members != KT.sent_members)) {
                KT.sent_gen = st.code_gen;
                KT.sent_members = members;
                klog("np_kt sent code=" + st.code + " gen=" + std::to_string(st.code_gen) + " members=" + std::to_string(members));
                A.lobby.send_msg("{\"kind\":\"np_kt\",\"code\":\"" + st.code + "\",\"launch\":" + std::to_string(KT.launch_no) + ",\"pkg\":\"" + my_ver().id + "\"}");
            }
        } else if (!KT.host && kanto::valid_code(KT.guest_code) && KT.guest_code != KT.cur_code) {
            /* the host restarted/refreshed its code. Only a guest that never got connected needs it: a connected one reconnects by
             * itself, and `code` would drop its campaign and re-sync it (its game would look reset while the host carries on). */
            klog(std::string("new host code ") + KT.guest_code + (kanto::guest_auto_rejoin_ok(st) ? ": sending `code` (guest never connected)" : ": NOT sent (guest already connected)"));
            if (kanto::guest_auto_rejoin_ok(st)) KT.sess.rejoin(KT.guest_code);
            KT.cur_code = KT.guest_code;
        }
    } else if (KT.arm_guest && !A.proc && kanto::valid_code(KT.guest_code) && prep_ready()) {
        KT.arm_guest = false;
        start_session(KT.entry, false, KT.guest_code);
    }
}

/* ---------------- UI ---------------- */

static const kanto::State& cur_state() { return KT.fake ? KT.fake_state : KT.sess.state(); }

static ImU32 phase_col(kanto::Phase p) {
    switch (p) {
        case kanto::Phase::Connected: return ui::GOOD;
        case kanto::Phase::Failed: return ui::BAD;
        case kanto::Phase::Reconnecting:
        case kanto::Phase::PartnerAway: return ui::WARN;
        default: return ui::ACCENT;
    }
}

void kt_room_panel(float width, bool early) {
    bool sel = A.selected_game == kanto::kCatalogId;
    bool live = (KT.fake && KT.fake_state.phase != kanto::Phase::Idle && KT.fake_state.phase != kanto::Phase::Failed) || KT.sess.active();
    if (!sel && !live) return;
    if (early != live) return;
    ImGui::Dummy(ImVec2(0, sp(0.75f)));
    begin_panel("kt_panel", ImVec2(width, 0));
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Kanto Together");
    ImGui::PopFont();
    ImGui::SameLine(0, sp(2.f));
    if (button("How to play", ImVec2(sp(14.f), sp(4.f)))) KT.howto = true;

    if (!KT.peer_fail.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("%s", KT.peer_fail.c_str());
        ImGui::PopStyleColor();
    }
    bool busy = KT.fake ? KT.fake_prog >= 0 : KT.prep.busy();
    if (busy) {
        float pr = KT.fake ? KT.fake_prog : KT.prep.progress();
        ImGui::ProgressBar(KT.fake || KT.prep.stage() == kanto::Prep::Stage::Installing ? pr : -1.0f * (float)ImGui::GetTime(), ImVec2(sp(32.f), 0));
        text_wrapped_disabled(KT.fake ? KT.fake_prep : KT.prep.message());
    } else if (!KT.fake && KT.prep.failed()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("%s", KT.prep.message().c_str());
        ImGui::PopStyleColor();
        if (button("Retry", ImVec2(sp(10.f), sp(4.f)))) { KT.prep.reset(); KT.prep_reported = false; }
    } else if (!live) {
        text_wrapped_disabled(KT.prep.ok() ? "Ready. Everyone ready? The host presses Launch; the game opens on both PCs and connects by itself."
                                           : "Preparing...");
    }
    const bool guest_stuck = !KT.fake ? (!KT.host && !KT.join_error.empty() && !(live && cur_state().phase == kanto::Phase::Connected))
                                      : (KT.fake_state.phase == kanto::Phase::Failed && !KT.fake_state.host);
    if (guest_stuck) {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("%s", KT.fake ? "Could not join: no such room" : KT.join_error.c_str());
        ImGui::PopStyleColor();
        text_wrapped_disabled("Ask the host for a new code (the game keeps trying by itself), or type the code the host gives you.");
        if (button("Ask the host for a new code", ImVec2(sp(26.f), sp(5.f))) && !KT.fake) { klog("np_kt_req sent"); A.lobby.send_msg("{\"kind\":\"np_kt_req\"}"); }
        ImGui::SameLine();
        std::string retry_code = !KT.guest_code.empty() ? KT.guest_code : KT.cur_code;
        if (button("Retry", ImVec2(sp(10.f), sp(5.f)), false, KT.fake || kanto::valid_code(retry_code))) {
            if (!KT.fake) {
                KT.join_error.clear();
                if (KT.sess.active() && KT.sess.running()) KT.sess.rejoin(retry_code);
                else { KT.guest_code = retry_code; KT.arm_guest = true; }
            }
        }
        ImGui::SetNextItemWidth(sp(22.f));
        osk::input("##ktcode", "Room code (10 characters)", KT.code_buf, sizeof KT.code_buf, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank, osk::Kind::Code);
        ImGui::SameLine();
        std::string typed = kanto::normalize_code(KT.code_buf);
        if (button("Join with this code", ImVec2(sp(20.f), sp(5.f)), true, KT.fake || kanto::valid_code(typed))) {
            if (!KT.fake) {
                KT.join_error.clear();
                if (KT.sess.active() && KT.sess.running()) { klog("manual `code` sent"); KT.sess.rejoin(typed); KT.cur_code = typed; }
                else { KT.guest_code = typed; KT.arm_guest = true; }
                KT.code_buf[0] = 0;
            }
        }
    }
    if (live) {
        const kanto::State& st = cur_state();
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = sp(0.7f);
        dl->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f), r, phase_col(st.phase), 16);
        ImGui::SetCursorScreenPos(ImVec2(p.x + r * 2 + sp(1.f), p.y));
        ImGui::PushStyleColor(ImGuiCol_Text, phase_col(st.phase));
        ImGui::TextWrapped("%s", (st.detail.empty() ? kanto::phase_text(st.phase, st.host) : st.detail).c_str());
        ImGui::PopStyleColor();
        if (st.barrier_waiting) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
            ImGui::TextWrapped("%s", st.barrier_text.c_str());
            ImGui::PopStyleColor();
            if (button("Skip waiting", ImVec2(sp(16.f), sp(4.f))) && !KT.fake) KT.sess.go();
        } else if (!st.barrier_text.empty()) text_wrapped_disabled(st.barrier_text);
        if (st.legacy_peer || st.barriers_off)
            text_wrapped_disabled(st.legacy_peer ? "Your partner's Kanto Together package is older: the games do not wait for each other at the title screen. Both of you should update the package."
                                                 : "Start-up waiting is off in this game build; both games continue on their own.");
        if (st.host && !st.code.empty() && st.phase != kanto::Phase::Connected)
            text_wrapped_disabled("Your partner's RetroHub gets the connection code from this room automatically.");
        if (!st.host && st.synced_story)
            text_wrapped_disabled("Synced to the host's story. Your game may jump to the host's spot: that is the co-op sync, not a reset.");
        if (!st.last_error.empty() && st.phase != kanto::Phase::Connected) text_wrapped_disabled(st.last_error);

        if (st.phase == kanto::Phase::Connected || st.phase == kanto::Phase::Reconnecting || st.phase == kanto::Phase::PartnerAway) {
            const kanto::Trade& t = st.trade;
            ImGui::Dummy(ImVec2(0, sp(1.f)));
            ImGui::PushFont(ui::F.bold);
            ImGui::TextUnformatted("Trade");
            ImGui::PopFont();
            text_wrapped_disabled("Stand still in the game, pick the team slot to offer (1 = first Pokemon). You both see the other's offer, then both press Confirm.");
            bool can_offer = st.phase == kanto::Phase::Connected;
            for (int i = 1; i <= 6; ++i) {
                if (i > 1) ImGui::SameLine();
                char lab[24];
                std::snprintf(lab, sizeof lab, "Slot %d##ts%d", i, i);
                bool on = t.phase == kanto::TradePhase::Offered && t.my_slot == i;
                ImGui::PushStyleColor(ImGuiCol_Text, on ? ui::GOOD : ui::TEXT);
                if (button(lab, ImVec2(sp(7.f), sp(4.f)), on, can_offer) && !KT.fake) KT.sess.trade(i);
                ImGui::PopStyleColor();
            }
            ImGui::Text("You offer: %s", t.mine.empty() ? "-" : t.mine.c_str());
            ImGui::Text("Partner offers: %s", t.theirs.empty() ? "-" : t.theirs.c_str());
            bool both = !t.mine.empty() && !t.theirs.empty() && t.phase == kanto::TradePhase::Offered;
            if (button("Confirm trade", ImVec2(sp(16.f), sp(5.f)), true, both && !t.i_confirmed) && !KT.fake) KT.sess.confirm();
            ImGui::SameLine();
            if (button("Cancel trade", ImVec2(sp(16.f), sp(5.f)), false, t.phase == kanto::TradePhase::Offered) && !KT.fake) KT.sess.cancel();
            if (t.i_confirmed || t.peer_confirmed) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s%s", t.i_confirmed ? "You confirmed. " : "", t.peer_confirmed ? "Partner confirmed." : "");
            }
            if (!t.note.empty()) text_wrapped_disabled(t.note);
            if (st.trades_done > 0) ImGui::TextDisabled("Trades this session: %d", st.trades_done);
        }
    }
    end_panel();
}

static void modal_begin(const char* name, bool* open) {
    ImGui::OpenPopup(name);
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(78.f), ImGui::GetIO().DisplaySize.x * 0.94f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    *open = ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(2);
}

void kt_draw_modals() {
    /* install / build progress (library Play) */
    if (KT.show_install && (KT.fake || KT.prep.busy())) {
        bool open = false;
        modal_begin("Kanto Together setup", &open);
        if (open) {
            ImGui::TextWrapped("Setting up Kanto Together. RetroHub copies your package into its own data folder (your zip is not changed) and builds the game from your own FireRed ROM. This happens once.");
            ImGui::Dummy(ImVec2(0, sp(1.f)));
            float pr = KT.fake ? KT.fake_prog : (KT.prep.stage() == kanto::Prep::Stage::Installing ? KT.prep.progress() : -1.0f * (float)ImGui::GetTime());
            ImGui::ProgressBar(pr, ImVec2(-1, 0));
            text_wrapped_disabled(KT.fake ? KT.fake_prep : KT.prep.message());
            ImGui::Dummy(ImVec2(0, sp(1.f)));
            if ((button("Cancel", ImVec2(sp(14.f), sp(5.f))) || ui::modal_back()) && !KT.fake) { KT.prep.cancel(); KT.play_wait = false; KT.show_install = false; }
            ImGui::EndPopup();
        }
        return;
    }
    /* first-session note, once */
    if (KT.intro && !KT.howto) {
        bool open = false;
        modal_begin("Kanto Together first session", &open);
        if (open) {
            ImGui::PushFont(ui::F.h2);
            ImGui::TextUnformatted("Your first session");
            ImGui::PopFont();
            ImGui::TextWrapped("%s", kanto::first_session_text(KT.host).c_str());
            ImGui::Dummy(ImVec2(0, sp(1.f)));
            if (button("Got it", ImVec2(sp(14.f), sp(5.f)), true) || ui::modal_back()) {
                KT.intro = false;
                A.st.kt_intro_seen = true;
                mark_dirty();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        return;
    }
    if (KT.howto) {
        bool open = false;
        modal_begin("How to play Kanto Together", &open);
        if (open) {
            ImGui::PushFont(ui::F.h2);
            ImGui::TextUnformatted("How to play Kanto Together");
            ImGui::PopFont();
            ImGui::BeginChild("kt_howto_body", ImVec2(0, std::min(sp(46.f), ImGui::GetIO().DisplaySize.y * 0.6f)), ImGuiChildFlags_None);
            std::stringstream ss(kanto::howto_text());
            std::string line;
            while (std::getline(ss, line)) {
                if (line.compare(0, 2, "# ") == 0) {
                    ImGui::Dummy(ImVec2(0, sp(0.75f)));
                    ImGui::PushFont(ui::F.bold);
                    ImGui::PushStyleColor(ImGuiCol_Text, COL_ACCENT);
                    ImGui::TextUnformatted(line.c_str() + 2);
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                } else {
                    ImGui::PushTextWrapPos(0.f);
                    ImGui::TextUnformatted(line.c_str());
                    ImGui::PopTextWrapPos();
                }
            }
            ImGui::Dummy(ImVec2(0, sp(1.f)));
            text_wrapped_disabled(kanto::credit_text());
            ImGui::EndChild();
            ImGui::Dummy(ImVec2(0, sp(1.f)));
            if (button("Close", ImVec2(sp(14.f), sp(5.f)), true) || ui::modal_back()) {
                KT.howto = false;
                A.st.kt_intro_seen = true;
                mark_dirty();
                ImGui::CloseCurrentPopup();
            }
            std::string lic = KT.prep.dir() + "LICENSES.txt";
            if (!KT.fake && file_exists(lic)) {
                ImGui::SameLine();
                if (button("Open licences", ImVec2(sp(16.f), sp(5.f)))) ShellExecuteW(nullptr, L"open", W(lic).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::EndPopup();
        }
    }
}

void kt_shot_init() {
    const char* m = std::getenv("HUB_SHOT_KT");
    if (!m) return;
    std::string s = m;
    KT.fake = true;
    kanto::State& st = KT.fake_state;
    st.host = true;
    st.code = "3F9A0C7D21";
    st.phase = kanto::Phase::Connected;
    st.detail = kanto::phase_text(st.phase, true);
    if (s == "connecting") { st.phase = kanto::Phase::WaitingForPartner; st.detail = kanto::phase_text(st.phase, true); }
    if (s == "joinfail") { st.host = false; st.phase = kanto::Phase::Failed; st.detail = "Could not join: no such room"; }
    if (s == "peerfail") KT.peer_fail = "Mia can't start Kanto Together: " + kanto::fail_sentence("no_rom") + ".";
    if (s == "reconnecting") { st.phase = kanto::Phase::Reconnecting; st.detail = kanto::phase_text(st.phase, true); }
    if (s == "trade") {
        st.trade.phase = kanto::TradePhase::Offered;
        st.trade.my_slot = 2;
        st.trade.mine = "CHARMANDER lvl 12";
        st.trade.theirs = "PIDGEY lvl 9";
        st.trade.i_confirmed = true;
        st.trade.note = "You confirmed. Waiting for your partner.";
    }
    if (s == "installing") { KT.fake_prog = 0.62f; KT.fake_prep = "Installing Kanto Together..."; KT.show_install = true; }
    if (s == "block") KT.fake_block = true;   // with HUB_SHOT_ROOM_N=3: shows the "exactly 2 players" block reason
    if (s == "howto") KT.howto = true;
    if (s == "intro") KT.intro = true;
    if (s != "installing") { /* connected / trade etc. run a live (fake) session panel */ }
    if (s == "howto" || s == "intro") KT.fake_state.phase = kanto::Phase::Idle;
}
