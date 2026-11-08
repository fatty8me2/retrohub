#include "app.h"
#include "identity_link.h"
#include "cloud_ui_logic.h"

#include <cstdio>
#include <fstream>
#include <memory>

/* ---------- Settings: Account ----------
 * Link a new PC (old PC makes an 8-character code, valid 10 minutes), redeem a code (new PC), and export / import the
 * account as a file. The server and file calls block, so they run on a detached worker thread; the UI polls the result
 * every frame. Nothing here touches the network in a headless screenshot run (g_headless). */

namespace {

const double kCodeLifetime = 600.0;   // seconds, same as the server

/* Result of one worker call. The worker writes under mu and sets done last; the UI reads only after done. */
struct LinkJob {
    std::mutex mu;
    std::atomic<bool> done{false};
    bool ok = false;
    std::string text;         // the code (create) or the error message
    hubnet::Identity id;      // redeem result
};

struct AccountState {
    std::shared_ptr<LinkJob> create_job, redeem_job;
    std::string create_err;
    bool show_code = false;
    std::string code;
    double code_t0 = 0;       // ImGui time when the code was created
    bool permanent = true;    // the account code (XXXX-XXXX-XXXX) never expires; only the HUB_SHOT_ACCOUNT=expired aid is temporary
    bool code_loaded = false; // account_code.txt was read

    char redeem_buf[24] = {0};
    std::string redeem_err;

    std::string io_msg;       // export / import result
    bool io_ok = true;

