#include "app.h"
#include "lockout_logic.h"
#include "friends_ui.h"
#include "ui_kit.h"
#include "winutil.h"
#include "invite_logic.h"
#include "osk.h"
#include "imgui_internal.h"

/* Whole-app lock (docs/ADMIN.md "Locking the whole app"). The verdict state machine is lockout_logic.h; this file feeds it from the friends
 * client, remembers it in <data dir>\access.json, draws the lock screen and answers launch_blocked() for every way a game can start.
 * A modified client can skip all of it: the server is the real gate. */

namespace {
hublock::Verdict V;
bool g_enabled = false;                       // off for headless runs, the QA room driver and fake-friends screenshots
bool g_forced = false;                        // HUB_SHOT_ACCESS / HUB_SHOT_LOCK: a fixed lock for screenshots
hublock::Lock g_forced_lock = hublock::Lock::None;
bool g_forced_trying = false;
double g_t0 = 0.0;                            // when the app started (a new install shows "Setting up" for a few seconds before asking for internet)
bool g_first_draw = true;
bool g_signin_open = false;                   // "Already have an account?" panel (pending / denied): sign in with the account code from another PC

/* The waiting screen's "Have an invite code?" field (net/invite_logic.h). Only on the Pending lock; the server approves the account on a good code. */
char g_inv_buf[24] = {0};
std::string g_inv_msg;
int g_inv_kind = 0;                           // 0 nothing, 1 working, 2 good, 3 problem
bool g_inv_busy = false;
double g_inv_sent = 0.0, g_inv_last = 0.0;
std::string g_inv_link;                       // a retrohub://invite/ code waiting for the lock screen to be ready
double g_inv_link_at = 0.0;
double now_s() { return (double)GetTickCount64() / 1000.0; }
void inv_set(int kind, const std::string& m) { g_inv_kind = kind; g_inv_msg = m; }
void inv_submit() {
    if (g_inv_busy) return;
    const std::string c = hubnet::invite_clean(g_inv_buf);
    if (!hubnet::invite_valid(c)) { inv_set(3, hubnet::redeem_local_problem()); return; }
    if (now_s() - g_inv_last < 1.5) return;
    g_inv_last = now_s();
    if (g_forced) return;
    if (FR.cli.redeem_invite(c)) { g_inv_busy = true; g_inv_sent = now_s(); inv_set(1, "Checking the code..."); }
    else { inv_set(3, "Can't reach the server yet. Wait a moment and try again."); FR.cli.reconnect_now(); }
}

std::string cache_path() { return hub::data_dir() + "access.json"; }

const char* kind_name(hublock::Lock l) { return l == hublock::Lock::Pending ? "pending" : l == hublock::Lock::Unavailable ? "unavailable" : l == hublock::Lock::Setup ? "setup" : "none"; }
}

void lockout_init(bool had_account, const std::string& uid) {
    g_enabled = !g_headless;   // autotest only skips the "Setting up" lock (fresh QA data dirs); a pending / banned verdict always locks
    g_t0 = (double)GetTickCount64() / 1000.0;
    hublock::Cache c;
    std::string text;
    if (winutil::read_file(cache_path(), &text)) c = hublock::cache_parse(text);
    V.init(c, had_account, uid);
    if (g_enabled && V.take_dirty(&c)) {   // a brand-new install: remember "not yet confirmed" right away, so a restart or going offline cannot unlock it
        c.uid = uid;
        winutil::write_file(cache_path(), hublock::cache_json(c));
    }
}

void lockout_invite_link(const std::string& code) {
    if (!hubnet::invite_valid(code)) return;
    g_inv_link = hubnet::invite_format(code);
    g_inv_link_at = now_s();
}
void lockout_invite_result(bool ok, const std::string& reason) {
    g_inv_busy = false;
    inv_set(ok ? 2 : 3, hubnet::redeem_result_text(ok, reason));
}

