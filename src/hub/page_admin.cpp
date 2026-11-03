#include "app.h"
#include "friends_ui.h"
#include "admin_logic.h"
#include "osk.h"

/* Admin page (docs/ADMIN.md): accounts, bans, approvals and the two switches. It exists only for the owner account: the welcome says so
 * (FriendsClient::is_admin) and the sidebar entry, the page and every request are gated on it. That is a convenience only - the Worker
 * checks the account secret on every admin_* message and ignores them from anyone else. */

namespace {

using hubnet::AdminAccount;

struct AdminUi {
    hubnet::AdminQuery q;
    char search[48] = {0};
    std::string sel;                       // the account shown in the detail panel
    bool confirm_open = false;
    std::string confirm_op, confirm_uid, confirm_name;
    int seen_ver = -1;
    double refresh_at = 0;
    bool dirty = true;
    bool shot_scroll = false;
    /* screenshot / fake mode (HUB_SHOT_ADMIN): the same shapes, no network */
    bool fake = false;
    hubnet::AdminList fl;
    hubnet::AdminDetail fd;
    hubnet::AdminSettings fs;
} AU;

const hubnet::AdminList& cur_list() { return AU.fake ? AU.fl : FR.cli.admin_list(); }
const hubnet::AdminDetail& cur_detail() { return AU.fake ? AU.fd : FR.cli.admin_detail(); }
const hubnet::AdminSettings& cur_settings() { return AU.fake ? AU.fs : FR.cli.admin_settings(); }

std::string fmt_date(long long ms) {
    if (ms <= 0) return "unknown";
    std::time_t t = (std::time_t)(ms / 1000);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[40];
    std::strftime(b, sizeof b, "%b %d, %Y", &tmv);
    return b;
}

ImU32 status_col(const AdminAccount& a) { return a.blocked() ? ui::BAD : a.pending() ? ui::WARN : ui::GOOD; }

/* ---- fake mode ---- */
void fake_filter_apply();
std::vector<AdminAccount> g_fake_all;
void fake_filter_apply() {
    AU.fl.rows.clear();
    for (const AdminAccount& a : g_fake_all) {
        if (a.test && !AU.q.show_test) continue;
        if (AU.q.status == "pending" && !a.pending()) continue;
        if (AU.q.status == "banned" && !a.blocked()) continue;
        if (!AU.q.q.empty()) {
            std::string n = a.shown_name(), qq = AU.q.q;
            for (char& c : n) c = (char)std::tolower((unsigned char)c);
            for (char& c : qq) c = (char)std::tolower((unsigned char)c);
            if (n.find(qq) == std::string::npos) continue;
        }
        AU.fl.rows.push_back(a);
    }
    std::stable_sort(AU.fl.rows.begin(), AU.fl.rows.end(), [](const AdminAccount& x, const AdminAccount& y) {
        long long a = AU.q.by_first ? x.first_seen : x.last_seen, b = AU.q.by_first ? y.first_seen : y.last_seen;
        return AU.q.asc ? a < b : a > b;
    });
    int total = (int)AU.fl.rows.size();
    hubnet::AdminCounts c;
    for (const AdminAccount& a : g_fake_all) { ++c.all; if (a.blocked()) ++c.banned; else if (a.pending()) ++c.pending; else ++c.ok; }
    AU.fl.counts = c; AU.fl.total = total; AU.fl.tracked = c.all; AU.fl.settings = AU.fs; AU.fl.valid = true; AU.fl.offset = 0;
}
void fake_detail_for(const std::string& uid) {
    AU.fd = hubnet::AdminDetail();
    for (const AdminAccount& a : g_fake_all) if (a.uid == uid) {
        AU.fd.valid = true; AU.fd.found = true; AU.fd.uid = uid; AU.fd.row = a; AU.fd.code = a.code;
        AU.fd.rel = a.friend_of_you ? "friend" : a.req == "in" ? "requested_you" : "none";
        AU.fd.how = a.friend_of_you ? (a.by == "seed" ? "auto" : "auto") : "";
        AU.fd.how_at = a.first_seen; AU.fd.friend_since = a.first_seen;
        if (a.name == "Mia") { AU.fd.how = "room"; }
        if (!a.blocked()) {
            AU.fd.has_peek = true;
            AU.fd.friends = a.name == "lucas" ? std::vector<std::string>{"RetroHub owner"} : std::vector<std::string>{"RetroHub owner", "Jake", "Kai", "Luna"};
            AU.fd.nfriends = (int)AU.fd.friends.size();
        }
    }
}
void fake_act(const std::string& op, const std::string& uid) {
    for (AdminAccount& a : g_fake_all) if (a.uid == uid) {
        if (op == "ban") { a.status = "banned"; a.friend_of_you = false; }
        else if (op == "deny") { a.status = "denied"; }
        else a.status = "ok";
        push_toast(hubnet::admin_result_text(op, true, a.shown_name(), "", true, true), ui::GOOD, 5.0);
    }
    fake_filter_apply();
    if (!AU.sel.empty()) fake_detail_for(AU.sel);
}

void request_refresh() {
    if (AU.fake) { fake_filter_apply(); if (!AU.sel.empty()) fake_detail_for(AU.sel); return; }
    FR.cli.admin_request_list(AU.q);
    if (!AU.sel.empty()) FR.cli.admin_request_detail(AU.sel);
}
void do_act(const std::string& op, const std::string& uid) {
    if (AU.fake) { fake_act(op, uid); return; }
    FR.cli.admin_act(op, uid);
    AU.refresh_at = ImGui::GetTime() + 0.6;   // the answer (a toast) comes first; the list follows
}
void select(const std::string& uid) {
    if (AU.sel == uid) return;
    AU.sel = uid;
    if (AU.fake) fake_detail_for(uid); else FR.cli.admin_request_detail(uid);
}
void ask_confirm(const char* op, const AdminAccount& a) {
    AU.confirm_op = op; AU.confirm_uid = a.uid; AU.confirm_name = a.shown_name(); AU.confirm_open = true;
}

void badge_pills(const AdminAccount& a) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float x = p.x;
    auto pill = [&](const char* t, ImU32 bg) { x += ui::pill_at(dl, ImVec2(x, p.y), t, bg, IM_COL32_WHITE, ui::F.cap) + sp(0.5f); };
    if (a.test) pill("TEST", ui::PANEL3);
    if (a.unapplied) pill("NOT APPLIED YET", ui::WARN);
    else if (a.unreachable) pill("UNREACHABLE", ui::WARN);   // enforced at their next hello
    if (a.blocked()) pill(a.status == "denied" ? "DENIED" : "BANNED", ui::BAD);
    else if (a.pending()) pill("PENDING", ui::WARN);
    if (a.friend_of_you) pill("FRIEND", ui::ACCENT);
    else if (a.req == "in") pill("ASKED YOU", ui::mix(ui::PANEL3, ui::ACCENT, 0.5f));
}