    bool confirm_open = false;
    hubnet::Identity pending; // identity waiting for the user's confirmation
    bool seeded = false;
    bool auto_adopt = false;  // setup wizard: a redeemed code signs in at once (no "replace account?" prompt: nothing to lose yet)
    bool adopted = false;     // an adoption happened (the wizard moves on)
};
AccountState AC;

/* Linked PCs: the PCs on this account (server list), each with "Stop syncing on this PC" except the one in use. The list is fetched when the
 * Account page opens (and again after 5 minutes or on Refresh): one small request, never a poll. */
namespace hs = hub::saves;
struct PcJob {
    std::atomic<bool> done{false};
    bool ok = false;
    std::string err;
    std::vector<hs::LinkedPc> pcs;
};
struct PcState {
    std::shared_ptr<PcJob> list_job, revoke_job;
    std::vector<hs::LinkedPc> pcs;
    bool loaded = false;
    double loaded_at = -1e9;
    std::string err;
    hs::LinkedPc pending;        // the PC the confirmation is about
    bool confirm_open = false;
    std::string msg;             // "Signed out PC-LIVING."
    bool msg_ok = true;
};
PcState PC;

void pcs_start_list() {
    if (PC.list_job || g_headless) return;
    auto job = std::make_shared<PcJob>();
    PC.list_job = job;
    std::thread([job]() {
        std::string e;
        job->ok = hs::cloud_list_pcs(&job->pcs, &e);
        job->err = e;
        job->done = true;
    }).detach();
}
void pcs_start_revoke(const hs::LinkedPc& pc) {
    if (PC.revoke_job || g_headless) return;
    auto job = std::make_shared<PcJob>();
    PC.revoke_job = job;
    PC.pending = pc;
    const std::string dev = pc.dev;
    std::thread([job, dev]() {
        std::string e;
        job->ok = hs::cloud_revoke_pc(dev, &e);
        job->err = e;
        job->done = true;
    }).detach();
}
void pcs_poll() {
    if (PC.list_job && PC.list_job->done) {
        auto j = PC.list_job;
        PC.list_job.reset();
        PC.loaded = true;
        PC.loaded_at = ImGui::GetTime();
        if (j->ok) { PC.pcs = j->pcs; PC.err.clear(); }
        else PC.err = j->err;
    }
    if (PC.revoke_job && PC.revoke_job->done) {
        auto j = PC.revoke_job;
        PC.revoke_job.reset();
        if (j->ok) {
            PC.msg = "Stopped syncing on " + hubui::cloudui::pc_name(PC.pending) + ".";
            PC.msg_ok = true;
            PC.pcs.erase(std::remove_if(PC.pcs.begin(), PC.pcs.end(), [&](const hs::LinkedPc& p) { return p.dev == PC.pending.dev; }), PC.pcs.end());
            PC.loaded_at = -1e9;   // reload once to be sure
        } else { PC.msg = j->err; PC.msg_ok = false; }
    }
}
void pcs_seed_for_screenshots(const std::string& m) {
    int64_t now = (int64_t)std::time(nullptr);
    PC.loaded = true; PC.loaded_at = 1e12;
    PC.pcs = {{"d-desk", "PC-DESK", now - 86400 * 90, now, true},
              {"d-living", "PC-LIVING-ROOM", now - 86400 * 40, now - 2 * 3600, false},
              {"d-laptop", "LAPTOP-KAI", now - 86400 * 12, now - 3 * 86400, false}};
    if (m == "pcs_confirm") { PC.pending = PC.pcs[1]; PC.confirm_open = true; }
    if (m == "pcs_done") { PC.msg = "Stopped syncing on PC-LIVING-ROOM."; PC.msg_ok = true; PC.pcs.erase(PC.pcs.begin() + 1); }
    if (m == "pcs_err") { PC.pcs.clear(); PC.err = "Can't reach the server. Check your internet connection."; }
}

std::string identity_path() { return hub::data_dir() + "identity.json"; }
std::string code_path() { return hub::data_dir() + "account_code.txt"; }

/* The account code is kept next to identity.json (same protection) so Settings can show it again later. */
void load_saved_code() {
    if (AC.code_loaded || (g_headless && !shot_net())) return;
    AC.code_loaded = true;
    std::ifstream f(code_path());
    std::string c;
    if (f && std::getline(f, c) && hubnet::link_normalize_code(c).size() == 12) { AC.code = c; AC.show_code = true; AC.permanent = true; }
}
void save_code(const std::string& c) {
    std::ofstream f(code_path(), std::ios::trunc);
    if (f) f << c << "\n";
}

/* "9:05" for 545 seconds; never negative. */
std::string countdown_text(double secs_left) {
    int s = (int)std::ceil(secs_left);
    if (s < 0) s = 0;
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

void start_create() {
    if ((g_headless && !shot_net()) || AC.create_job) return;
    hubnet::Identity id;
    AC.create_err.clear();
    if (!hubnet::identity_read_file(identity_path(), &id)) { AC.create_err = "There is no account on this PC yet."; return; }
    auto job = std::make_shared<LinkJob>();
    AC.create_job = job;
    std::string server = server_url();
    std::thread([job, server, id]() {
        std::string code, err;
        bool ok = hubnet::link_keep_code(server, id, &code, &err);
        {
            std::lock_guard<std::mutex> lk(job->mu);
            job->ok = ok;
            job->text = ok ? code : err;
        }
        job->done = true;
    }).detach();
}

void start_redeem() {
    if ((g_headless && !shot_net()) || AC.redeem_job) return;
    std::string code = hubnet::link_normalize_code(AC.redeem_buf);
    AC.redeem_err.clear();
    if (code.empty()) { AC.redeem_err = "Enter the code from your other PC (XXXX-XXXX-XXXX)."; return; }
    auto job = std::make_shared<LinkJob>();
    AC.redeem_job = job;
    std::string server = server_url();
    std::thread([job, server, code]() {
        hubnet::Identity id;
        std::string err;
        bool ok = hubnet::link_redeem_code(server, code, &id, &err);
        {
            std::lock_guard<std::mutex> lk(job->mu);
            job->ok = ok;
            job->text = ok ? std::string() : err;
            job->id = id;
        }
        job->done = true;
    }).detach();
}

/* Applies the confirmed identity: backs up this PC's account, adopts the new one (reconnects if the friends client runs). */
void adopt_pending() {
    if ((g_headless && !shot_net()) || FR.fake) return;
    if (!FR.started) FR.cli.set_paths(hub::data_dir(), hub::saves::vault_dir());
    const bool same_account = FR.cli.my_uid() == AC.pending.user_id;   // signing in as the account already here keeps its saved code
    if (!FR.cli.adopt_identity(AC.pending)) {
        set_toast("Could not switch accounts. The current account is unchanged.", false, 8.0);
        return;
    }
    AC.redeem_buf[0] = 0;
    AC.show_code = false;
    if (!same_account) { std::remove(code_path().c_str()); AC.code_loaded = false; }   // the saved account code belonged to the previous account
    AC.io_msg.clear();
    cloud_account_changed();   // cloud saves follow the account: this PC pulls it (cloud wins where there is no local save)
    if (!FR.started)   // when the friends client runs, its IdentityAdopted event shows the same toast
        push_toast("Signed in as " + hubnet::friend_code(AC.pending.user_id) + " (your account from your other PC)", ui::GOOD, 6.0);
}

void do_export() {
    hubnet::Identity id;
    if (!hubnet::identity_read_file(identity_path(), &id)) { AC.io_ok = false; AC.io_msg = "There is no account on this PC yet."; return; }
    std::string path = pick_save_path(A.hwnd, L"retrohub-account.json", L"Account file", L"*.json", L"json");
    if (path.empty()) return;
    std::string err;
    AC.io_ok = hubnet::identity_export_file(id, path, &err);
    AC.io_msg = AC.io_ok ? "Account saved to " + path : "Export failed: " + err;
}

void do_import(bool clicked) {
    if (g_headless) return;
    std::string path = osk::file("acct_import", clicked, {".json"});
    if (path.empty()) return;
    hubnet::Identity id;
    std::string err;
    if (!hubnet::identity_import_file(path, &id, &err)) { AC.io_ok = false; AC.io_msg = "Import failed: " + err; return; }
    AC.pending = id;
    AC.confirm_open = true;
}

/* Collects finished worker results (called every frame, before drawing). */
void poll_jobs() {
    if (AC.create_job && AC.create_job->done) {
        bool ok; std::string text;
        {
            std::lock_guard<std::mutex> lk(AC.create_job->mu);
            ok = AC.create_job->ok; text = AC.create_job->text;
        }
        AC.create_job.reset();
        if (ok) { AC.show_code = true; AC.code = text; AC.permanent = true; AC.code_t0 = ImGui::GetTime(); AC.create_err.clear(); save_code(text); }
        else AC.create_err = text;
    }
    if (AC.redeem_job && AC.redeem_job->done) {
        bool ok; std::string text; hubnet::Identity id;
        {
            std::lock_guard<std::mutex> lk(AC.redeem_job->mu);
            ok = AC.redeem_job->ok; text = AC.redeem_job->text; id = AC.redeem_job->id;
        }
        AC.redeem_job.reset();
        if (ok) {
            AC.pending = id; AC.redeem_err.clear();
            if (AC.auto_adopt) { adopt_pending(); AC.adopted = true; }
            else AC.confirm_open = true;
        }
        else AC.redeem_err = text;
    }
}

/* Screenshot aid (HUB_SHOT_ACCOUNT=code|expired|redeem|redeem_err|offline|confirm|io_ok|io_err): seeds one state, no network. */
void seed_for_screenshots() {
    if (AC.seeded) return;
    AC.seeded = true;
    const char* s = std::getenv("HUB_SHOT_ACCOUNT");
    if (!s || !g_headless) return;
    std::string m = s;
    if (m.rfind("pcs", 0) == 0) { pcs_seed_for_screenshots(m); return; }
    if (m == "code" || m == "expired") {
        AC.show_code = true;
        AC.code = m == "expired" ? "K7M2-QX9P" : "K7M2-QX9P-4HTW";
        AC.permanent = m != "expired";
        AC.code_t0 = ImGui::GetTime() - (m == "expired" ? kCodeLifetime + 1 : 95.0);
    } else if (m == "redeem") {
        std::snprintf(AC.redeem_buf, sizeof AC.redeem_buf, "%s", "K7M2QX9P");
    } else if (m == "redeem_err") {
        std::snprintf(AC.redeem_buf, sizeof AC.redeem_buf, "%s", "K7M2-QX9Z");
        AC.redeem_err = "That code is wrong or has expired. Make a new one on your other PC.";
    } else if (m == "offline") {
        AC.create_err = "Can't reach the lobby server. Check your internet connection and try again.";
    } else if (m == "confirm") {
        AC.pending.user_id = "abcdefghijklmnopqrstuvwxyz";
        AC.pending.secret = std::string(52, 'a');
        AC.confirm_open = true;
    } else if (m == "io_ok") {
        AC.io_ok = true;
        AC.io_msg = "Account saved to C:\\Users\\you\\Documents\\retrohub-account.json";
    } else if (m == "io_err") {
        AC.io_ok = false;
        AC.io_msg = "Import failed: not a valid identity file";
    }
}

void account_text_error(const std::string& s) {
    ImGui::PushTextWrapPos(0.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_BAD));
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}


/* ---------- Account lifecycle: PCs signed in, sign out, give up the username, delete the account ----------
 * Everything here asks the friends server over the connection the hub already has (no polling: the PC list is asked for once when the
 * page opens and again on Refresh or after a change). Nothing is ever deleted on this PC without a confirmation the person typed or clicked. */
struct LifeState {
    double last_draw = -1e9;   // when the section was last drawn: a gap means the page was just opened
    bool asked = false;        // the PC list was asked for since the page opened (never re-asked on a timer)
    int seen_seq = 0;
    std::string msg;
    bool msg_ok = true;
    enum Kind { None, SignOutPc, SignOutAll, SignOutThis, ReleaseName, Delete } confirm = None;
    bool open = false;
    hubnet::DeviceInfo target;
    char typed[48] = {0};
    bool deleting = false, signing_out = false;
};
LifeState LF;

bool life_online() { return !g_headless && !FR.fake && FR.started && FR.cli.connected(); }

/* Forgets the identity on this PC and starts over as a new account. Saves and games are not touched. */
void finish_forget(const std::string& note) {
    FR.cli.forget_identity_and_restart();
    std::remove(code_path().c_str());
    AC.code_loaded = false; AC.show_code = false; AC.code.clear();
    AC.io_msg.clear();
    LF.deleting = LF.signing_out = false; LF.typed[0] = 0;
    cloud_account_changed();
    LF.msg = note; LF.msg_ok = true;
    push_toast(note, ui::GOOD, 8.0);
}

void life_poll() {
    if (g_headless || FR.fake || !FR.started) return;
    const hubnet::AccountResult& r = FR.cli.account_result();
    if (r.seq != LF.seen_seq) {
        LF.seen_seq = r.seq;
        if (r.op == "sign_out" && LF.signing_out) {
            LF.signing_out = false;
            if (r.ok) finish_forget("This PC is signed out. Your saves on this PC are untouched. Enter your account code to sign back in.");
            else { LF.msg = hubnet::account_result_text(r); LF.msg_ok = false; }
        } else {
            LF.msg = hubnet::account_result_text(r);
            LF.msg_ok = r.ok;
            // The old account codes stopped working after a lock-out. The message says so; a new code is made only when the person clicks.
            if (r.op == "delete_account" && !r.ok) LF.deleting = false;
        }
    }
    if (LF.deleting && FR.cli.status_text().rfind("This account was deleted", 0) == 0)
        finish_forget("Your account was deleted. Saves and games on this PC are untouched. This PC now has a new, empty account.");
}

/* The "PCs signed in" list + the buttons that end an account. */
void draw_life_section() {
    settings_label("PCs signed in to this account", "Each PC that uses this account has its own sign-in. Signing a PC out here stops it from using your "
                                                  "friends, messages and username until someone there enters your account code. Nothing on that PC is deleted.");
    const bool online = life_online();
    const bool dev_ok = online && FR.cli.server_has_devices();   // an older server has no per-PC sign-ins: the PC tools stay off and nothing else changes
    const double tnow = ImGui::GetTime();
    if (tnow - LF.last_draw > 1.0) LF.asked = false;   // the page was just opened
    LF.last_draw = tnow;
    if (dev_ok && !LF.asked) { FR.cli.request_devices(); LF.asked = true; }   // once per opening; Refresh asks again, nothing polls
    const hubnet::DeviceList& dl = FR.cli.devices();
    const int64_t nowms = (int64_t)std::time(nullptr) * 1000;
    if (!online && !g_headless) ImGui::TextDisabled("Connect to see your PCs.");
    else if (online && !dev_ok) ImGui::TextDisabled("Signing single PCs out is not available on the server yet.");
    for (const auto& d : dl.devices) {
        ImGui::PushID(d.id.c_str());
        ImGui::PushFont(ui::F.bold);
        ImGui::TextUnformatted(d.name.empty() ? "A PC" : d.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        if (d.current) ImGui::TextDisabled("this PC");
        else {
            const int64_t days = d.seen > 0 ? (nowms - d.seen) / 86400000 : 0;
            ImGui::TextDisabled("%s", days <= 0 ? "seen today" : (std::to_string(days) + (days == 1 ? " day ago" : " days ago")).c_str());
            ImGui::SameLine(0, sp(2.f));
            if (button("Sign out this PC", ImVec2(0, 0), false, dev_ok)) { LF.target = d; LF.confirm = LifeState::SignOutPc; LF.open = true; }
        }
        ImGui::PopID();
    }
    if (online && dl.loaded && dl.legacy)
        ImGui::TextDisabled("A PC that has not updated RetroHub may still be signed in. It can only be locked out with \"Sign out all other PCs\".");
    if (button("Refresh##life", ImVec2(0, 0), false, dev_ok, ui::Icon::Refresh)) { FR.cli.request_devices(); LF.asked = true; }
    ImGui::SameLine();
    if (button("Sign out all other PCs", ImVec2(0, 0), false, dev_ok)) { LF.confirm = LifeState::SignOutAll; LF.open = true; }
    if (!LF.msg.empty()) {
        if (LF.msg_ok) { ImGui::PushTextWrapPos(0.f); ImGui::TextUnformatted(LF.msg.c_str()); ImGui::PopTextWrapPos(); }
        else account_text_error(LF.msg);
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    settings_label("End this account", "Sign out only affects this PC. Giving up your username frees it for you to pick again (nobody else can take it for 7 days). "
                                      "Deleting the account removes your friends list, messages, username and cloud saves from the server for good.");
    if (A.st.username_claimed && button("Give up my username", ImVec2(0, 0), false, online)) { LF.confirm = LifeState::ReleaseName; LF.open = true; }
    if (A.st.username_claimed) ImGui::SameLine();
    if (button("Sign out of this account", ImVec2(0, 0), false, online)) { LF.confirm = LifeState::SignOutThis; LF.open = true; }
    ImGui::SameLine();
    if (button("Delete my account...", ImVec2(0, 0), false, online)) { LF.confirm = LifeState::Delete; LF.open = true; LF.typed[0] = 0; }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
}

/* One modal for all five confirmations (keeps the popup guard happy: it only opens from this page, never over a game). */
void draw_life_confirm() {
    if (LF.open) { ImGui::OpenPopup("Are you sure?##life"); LF.open = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.5f), sp(2.5f)));
    if (ImGui::BeginPopupModal("Are you sure?##life", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (LF.confirm == LifeState::None) ImGui::CloseCurrentPopup();   // finished (account deleted / signed out) while the box was open
        if (ui::modal_back()) { LF.confirm = LifeState::None; ImGui::CloseCurrentPopup(); }
        ImGui::PushTextWrapPos(sp(46.f));
        load_saved_code();
        const std::string code = AC.code;   // the saved account code, if this PC made one
        auto code_reminder = [&]() {
            if (!code.empty()) ImGui::Text("Your account code: %s. Write it down first.", code.c_str());
            else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_BAD));
                ImGui::TextUnformatted("You have no account code saved on this PC. Make one first (\"Show my account code\" above), or you will not be able to get back in.");
                ImGui::PopStyleColor();
            }
        };
        bool go = false, can = true;
        const char* go_label = "Yes";
        switch (LF.confirm) {
        case LifeState::SignOutPc:
            ImGui::PushFont(ui::F.bold); ImGui::TextUnformatted(("Sign out " + (LF.target.name.empty() ? std::string("this PC") : LF.target.name) + "?").c_str()); ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            ImGui::TextUnformatted("That PC stops using your friends, messages and username right away. It can come back only with a NEW account code, because this also stops your old account codes and any PC that has not updated RetroHub. Nothing on it is deleted.");
            go_label = "Sign it out"; go = false;
            break;
        case LifeState::SignOutAll:
            ImGui::PushFont(ui::F.bold); ImGui::TextUnformatted("Sign out all other PCs?"); ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            ImGui::TextUnformatted("Every other PC is signed out, including any that have not updated RetroHub. All your old account codes stop working, so you will make a new one afterwards (it is not made for you). "
                                   "This PC stays signed in.");
            go_label = "Sign them out";
            break;
        case LifeState::SignOutThis:
            ImGui::PushFont(ui::F.bold); ImGui::TextUnformatted("Sign out of this account on this PC?"); ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            ImGui::TextUnformatted("This PC forgets the account and starts a new, empty one. Your games and saves on this PC stay where they are. To use this account again, enter your account code.");
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            code_reminder();
            can = !code.empty();
            go_label = "Sign out";
            break;
        case LifeState::ReleaseName:
            ImGui::PushFont(ui::F.bold); ImGui::TextUnformatted("Give up your username?"); ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            ImGui::TextUnformatted("Your friends keep you, but your username goes back to the pool. It is held for you for 7 days, so nobody else can take it. You pick a new name when you are ready.");
            go_label = "Give it up";
            break;
        case LifeState::Delete: {
            ImGui::PushFont(ui::F.bold); ImGui::TextUnformatted("Delete your account?"); ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            ImGui::TextUnformatted("This removes your friends list, messages, username and cloud saves from the server. It can't be undone and your account code stops working. "
                                   "Games and saves on this PC are not touched.");
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            const std::string uname = A.st.username_claimed ? A.st.name : std::string();
            const std::string fcode = fr_my_code();
            ImGui::Text("To confirm, type %s", uname.empty() ? fcode.c_str() : (uname + " (or " + fcode + ")").c_str());
            ImGui::SetNextItemWidth(sp(28.f));
            osk::input("##deltyped", "Type it here", LF.typed, sizeof LF.typed, 0, osk::Kind::Text);
            can = hubnet::delete_confirmation_ok(LF.typed, uname, fcode);
            go_label = LF.deleting ? "Deleting..." : "Delete my account";
            if (LF.deleting) can = false;
            break;
        }
        default: break;
        }
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        if (button(go_label, ImVec2(sp(18.f), sp(5.f)), true, can && life_online())) go = true;
        ImGui::SameLine();
        if (button(LF.deleting ? "Close" : "Cancel", ImVec2(sp(12.f), sp(5.f)))) { LF.confirm = LifeState::None; ImGui::CloseCurrentPopup(); }
        if (go) {
            switch (LF.confirm) {
            case LifeState::SignOutPc:
                FR.cli.revoke_device(LF.target.id);
                if (!LF.target.cdev.empty() && A.st.cloud_enabled) pcs_start_revoke(hs::LinkedPc{LF.target.cdev, LF.target.name, 0, 0, false});   // also stop its cloud sync cleanly
                LF.msg.clear();
                break;
            case LifeState::SignOutAll: FR.cli.revoke_other_pcs(); LF.msg.clear(); break;
            case LifeState::SignOutThis: LF.signing_out = true; FR.cli.sign_out_this_pc(); LF.msg.clear(); break;
            case LifeState::ReleaseName: FR.cli.release_username(); LF.msg.clear(); break;   // the sentence comes from the server's answer (account_result), never before
            case LifeState::Delete: LF.deleting = true; FR.cli.delete_account(LF.typed); LF.msg.clear(); break;
            default: break;
            }
            if (LF.confirm != LifeState::Delete) { LF.confirm = LifeState::None; ImGui::CloseCurrentPopup(); }
        }
        if (LF.confirm == LifeState::Delete && !LF.deleting && LF.msg.size() && !LF.msg_ok) account_text_error(LF.msg);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

} // namespace