void lockout_shot(const std::string& mode) {   // HUB_SHOT_ACCESS=pending|pending-code|pending-bad|unavailable|setup|setup-offline
    if (mode.size() > 7 && mode.compare(mode.size() - 7, 7, "-signin") == 0) g_signin_open = true;
    if (mode == "pending-code" || mode == "pending-bad") {
        std::snprintf(g_inv_buf, sizeof g_inv_buf, "%s", "K7QM-2XDT");
        if (mode == "pending-bad") inv_set(3, hubnet::redeem_result_text(false, "expired"));
    }
    g_forced = true;
    g_forced_trying = mode == "setup";
    g_forced_lock = mode.rfind("pending", 0) == 0 ? hublock::Lock::Pending : mode.rfind("unavailable", 0) == 0 ? hublock::Lock::Unavailable : hublock::Lock::Setup;
}

hublock::Lock lockout_kind() {
    if (g_forced) return g_forced_lock;
    if (!g_enabled) return hublock::Lock::None;
    const hublock::Lock l = V.lock();
    return l == hublock::Lock::Setup && autotest_active() ? hublock::Lock::None : l;
}
bool app_locked() { return lockout_kind() != hublock::Lock::None; }

bool launch_blocked(const char* what) {
    if (!app_locked()) return false;
    static double last = 0;
    const double now = (double)GetTickCount64() / 1000.0;
    if (now - last > 5.0) { last = now; std::fprintf(stderr, "RetroHub: %s refused: the account is locked (%s)\n", what ? what : "launch", kind_name(lockout_kind())); }
    return true;
}

void lockout_tick() {
    if (!g_enabled || g_forced) return;
    if (g_inv_busy && now_s() - g_inv_sent > 12.0) { g_inv_busy = false; inv_set(3, "No answer from the server. Try again."); }
    if (!g_inv_link.empty()) {   // an invite link: wait until we know whether this account is waiting
        const hublock::Lock l = lockout_kind();
        if (l == hublock::Lock::Pending) {
            std::snprintf(g_inv_buf, sizeof g_inv_buf, "%s", g_inv_link.c_str());
            g_inv_link.clear();
            inv_submit();
        } else if ((l == hublock::Lock::None && FR.cli.connected()) || now_s() - g_inv_link_at > 60.0) g_inv_link.clear();   // already approved (or nothing to do): the link has no use
    }
    if (const int gate = hub::saves::cloud_gate_take()) V.on_live(gate == 1 ? hublock::Access::Pending : hublock::Access::Unavailable, false, false);   // a /saves 403 reason
    if (FR.started && !FR.fake) {
        const std::string uid = FR.cli.my_uid();
        static std::string last_uid;
        if (!last_uid.empty() && uid != last_uid) V.reset_account(uid);   // the account was replaced (identity adopted from another PC / regenerated): a verdict for the old one means nothing; a new install stays one
        last_uid = uid;
        V.on_live(FR.cli.access(), FR.cli.connected(), FR.cli.is_admin());
        hublock::Cache c;
        if (V.take_dirty(&c)) {
            c.uid = FR.cli.my_uid();
            winutil::write_file(cache_path(), hublock::cache_json(c));
        }
    }
    {   // the lock just came on: cancel what was armed before it (a guest's pending launch, a 3DS / GameCube room being prepared) unless a game is running
        static bool was = false;
        const bool now = app_locked();
        if (now && !was && !A.proc && !session_active()) online_cancel_pending();
        was = now;
    }
    /* Once the game is over and the lock is on: leave any room quietly. Never while a game runs (that would end it). */
    if (app_locked() && !A.proc && !session_active() && A.in_room) leave_room();
}

