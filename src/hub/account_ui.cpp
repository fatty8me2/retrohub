#include "app.h"
#include "identity_link.h"

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
    if (!FR.cli.adopt_identity(AC.pending)) {
        set_toast("Could not switch accounts. The current account is unchanged.", false, 8.0);
        return;
    }
    AC.redeem_buf[0] = 0;
    AC.show_code = false;
    std::remove(code_path().c_str()); AC.code_loaded = false;   // the saved account code belonged to the previous account
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

} // namespace

void draw_account_section() {
    seed_for_screenshots();
    poll_jobs();

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