void draw_account_section() {
    seed_for_screenshots();
    poll_jobs();
    pcs_poll();
    life_poll();

    begin_panel("set_account");
    section_title(A.st.username_claimed && !A.st.name.empty() ? "Account: " + A.st.name : std::string("Account"));
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    un_account_block();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    std::string mycode = fr_my_code();
    if (mycode.empty()) {
        hubnet::Identity id;
        if (hubnet::identity_read_file(identity_path(), &id)) mycode = hubnet::friend_code(id.user_id);
    }
    ImGui::TextUnformatted(("Friend code: " + (mycode.empty() ? std::string("none yet") : mycode)).c_str());
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    /* ---- your PCs (linked PCs) ---- */
    {
        namespace cu = hubui::cloudui;
        settings_label("Your PCs", "The PCs that use this account. \"Stop syncing\" switches cloud saves off on that PC until someone there turns sync back on; "
                                   "nothing is deleted. It does not sign that PC out of your friends or username (use \"PCs signed in\" below for that). You can't do this to the PC you are using.");
        if (hs::cloud_revoked()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_BAD));
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextUnformatted(cu::attention_help("device-revoked").c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            if (button("Turn sync back on", ImVec2(0, 0), true, !g_headless)) { hs::cloud_sign_in_again(); PC.loaded_at = -1e9; set_toast("Cloud sync is back on. Your saves are syncing.", true, 4.0); }
        } else {
            const bool cloud_on = A.st.cloud_enabled;
            if (cloud_on && !g_headless && !PC.list_job && ImGui::GetTime() - PC.loaded_at > 300.0) pcs_start_list();
            const int64_t now = (int64_t)std::time(nullptr);
            for (const auto& p : cu::pcs_sorted(PC.pcs)) {
                ImGui::PushID(p.dev.c_str());
                ImGui::PushFont(ui::F.bold);
                ImGui::TextUnformatted(cu::pc_name(p).c_str());
                ImGui::PopFont();
                ImGui::SameLine();
                ImGui::TextDisabled("%s", cu::pc_last_seen(p, now).c_str());
                if (cu::pc_can_revoke(p)) {
                    ImGui::SameLine(0, sp(2.f));
                    if (button("Stop syncing on this PC", ImVec2(0, 0), false, !PC.revoke_job && !g_headless)) { PC.pending = p; PC.confirm_open = true; }
                }
                ImGui::PopID();
            }
            if (PC.list_job && PC.pcs.empty()) ImGui::TextDisabled("Loading your PCs...");
            else if (PC.loaded && PC.pcs.empty() && PC.err.empty()) ImGui::TextDisabled(cloud_on ? "No other PCs yet." : "Turn on cloud sync to see your PCs.");
            if (!PC.err.empty()) account_text_error(PC.err);
            if (!PC.msg.empty()) {
                if (PC.msg_ok) { ImGui::PushTextWrapPos(0.f); ImGui::TextUnformatted(PC.msg.c_str()); ImGui::PopTextWrapPos(); }
                else account_text_error(PC.msg);
            }
            if (cloud_on && button(PC.list_job ? "Refreshing..." : "Refresh", ImVec2(0, 0), false, !PC.list_job && !g_headless, ui::Icon::Refresh)) pcs_start_list();
        }
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    /* ---- old PC: make a code ---- */
    settings_label("Add this account to another PC", "Your account code: 12 letters and digits. On the other PC choose \"I already have a username on another PC\" in setup "
                                                  "(or enter it below in Settings) and type it. That PC then signs in as you and your cloud saves come with it. "
                                                  "The same code also gets your account and cloud saves back if you lose this PC. Keep it somewhere safe; a new code replaces the old one.");
    load_saved_code();
    bool creating = AC.create_job != nullptr;
    if (button(creating ? "Creating code..." : ((AC.show_code ? "Make a new code" : "Show my account code")), ImVec2(0, 0), false, !creating && !g_headless, ui::Icon::Plus))
        start_create();
    if (!AC.create_err.empty()) account_text_error(AC.create_err);
    if (AC.show_code) {
        double left = AC.permanent ? 1e9 : kCodeLifetime - (ImGui::GetTime() - AC.code_t0);
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        {   // big, readable code in a box: type it on the other PC
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImFont* cf = ui::F.hero ? ui::F.hero : ui::F.title;
            const float fs = cf->FontSize;
            ImVec2 ts = cf->CalcTextSizeA(fs, 1e5f, 0, AC.code.c_str());
            ImVec2 p = ImGui::GetCursorScreenPos();
            const float bw = ts.x + sp(5.f), bh = ts.y + sp(2.5f);
            dl->AddRectFilled(p, p + ImVec2(bw, bh), ui::PANEL3, sp(1.5f));
            dl->AddRect(p, p + ImVec2(bw, bh), left > 0 ? ui::ACCENT : ui::HAIRLINE, sp(1.5f), 0, 2.f);
            dl->AddText(cf, fs, p + ImVec2(sp(2.5f), sp(1.25f)), left > 0 ? ui::TEXT : ui::TEXT3, AC.code.c_str());
            ImGui::Dummy(ImVec2(bw, bh));
            ImGui::SameLine(0, sp(2.f));
            ImGui::BeginGroup();
            if (button("Copy code", ImVec2(sp(14.f), sp(4.5f)), true, left > 0, ui::Icon::None)) {
                SDL_SetClipboardText(AC.code.c_str());
                set_toast("Link code copied.", true, 3.0);
            }
            if (AC.permanent) ImGui::TextDisabled("Does not expire");
            else if (left > 0) {
                ImGui::PushFont(ui::F.bold);
                ImGui::TextColored(ImColor(left < 60 ? ui::WARN : ui::GOOD), "Expires in %s", countdown_text(left).c_str());
                ImGui::PopFont();
            } else ImGui::TextDisabled("This code has expired. Make a new one.");
            ImGui::EndGroup();
            if (!AC.permanent && left > 0) {   // countdown bar
                ImVec2 q = ImGui::GetCursorScreenPos();
                const float fw = std::min(1.f, (float)(left / kCodeLifetime));
                dl->AddRectFilled(q, q + ImVec2(bw, sp(0.5f)), ui::PANEL3, sp(0.25f));
                dl->AddRectFilled(q, q + ImVec2(bw * fw, sp(0.5f)), left < 60 ? ui::WARN : ui::ACCENT, sp(0.25f));
                ImGui::Dummy(ImVec2(bw, sp(0.5f)));
            }
        }
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    /* ---- new PC: enter a code ---- */
    settings_label("This is a new PC - sign in with a code", "Type the code shown on your other PC. This PC then uses that account "
                                                           "instead of its own.");
    ImGui::SetNextItemWidth(sp(26.f));
    bool redeeming = AC.redeem_job != nullptr;
    bool enter = osk::input("##linkcode", "XXXX-XXXX-XXXX", AC.redeem_buf, sizeof AC.redeem_buf,
                                          ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue, osk::Kind::Code);
    ImGui::SameLine();
    if (button(redeeming ? "Checking..." : "Redeem code", ImVec2(0, 0), false, !redeeming && !g_headless && AC.redeem_buf[0] != 0) || enter) {
        if (!redeeming && AC.redeem_buf[0] != 0) start_redeem();
    }
    if (!AC.redeem_err.empty()) account_text_error(AC.redeem_err);
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    /* ---- file: export / import ---- */
    settings_label("Account file", "The file contains your account secret. Keep it private: anyone who has it can sign in as you.");
    if (button("Export account to file...", ImVec2(0, 0), false, !g_headless, ui::Icon::Folder)) do_export();
    ImGui::SameLine();
    const bool imp_clicked = (button("Import account from file...", ImVec2(0, 0), false, !g_headless, ui::Icon::Folder));
    do_import(imp_clicked);
    if (!AC.io_msg.empty()) {
        if (AC.io_ok) { ImGui::PushTextWrapPos(0.f); ImGui::TextUnformatted(AC.io_msg.c_str()); ImGui::PopTextWrapPos(); }
        else account_text_error(AC.io_msg);
    }

    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    draw_life_section();

    /* ---- confirmation before stopping sync on another PC ---- */
    if (PC.confirm_open) { ImGui::OpenPopup("Stop syncing on this PC?##pc"); PC.confirm_open = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.5f), sp(2.5f)));
    if (ImGui::BeginPopupModal("Stop syncing on this PC?##pc", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (ui::modal_back()) ImGui::CloseCurrentPopup();
        ImGui::PushFont(ui::F.bold);
        ImGui::TextUnformatted(hubui::cloudui::pc_revoke_title(PC.pending).c_str());
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::PushTextWrapPos(sp(44.f));
        ImGui::TextUnformatted(hubui::cloudui::pc_revoke_body(PC.pending).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        if (button("Stop syncing", ImVec2(sp(16.f), sp(5.f)), true, !PC.revoke_job)) { pcs_start_revoke(PC.pending); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (button("Cancel", ImVec2(sp(12.f), sp(5.f)))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    /* ---- confirmation before replacing this PC's account ---- */
    if (AC.confirm_open) { ImGui::OpenPopup("Replace the account on this PC?##acct"); AC.confirm_open = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.5f), sp(2.5f)));
    if (ImGui::BeginPopupModal("Replace the account on this PC?##acct", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (ui::modal_back()) ImGui::CloseCurrentPopup();
        ImGui::PushTextWrapPos(sp(44.f));
        ImGui::Text("Account to sign in with: %s", hubnet::friend_code(AC.pending.user_id).c_str());
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::TextUnformatted("This replaces the account on this PC; the current one is backed up. If another PC still uses the current account, that account is left as it is and the other PC keeps working.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        if (button("Replace account", ImVec2(sp(16.f), sp(5.f)), true)) { adopt_pending(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (button("Cancel", ImVec2(sp(12.f), sp(5.f)))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    draw_life_confirm();

    achievements_account_section();

    end_panel();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
}

/* ---------- Setup wizard: "I already have a username on another PC" ----------
 * Draws the code box + button. A good code signs this PC in at once (nothing to lose on a fresh install), turns cloud saves
 * on and starts the first pull. Returns true on the frame the account switched. Server calls run on the worker thread. */
/* QA hooks (userflow.cpp): the same calls the buttons make. */
void account_qa_poll() { poll_jobs(); }
void account_qa_make_code() { start_create(); }
std::string account_qa_code() { return AC.code; }
std::string account_qa_err() { return AC.create_err.empty() ? AC.redeem_err : AC.create_err; }
bool account_qa_busy() { return AC.create_job != nullptr || AC.redeem_job != nullptr; }
void account_qa_redeem(const std::string& code) {
    AC.auto_adopt = true;
    std::snprintf(AC.redeem_buf, sizeof AC.redeem_buf, "%s", code.c_str());
    start_redeem();
}
bool account_redeem_for_wizard() {
    seed_for_screenshots();
    AC.auto_adopt = true;
    poll_jobs();
    bool adopted = false;
    if (AC.adopted) { AC.adopted = false; adopted = true; }
    bool redeeming = AC.redeem_job != nullptr;
    ImGui::SetNextItemWidth(sp(26.f));
    bool enter = osk::input("##wizcode", "XXXX-XXXX-XXXX", AC.redeem_buf, sizeof AC.redeem_buf,
                            ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue, osk::Kind::Code);
    ImGui::SameLine();
    const bool complete = hubui::un::link_code_complete(AC.redeem_buf);
    if (button(redeeming ? "Checking..." : "Sign in", ImVec2(sp(14.f), 0), true, !redeeming && (!g_headless || shot_net()) && complete) || (enter && complete)) {
        if (!redeeming && complete) start_redeem();
    }
    if (!AC.redeem_err.empty()) account_text_error(AC.redeem_err);
    return adopted;
}
void account_redeem_reset() { AC.redeem_buf[0] = 0; AC.redeem_err.clear(); AC.auto_adopt = false; }