/* The full-window lock screen. It replaces every page; only the quick menu of a running game is still drawn by the caller. */
void draw_lock_screen() {
    const hublock::Lock lk = lockout_kind();
    ImGuiIO& io = ImGui::GetIO();
    {   // the lock just came on (or came back): drop any modal that was open behind it, and put the pad on the first button
        static int last_frame = -10;
        if (ImGui::GetFrameCount() != last_frame + 1) {
            if (ImGui::GetCurrentContext()->OpenPopupStack.Size > 0) ImGui::ClosePopupToLevel(0, false);
            g_first_draw = true;
        }
        last_frame = ImGui::GetFrameCount();
    }
    const bool trying = g_forced ? g_forced_trying : ((double)GetTickCount64() / 1000.0 - g_t0 < 8.0) && !FR.cli.connected();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::BG);
    ImGui::Begin("##lock", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cw = std::min(sp(64.f), io.DisplaySize.x - sp(8.f));
    const float logo = sp(14.f);
    const bool pending = lk == hublock::Lock::Pending;
    const bool setup = lk == hublock::Lock::Setup;
    const std::string title = hublock::lock_title(lk, trying);
    const std::string msg = hublock::lock_message(lk, trying);
    std::string name = A.st.name, code = FR.fake || g_forced ? std::string("RH-7KQ2-M9XD") : fr_my_code();
    if (g_forced) name = "Mia";   // screenshots never show the real PC user name

    /* Content height, to centre it: logo, title, message, account card (pending only), buttons. */
    ImFont* hf = ui::F.title;
    const float title_h = hf->FontSize * 1.2f, msg_h = msg.empty() ? 0.f : ui::F.body->FontSize * (msg.size() > 90 && cw < sp(70.f) ? 4.f : 3.f);
    const bool can_signin = pending || lk == hublock::Lock::Unavailable;
    const float card_h = pending ? sp(15.f) : 0.f, btn_h = sp(6.f), inv_h = !can_signin ? 0.f : g_signin_open ? sp(29.f) : pending ? sp(22.f) : sp(8.f);
    const float total = logo + sp(3.f) + title_h + sp(2.f) + msg_h + (pending ? sp(2.f) + card_h : 0.f) + inv_h + (can_signin && !pending ? sp(2.f) : 0.f) + sp(4.f) + btn_h;
    const float x0 = (io.DisplaySize.x - cw) * 0.5f, y0 = std::max(sp(3.f), (io.DisplaySize.y - total) * 0.5f);

    ui_draw_brand_logo(dl, ImVec2((io.DisplaySize.x - logo) * 0.5f, y0), logo);
    float y = y0 + logo + sp(3.f);
    {   // title, centred (wraps only on a very narrow window)
        const float tw = std::min(sp(95.f), io.DisplaySize.x - sp(6.f));
        std::vector<std::string> tl = ui::wrap_lines(hf, hf->FontSize, title, tw, 3);
        for (auto& l : tl) {
            ImVec2 ts = hf->CalcTextSizeA(hf->FontSize, FLT_MAX, 0.f, l.c_str());
            dl->AddText(hf, hf->FontSize, ImVec2((io.DisplaySize.x - ts.x) * 0.5f, y), ui::TEXT, l.c_str());
            y += hf->FontSize * 1.2f;
        }
        y += sp(2.f) - (hf->FontSize * 1.2f - title_h);
    }
    if (!msg.empty()) {
        std::vector<std::string> lines = ui::wrap_lines(ui::F.body, ui::F.body->FontSize, msg, cw, 6);
        for (auto& l : lines) {
            ImVec2 ls = ui::F.body->CalcTextSizeA(ui::F.body->FontSize, FLT_MAX, 0.f, l.c_str());
            dl->AddText(ui::F.body, ui::F.body->FontSize, ImVec2((io.DisplaySize.x - ls.x) * 0.5f, y), ui::TEXT2, l.c_str());
            y += ui::F.body->FontSize * 1.35f;
        }
        y += sp(2.f);
    }
    if (pending) {   // who the owner is looking for
        const float w = std::min(cw, sp(48.f));
        ImVec2 p0((io.DisplaySize.x - w) * 0.5f, y), p1(p0.x + w, y + card_h);
        dl->AddRectFilled(p0, p1, ui::mix(ui::PANEL, ui::WARN, 0.12f), sp(1.5f));
        dl->AddRect(p0, p1, ui::HAIRLINE, sp(1.5f), 0, 1.f);
        const float lx = p0.x + sp(3.f), vx = p0.x + sp(15.f);
        float ry = p0.y + sp(2.5f);
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(lx, ry + 2), ui::TEXT3, "Name");
        dl->AddText(ui::F.h2, ui::F.h2->FontSize, ImVec2(vx, ry), ui::TEXT, ui::ellipsize(ui::F.h2, ui::F.h2->FontSize, name.empty() ? "?" : name, p1.x - vx - sp(2.f)).c_str());
        ry += sp(5.5f);
        dl->AddLine(ImVec2(lx, ry - sp(1.f)), ImVec2(p1.x - sp(3.f), ry - sp(1.f)), ui::HAIRLINE, 1.f);
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(lx, ry + 2), ui::TEXT3, "Friend code");
        dl->AddText(ui::F.h2, ui::F.h2->FontSize, ImVec2(vx, ry), ui::TEXT, code.empty() ? "..." : code.c_str());
        y = p1.y + sp(2.f);
        if (!g_signin_open) {   // "Have an invite code?"
            const char* hd = "Have an invite code?";
            ImVec2 hs = ui::F.bold->CalcTextSizeA(ui::F.bold->FontSize, FLT_MAX, 0.f, hd);
            dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2((io.DisplaySize.x - hs.x) * 0.5f, y + sp(1.f)), ui::TEXT, hd);
            const float iw = std::min(sp(24.f), cw * 0.5f), ubw = sp(14.f), ig = sp(1.5f), rh = sp(5.5f);
            const float rx = (io.DisplaySize.x - (iw + ig + ubw)) * 0.5f, ry = y + sp(4.5f);
            ImGui::SetCursorScreenPos(ImVec2(rx, ry));
            ImGui::SetNextItemWidth(iw);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(sp(1.5f), (rh - ImGui::GetFontSize()) * 0.5f));
            const bool sub = osk::input("##invcode", "ABCD-EFGH", g_inv_buf, sizeof g_inv_buf, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue, osk::Kind::Code);
            ImGui::PopStyleVar();
            {   // keep the field as ABCD-EFGH while typing / pasting (a pasted link or message is cut down to its code)
                const std::string f = hubnet::invite_field_text(g_inv_buf);
                if (f != g_inv_buf) {
                    std::snprintf(g_inv_buf, sizeof g_inv_buf, "%s", f.c_str());
                    if (ImGuiInputTextState* st = ImGui::GetInputTextState(ImGui::GetID("##invcode"))) st->ReloadUserBufAndMoveToEnd();
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(rx + iw + ig, ry));
            const bool have = hubnet::invite_clean(g_inv_buf).size() > 0;
            if (button(g_inv_busy ? "Checking..." : "Use code", ImVec2(ubw, rh), have && !g_inv_busy, have && !g_inv_busy) || (sub && have)) inv_submit();
            if (!g_inv_msg.empty()) {
                const ImU32 mc = g_inv_kind == 2 ? ui::GOOD : g_inv_kind == 3 ? ui::BAD : ui::TEXT2;
                std::vector<std::string> ml = ui::wrap_lines(ui::F.body, ui::F.body->FontSize, g_inv_msg, std::min(cw, sp(48.f)), 2);
                float my = ry + rh + sp(1.f);
                for (auto& l : ml) {
                    ImVec2 ls = ui::F.body->CalcTextSizeA(ui::F.body->FontSize, FLT_MAX, 0.f, l.c_str());
                    dl->AddText(ui::F.body, ui::F.body->FontSize, ImVec2((io.DisplaySize.x - ls.x) * 0.5f, my), mc, l.c_str());
                    my += ui::F.body->FontSize * 1.3f;
                }
            }
        }
        if (!g_signin_open) {
            ImGui::SetCursorScreenPos(ImVec2((io.DisplaySize.x - sp(36.f)) * 0.5f, y + sp(15.5f)));
            if (button("Already have an account? Sign in", ImVec2(sp(36.f), sp(5.f)), true, false)) { g_signin_open = true; account_redeem_reset(); }
        }
        y += inv_h;
    }
    if (can_signin && (g_signin_open || !pending)) {
        if (!pending && !g_signin_open) {
            ImGui::SetCursorScreenPos(ImVec2((io.DisplaySize.x - sp(36.f)) * 0.5f, y + sp(1.f)));
            if (button("Already have an account? Sign in", ImVec2(sp(36.f), sp(5.f)), true, false)) { g_signin_open = true; account_redeem_reset(); }
            y += inv_h;
        } else {
            if (pending) y -= inv_h;   // the sign-in panel takes the invite box's place
            const char* hd = "Sign in to your account";
            ImVec2 hs = ui::F.bold->CalcTextSizeA(ui::F.bold->FontSize, FLT_MAX, 0.f, hd);
            dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2((io.DisplaySize.x - hs.x) * 0.5f, y + sp(1.f)), ui::TEXT, hd);
            const char* hint = "On your other PC, open Settings > Account and make an account code. Type it here.";
            std::vector<std::string> hl = ui::wrap_lines(ui::F.body, ui::F.body->FontSize, hint, std::min(cw, sp(56.f)), 3);
            float hy = y + sp(4.f);
            for (auto& l : hl) {
                ImVec2 ls = ui::F.body->CalcTextSizeA(ui::F.body->FontSize, FLT_MAX, 0.f, l.c_str());
                dl->AddText(ui::F.body, ui::F.body->FontSize, ImVec2((io.DisplaySize.x - ls.x) * 0.5f, hy), ui::TEXT2, l.c_str());
                hy += ui::F.body->FontSize * 1.3f;
            }
            ImGui::SetCursorScreenPos(ImVec2((io.DisplaySize.x - sp(42.f)) * 0.5f, hy + sp(1.f)));
            ImGui::BeginGroup();
            account_redeem_for_wizard();   // the same code box + sign-in the setup wizard uses; adopting reconnects and the lock lifts once approved
            ImGui::EndGroup();
            ImGui::SetCursorScreenPos(ImVec2((io.DisplaySize.x - sp(14.f)) * 0.5f, hy + sp(8.f)));
            if (button("Back", ImVec2(sp(14.f), sp(5.f)), true, false)) { g_signin_open = false; account_redeem_reset(); }
            y += inv_h;
        }
    }
    y += sp(2.f);

    /* Buttons: real widgets so a pad can reach them. */
    const bool can_retry = !(setup && trying);   // a banned account can ask again too: an unban unlocks without a restart
    const float bw = sp(22.f), gap = sp(3.f);
    const float nb = can_retry ? 2.f : 1.f;
    ImGui::SetCursorScreenPos(ImVec2((io.DisplaySize.x - (bw * nb + gap * (nb - 1.f))) * 0.5f, y));
    ImGui::BeginGroup();
    if (g_first_draw) { ImGui::SetKeyboardFocusHere(); g_first_draw = false; }
    if (can_retry) {
        if (button("Check again", ImVec2(bw, btn_h), true, true, ui::Icon::Refresh)) {
            if (!g_forced) FR.cli.reconnect_now();
        }
        ImGui::SameLine(0, gap);
    }
    if (button("Quit", ImVec2(bw, btn_h), !can_retry, true)) {
        SDL_Event q;
        SDL_zero(q);
        q.type = SDL_QUIT;
        SDL_PushEvent(&q);
    }
    ImGui::EndGroup();
    ImGui::End();
}