void action_buttons(const AdminAccount& a, float bh, float bw) {
    if (a.pending()) {
        if (ui::button("Approve", ImVec2(bw, bh), true)) do_act("approve", a.uid);
        ImGui::SameLine();
        if (ui::button("Deny", ImVec2(bw, bh))) ask_confirm("deny", a);
    } else if (a.blocked()) {
        if (ui::button("Unban", ImVec2(bw, bh), true)) ask_confirm("unban", a);
    } else {
        if (ui::button("Ban", ImVec2(bw, bh))) ask_confirm("ban", a);
    }
}
float action_width(const AdminAccount& a, float bw) { return a.pending() ? bw * 2 + sp(0.75f) : bw; }

void settings_panel(float w) {
    const hubnet::AdminSettings& s = cur_settings();
    begin_panel("adm_settings", ImVec2(w, 0));
    section_title("Who can join");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    bool ap = s.approval, vo = s.vouch, af = s.autofriend;
    if (ImGui::Checkbox("Approve new accounts before they can go online", &ap)) {
        if (AU.fake) { AU.fs.approval = ap; fake_filter_apply(); } else FR.cli.admin_set_settings(ap, -1, -1);
        AU.refresh_at = ImGui::GetTime() + 0.5;
    }
    text_wrapped_disabled(hubnet::admin_approval_help(s.approval));
    ImGui::Dummy(ImVec2(0, sp(0.75f)));
    ImGui::BeginDisabled(!s.approval);
    if (ImGui::Checkbox("A friend of yours can vouch for a new account", &vo)) {
        if (AU.fake) AU.fs.vouch = vo; else FR.cli.admin_set_settings(-1, vo, -1);
        AU.refresh_at = ImGui::GetTime() + 0.5;
    }
    ImGui::EndDisabled();
    text_wrapped_disabled(hubnet::admin_vouch_help());
    ImGui::Dummy(ImVec2(0, sp(0.75f)));
    if (ImGui::Checkbox("Add every new account to my friends automatically", &af)) {
        if (AU.fake) AU.fs.autofriend = af; else FR.cli.admin_set_settings(-1, -1, af);
        AU.refresh_at = ImGui::GetTime() + 0.5;
    }
    text_wrapped_disabled(hubnet::admin_autofriend_help());
    if (AU.fake) AU.fl.settings = AU.fs;
    end_panel();
}

