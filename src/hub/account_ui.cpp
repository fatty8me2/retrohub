#include "app.h"
#include "identity_link.h"

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

std::string identity_path() { return hub::data_dir() + "identity.json"; }

/* "9:05" for 545 seconds; never negative. */
std::string countdown_text(double secs_left) {
    int s = (int)std::ceil(secs_left);
    if (s < 0) s = 0;
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

void start_create() {
    if (g_headless || AC.create_job) return;
    hubnet::Identity id;
    AC.create_err.clear();
    if (!hubnet::identity_read_file(identity_path(), &id)) { AC.create_err = "There is no account on this PC yet."; return; }
    auto job = std::make_shared<LinkJob>();
    AC.create_job = job;
    std::string server = server_url();
    std::thread([job, server, id]() {
        std::string code, err;
        bool ok = hubnet::link_create_code(server, id, &code, &err);
        {
            std::lock_guard<std::mutex> lk(job->mu);
            job->ok = ok;
            job->text = ok ? code : err;
        }
        job->done = true;
    }).detach();
}

void start_redeem() {
    if (g_headless || AC.redeem_job) return;
    std::string code = hubnet::link_normalize_code(AC.redeem_buf);
    AC.redeem_err.clear();
    if (code.empty()) { AC.redeem_err = "Enter the 8-character code from your other PC (XXXX-XXXX)."; return; }
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
    if (g_headless || FR.fake) return;
    if (!FR.started) FR.cli.set_paths(hub::data_dir(), hub::saves::vault_dir());
    if (!FR.cli.adopt_identity(AC.pending)) {
        set_toast("Could not switch accounts. The current account is unchanged.", false, 8.0);
        return;
    }
    AC.redeem_buf[0] = 0;
    AC.show_code = false;
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
        if (ok) { AC.show_code = true; AC.code = text; AC.code_t0 = ImGui::GetTime(); AC.create_err.clear(); }
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
    if (m == "code" || m == "expired") {
        AC.show_code = true;
        AC.code = "K7M2-QX9P";
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

} // namespace

void draw_account_section() {
    seed_for_screenshots();
    poll_jobs();

    begin_panel("set_account");
    section_title("Account");
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

    /* ---- old PC: make a code ---- */
    settings_label("Add this account to another PC", "Makes a short code. On the other PC choose \"I already have a username on another PC\" in setup (or enter it below in Settings) and type it. "
                                                  "That PC then signs in as you and your cloud saves come with it. The code works once and expires after 10 minutes.");
    bool creating = AC.create_job != nullptr;
    if (button(creating ? "Creating code..." : ((AC.show_code ? "New code" : "Add this account to another PC")), ImVec2(0, 0), false, !creating && !g_headless, ui::Icon::Plus))
        start_create();
    if (!AC.create_err.empty()) account_text_error(AC.create_err);
    if (AC.show_code) {
        double left = kCodeLifetime - (ImGui::GetTime() - AC.code_t0);
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
            if (left > 0) {
                ImGui::PushFont(ui::F.bold);
                ImGui::TextColored(ImColor(left < 60 ? ui::WARN : ui::GOOD), "Expires in %s", countdown_text(left).c_str());
                ImGui::PopFont();
            } else ImGui::TextDisabled("This code has expired. Make a new one.");
            ImGui::EndGroup();
            if (left > 0) {   // countdown bar
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
    bool enter = osk::input("##linkcode", "XXXX-XXXX", AC.redeem_buf, sizeof AC.redeem_buf,
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

    /* ---- confirmation before replacing this PC's account ---- */
    if (AC.confirm_open) { ImGui::OpenPopup("Replace the account on this PC?##acct"); AC.confirm_open = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.5f), sp(2.5f)));
    if (ImGui::BeginPopupModal("Replace the account on this PC?##acct", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (ui::modal_back()) ImGui::CloseCurrentPopup();
        ImGui::PushTextWrapPos(sp(44.f));
        ImGui::Text("Account to sign in with: %s", hubnet::friend_code(AC.pending.user_id).c_str());
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::TextUnformatted("This replaces the account on this PC; the current one is backed up.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        if (button("Replace account", ImVec2(sp(16.f), sp(5.f)), true)) { adopt_pending(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (button("Cancel", ImVec2(sp(12.f), sp(5.f)))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    end_panel();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
}

/* ---------- Setup wizard: "I already have a username on another PC" ----------
 * Draws the code box + button. A good code signs this PC in at once (nothing to lose on a fresh install), turns cloud saves
 * on and starts the first pull. Returns true on the frame the account switched. Server calls run on the worker thread. */
bool account_redeem_for_wizard() {
    seed_for_screenshots();
    AC.auto_adopt = true;
    poll_jobs();
    bool adopted = false;
    if (AC.adopted) { AC.adopted = false; adopted = true; }
    bool redeeming = AC.redeem_job != nullptr;
    ImGui::SetNextItemWidth(sp(26.f));
    bool enter = osk::input("##wizcode", "XXXX-XXXX", AC.redeem_buf, sizeof AC.redeem_buf,
                            ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue, osk::Kind::Code);
    ImGui::SameLine();
    const bool complete = hubui::un::link_code_complete(AC.redeem_buf);
    if (button(redeeming ? "Checking..." : "Sign in", ImVec2(sp(14.f), 0), true, !redeeming && !g_headless && complete) || (enter && complete)) {
        if (!redeeming && complete) start_redeem();
    }
    if (!AC.redeem_err.empty()) account_text_error(AC.redeem_err);
    return adopted;
}
void account_redeem_reset() { AC.redeem_buf[0] = 0; AC.redeem_err.clear(); AC.auto_adopt = false; }
