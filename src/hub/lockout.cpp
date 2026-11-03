#include "app.h"
#include "lockout_logic.h"
#include "friends_ui.h"
#include "ui_kit.h"
#include "winutil.h"

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

std::string cache_path() { return hub::data_dir() + "access.json"; }

const char* kind_name(hublock::Lock l) { return l == hublock::Lock::Pending ? "pending" : l == hublock::Lock::Unavailable ? "unavailable" : l == hublock::Lock::Setup ? "setup" : "none"; }
}

void lockout_init(bool had_account, const std::string& uid) {
    g_enabled = !g_headless && !autotest_active();
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

void lockout_shot(const std::string& mode) {   // HUB_SHOT_ACCESS=pending|unavailable|setup|setup-offline
    g_forced = true;
    g_forced_trying = mode == "setup";
    g_forced_lock = mode == "pending" ? hublock::Lock::Pending : mode == "unavailable" ? hublock::Lock::Unavailable : hublock::Lock::Setup;
}

hublock::Lock lockout_kind() {
    if (g_forced) return g_forced_lock;
    return g_enabled ? V.lock() : hublock::Lock::None;
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
    const float card_h = pending ? sp(15.f) : 0.f, btn_h = sp(6.f);
    const float total = logo + sp(3.f) + title_h + sp(2.f) + msg_h + (pending ? sp(2.f) + card_h : 0.f) + sp(4.f) + btn_h;
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