void account_row(const AdminAccount& a, float w, long long now) {
    ImGui::PushID(a.uid.c_str());
    float rh = sp(7.f), bw = sp(9.f), bh = sp(3.5f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool sel = AU.sel == a.uid;
    if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_AllowOverlap, ImVec2(w, rh))) select(a.uid);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (sel) dl->AddRect(p, p + ImVec2(w, rh), ui::with_alpha(ui::ACCENT, 0.7f), sp(1.f), 0, 1.5f);
    ImVec2 c(p.x + sp(2.5f), p.y + rh * 0.5f);
    std::string nm = a.shown_name();
    dl->AddCircleFilled(c, sp(1.9f), fr_avatar_col(nm), 24);
    char init[8] = {hubnet::username_initial(nm), 0};
    fr_text_c(dl, ui::F.bold, c, IM_COL32_WHITE, init);
    float aw = action_width(a, bw);
    float tx = p.x + sp(5.5f), tw = w - sp(5.5f) - aw - sp(1.5f);
    dl->PushClipRect(ImVec2(tx, p.y), ImVec2(tx + tw, p.y + rh), true);
    dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(tx, p.y + sp(0.9f)), ui::TEXT, nm.c_str());
    ImVec2 nsz = ui::F.bold->CalcTextSizeA(ui::F.bold->FontSize, FLT_MAX, 0.f, nm.c_str());
    ImGui::SetCursorScreenPos(ImVec2(tx + nsz.x + sp(1.f), p.y + sp(1.0f)));
    badge_pills(a);
    std::string sub = "First seen " + hubnet::admin_ago(now, a.first_seen) + "   -   Last seen " + hubnet::admin_ago(now, a.last_seen);
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p.y + sp(3.9f)), ui::TEXT2, sub.c_str());
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - aw - sp(1.f), p.y + (rh - bh) * 0.5f));
    action_buttons(a, bh, bw);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
    dl->AddLine(ImVec2(p.x + sp(1.f), p.y + rh), ImVec2(p.x + w - sp(1.f), p.y + rh), ui::HAIRLINE);
    ImGui::Dummy(ImVec2(0, sp(0.25f)));
    ImGui::PopID();
}

void detail_panel(float w, long long now) {
    const hubnet::AdminDetail& d = cur_detail();
    begin_panel("adm_detail", ImVec2(w, 0));
    if (AU.sel.empty() || !d.valid || d.uid != AU.sel) {
        section_title("Account details");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        text_wrapped_disabled(AU.sel.empty() ? "Pick an account to see how it got onto your list and who its friends are." : "Loading...");
        end_panel();
        return;
    }
    const AdminAccount& a = d.row;
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted(ui::ellipsize(ui::F.h2, ui::F.h2->FontSize, a.shown_name(), w - sp(5.f)).c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, sp(0.25f)));
    badge_pills(a);
    ImGui::Dummy(ImVec2(0, sp(2.25f)));
    auto kv = [&](const char* k, const std::string& v) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::TextUnformatted(k);
        ImGui::PopStyleColor();
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted(v.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
    };
    kv("Friend code", d.code.empty() ? a.code : d.code);
    kv("Status", hubnet::admin_status_label(a) + (a.by == "admin" ? " (set by you)" : a.by == "grandfathered" ? " (was here before approval mode)" : a.by.rfind("vouch:", 0) == 0 ? " (vouched for by a friend)" : ""));
    kv("First seen", fmt_date(a.first_seen) + "  (" + hubnet::admin_ago(now, a.first_seen) + ")");
    kv("Last seen", a.last_seen > 0 ? fmt_date(a.last_seen) + "  (" + hubnet::admin_ago(now, a.last_seen) + ")" : std::string("not since tracking began"));
    if (a.pcs > 0) kv("PCs (approx.)", std::to_string(a.pcs));
    kv("How they got onto your list", hubnet::admin_how_text(d.how, d.rel) + (d.how_at > 0 && d.rel == "friend" ? " - " + fmt_date(d.how_at) : std::string()));
    if (d.has_peek) {
        std::string fl;
        for (size_t i = 0; i < d.friends.size(); ++i) { if (i) fl += ", "; fl += d.friends[i]; }
        if (d.nfriends > (int)d.friends.size()) fl += ", ...";
        kv(("Their friends (" + std::to_string(d.nfriends) + ")").c_str(), fl.empty() ? std::string("none") : fl);
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::PushID("detailact");
    action_buttons(a, sp(4.f), sp(10.f));
    ImGui::PopID();
    end_panel();
}

void confirm_modal() {
    if (AU.confirm_open) { ImGui::OpenPopup("Are you sure?##admconf"); AU.confirm_open = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.5f), sp(2.5f)));
    if (ImGui::BeginPopupModal("Are you sure?##admconf", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (ui::modal_back()) ImGui::CloseCurrentPopup();
        const bool ban = AU.confirm_op == "ban", deny = AU.confirm_op == "deny";
        std::string msg = ban ? hubnet::admin_ban_confirm(AU.confirm_name) : deny ? hubnet::admin_deny_confirm(AU.confirm_name)
                                                                                 : "Unban " + AU.confirm_name + "? They can use online features again. They are not added back to your friends.";
        ImGui::PushTextWrapPos(sp(40.f));
        ImGui::TextUnformatted(msg.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
        const char* go = ban ? "Ban" : deny ? "Deny" : "Unban";
        if (ui::button(go, ImVec2(sp(14.f), sp(5.f)), !ban && !deny)) { do_act(AU.confirm_op, AU.confirm_uid); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ui::button("Cancel", ImVec2(sp(14.f), sp(5.f)))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

} // namespace

/* ---- called from the friends event loop / shot aids ---- */
void admin_ui_poke() { AU.refresh_at = 0; AU.dirty = true; }

void admin_ui_shot_init(const char* mode) {
    AU.fake = true;
    long long now = wall_ms();
    const long long H = 3600 * 1000LL, D = 24 * H;
    auto mk = [&](const char* uid, const char* name, const char* status, const char* by, bool fr, long long first, long long last, int pcs) {
        AdminAccount a; a.uid = uid; a.name = name; a.status = status; a.by = by; a.friend_of_you = fr;
        a.created = first; a.first_seen = first; a.last_seen = last; a.pcs = pcs; a.code = std::string("RH-7KQ2-M") + (char)(uid[0] - 32) + "XD";
        g_fake_all.push_back(a);
    };
    mk("lucasuid", "lucas", "ok", "auto", true, now - 3 * D, now - 3 * H, 1);
    mk("adminuid", "Admin", "ok", "auto", true, now - 9 * D, now - 26 * H, 1);
    mk("newguy77", "newguy77", "pending", "", false, now - 40 * 60000, now - 5 * 60000, 1);
    mk("xxprouid", "xX_pro_Xx", "pending", "", false, now - 2 * H, now - 90 * 60000, 2);
    mk("miauid", "Mia", "ok", "grandfathered", true, now - 61 * D, now - 4 * H, 2);
    mk("jakuid", "Jake", "ok", "grandfathered", true, now - 80 * D, now - 12 * H, 1);
    mk("kaiuid", "Kai", "ok", "grandfathered", true, now - 95 * D, now - 2 * D, 1);
    mk("lunuid", "Luna", "ok", "grandfathered", true, now - 120 * D, now - 20 * D, 3);
    mk("spamuid", "spam4free", "banned", "admin", false, now - 5 * D, now - 4 * D, 1);
    mk("samuid", "Sam", "ok", "grandfathered", true, now - 150 * D, now - 3 * D, 1);
    mk("qabot01", "qa-bot", "ok", "auto", false, now - 6 * D, now - 2 * H, 1);
    g_fake_all.back().test = true;
    for (AdminAccount& a : g_fake_all) if (a.uid == "spamuid") a.unapplied = true;   // shows the "not applied yet - retrying" badge
    AU.fs.approval = true; AU.fs.vouch = false; AU.fs.autofriend = true; AU.fs.cutoff = now - 4 * D;
    std::string m = mode ? mode : "";
    if (m == "pending") AU.q.status = "pending";
    if (m == "banned") AU.q.status = "banned";
    if (m == "detail" || m == "confirm") { AU.sel = "lucasuid"; }
    fake_filter_apply();
    if (!AU.sel.empty()) fake_detail_for(AU.sel);
    if (m == "confirm") { AU.confirm_op = "ban"; AU.confirm_uid = "lucasuid"; AU.confirm_name = "lucas"; AU.confirm_open = true; }
    if (m == "settings") { AU.fs.vouch = true; AU.shot_scroll = true; }
    AU.dirty = false;
    AU.refresh_at = 1e18;
    A.page = Page::Admin;
}

void page_admin() {
    if (!fr_is_admin()) { A.page = Page::Library; return; }   // never shown to anyone else
    page_header("Admin", "Accounts, bans and approvals. Only you can see this page.");
    const double t = ImGui::GetTime();
    static double last_drawn = -100;
    if (t - last_drawn > 1.0) AU.dirty = true;   // just opened (or came back): fetch fresh data
    last_drawn = t;
    if (!AU.fake) {
        if (AU.dirty || t >= AU.refresh_at) { request_refresh(); AU.dirty = false; AU.refresh_at = t + 30.0; }
        if (AU.seen_ver != FR.cli.admin_version()) AU.seen_ver = FR.cli.admin_version();
    }
    const hubnet::AdminList& L = cur_list();
    const long long now = wall_ms();
    float avail = ImGui::GetContentRegionAvail().x;
    float gap = sp(2.f);
    bool two = avail > sp(100.f);

    if (L.counts.pending > 0) {
        begin_panel("adm_banner", ImVec2(avail, 0), ui::mix(ui::PANEL, ui::WARN, 0.18f));
        std::string t1 = std::to_string(L.counts.pending) + (L.counts.pending == 1 ? " account is" : " accounts are") + " waiting for your approval.";
        ImGui::TextUnformatted(t1.c_str());
        ImGui::SameLine();
        if (ui::button("Review", ImVec2(sp(10.f), sp(3.5f)), true)) { AU.q.status = "pending"; AU.q.offset = 0; AU.dirty = true; if (AU.fake) fake_filter_apply(); }
        end_panel();
        ImGui::Dummy(ImVec2(0, sp(1.f)));
    }
    /* accounts */
    float lw = two ? avail * 0.6f - gap * 0.5f : avail;
    float rw = two ? avail - lw - gap : avail;
    float top_y = ImGui::GetCursorPosY();
    begin_panel("adm_accounts", ImVec2(lw, 0));
    section_title("Accounts");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    {   // search + sort
        ImGui::SetNextItemWidth(lw - sp(5.f) - sp(34.f));
        if (osk::input("##adm_search", "Search by name or code", AU.search, sizeof AU.search, 0, osk::Kind::Text)) { /* Enter */ }
        if (AU.q.q != AU.search) { AU.q.q = AU.search; AU.q.offset = 0; AU.refresh_at = t + 0.35; if (AU.fake) fake_filter_apply(); }
        ImGui::SameLine();
        static const char* sorts[] = {"Last seen", "First seen"};
        int sv = ui::segmented("##adm_sort", sorts, 2, AU.q.by_first ? 1 : 0);
        if ((sv == 1) != AU.q.by_first) { AU.q.by_first = sv == 1; AU.q.offset = 0; AU.dirty = true; if (AU.fake) fake_filter_apply(); }
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    {   // filters + order
        std::string all = "All (" + std::to_string(L.counts.all) + ")", pen = "Pending (" + std::to_string(L.counts.pending) + ")", ban = "Banned (" + std::to_string(L.counts.banned) + ")";
        const std::pair<const char*, std::string> fl[] = {{"all", all}, {"pending", pen}, {"banned", ban}};
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            ImU32 col = std::string(fl[i].first) == "pending" && L.counts.pending > 0 ? ui::WARN : std::string(fl[i].first) == "banned" ? ui::BAD : ui::ACCENT;
            if (ui::chip(fl[i].second.c_str(), AU.q.status == fl[i].first, col)) { AU.q.status = fl[i].first; AU.q.offset = 0; AU.dirty = true; if (AU.fake) fake_filter_apply(); }
        }
        ImGui::SameLine();
        if (ui::chip(AU.q.asc ? "Oldest first" : "Newest first", false, ui::ACCENT)) { AU.q.asc = !AU.q.asc; AU.dirty = true; if (AU.fake) fake_filter_apply(); }
        ImGui::SameLine();
        if (ui::chip("Show test accounts", AU.q.show_test, ui::ACCENT)) { AU.q.show_test = !AU.q.show_test; AU.q.offset = 0; AU.dirty = true; if (AU.fake) fake_filter_apply(); }
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    float rw_in = ImGui::GetContentRegionAvail().x;
    if (!L.valid) text_wrapped_disabled("Loading accounts...");
    else if (L.rows.empty()) text_wrapped_disabled(AU.q.q.empty() && AU.q.status == "all" ? "No accounts yet." : "Nothing matches.");
    for (const AdminAccount& a : L.rows) account_row(a, rw_in, now);
    if (L.valid && (L.total > (int)L.rows.size() + L.offset || L.offset > 0)) {   // paging
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (ui::button("Previous", ImVec2(sp(11.f), sp(4.f)), false, L.offset > 0)) { AU.q.offset = std::max(0, L.offset - AU.q.limit); AU.dirty = true; }
        ImGui::SameLine();
        if (ui::button("Next", ImVec2(sp(11.f), sp(4.f)), false, L.offset + (int)L.rows.size() < L.total)) { AU.q.offset = L.offset + AU.q.limit; AU.dirty = true; }
        ImGui::SameLine();
        ImGui::Text("%d-%d of %d", L.offset + 1, L.offset + (int)L.rows.size(), L.total);
    }
    end_panel();
    if (two) { ImGui::SameLine(0, gap); } else ImGui::Dummy(ImVec2(0, sp(1.f)));
    (void)top_y;
    detail_panel(rw, now);
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (AU.shot_scroll) ImGui::SetScrollHereY(0.f);   // HUB_SHOT_ADMIN=settings: bring the switches into view
    settings_panel(avail);
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    confirm_modal();
}
