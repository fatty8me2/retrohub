/* hubadmin_test: owner dashboard + account access rules (pure logic, no network). */
#include "admin_logic.h"
#include "ident_env.h"
#include "invite_logic.h"

#include <cstdio>

using namespace hubnet;

static int g_fail = 0;
#define CHECK(c) do { if (c) printf("  ok   %s\n", #c); else { g_fail++; printf("  FAIL %s\n", #c); } } while (0)

int main()
{
    // access reasons from the server's refusal
    CHECK(access_from_reason("pending-approval") == Access::Pending);
    CHECK(access_from_reason("account-unavailable") == Access::Unavailable);
    CHECK(access_from_reason("server-busy") == Access::Ok && access_from_reason("") == Access::Ok && access_from_reason("registration-limited") == Access::Ok);
    CHECK(access_message(Access::Pending).find("Waiting for the owner to approve this account") == 0);
    CHECK(access_message(Access::Unavailable).find("ban") == std::string::npos);   // the blocked message is deliberately vague
    CHECK(access_message(Access::Pending).find("offline") == std::string::npos && access_message(Access::Unavailable).find("offline") == std::string::npos);   // the whole app locks: no "keep playing offline"
    CHECK(admin_approval_help(true).find("not even offline") != std::string::npos && admin_ban_confirm("x").find("Offline play is not touched") == std::string::npos);
    CHECK(access_message(Access::Ok).empty() && access_title(Access::Ok).empty());

    // account proof for game-room sockets (strict rooms): env value bound to one server, proof only, malformed values refused
    {
        const std::string uid(26, 'a'), sec(52, 'b');
        const std::string v = ident_env_value("wss://rh.example/", uid, sec);
        const std::string f = ident_frame_from_env("WSS://rh.example", v.c_str());   // trailing slash / case do not matter
        CHECK(f == "{\"type\":\"ident\",\"uid\":\"" + uid + "\",\"secret\":\"" + sec + "\",\"nopair\":true}");
        CHECK(ident_frame_from_env("wss://evil.example", v.c_str()).empty());   // never to another server
        CHECK(ident_frame_from_env("wss://rh.example", nullptr).empty() && ident_frame_from_env("wss://rh.example", "").empty());
        CHECK(ident_frame_from_env("wss://rh.example", "wss://rh.example|short|short").empty());
        CHECK(ident_frame_from_env("wss://rh.example", ("wss://rh.example|" + uid + "\"x|" + sec).c_str()).empty());
        set_process_ident("wss://rh.example", uid, sec);
        CHECK(process_ident_frame("wss://rh.example") == f && process_ident_frame("wss://other").empty());
        set_process_ident("", "", "");
        CHECK(process_ident_frame("wss://rh.example").empty());
    }

    // requests
    AdminQuery q; q.q = "lu\"cas"; q.by_first = true; q.asc = true; q.status = "pending"; q.offset = 50; q.limit = 500;
    std::string j = admin_list_json(q);
    CHECK(j.find("\"type\":\"admin_list\"") == 1);
    CHECK(j.find("\"q\":\"lu\\\"cas\"") != std::string::npos);
    CHECK(j.find("\"sort\":\"first\"") != std::string::npos && j.find("\"order\":\"asc\"") != std::string::npos);
    CHECK(j.find("\"limit\":50") != std::string::npos && j.find("\"offset\":50") != std::string::npos);   // clamped
    AdminQuery d;
    CHECK(admin_list_json(d).find("\"sort\":\"last\"") != std::string::npos && admin_list_json(d).find("\"order\":\"desc\"") != std::string::npos);
    CHECK(admin_uid_json("ban", "abc") == "{\"type\":\"admin_ban\",\"uid\":\"abc\"}");
    CHECK(admin_settings_json() == "{\"type\":\"admin_settings\"}");
    CHECK(admin_settings_json(1, -1, 0) == "{\"type\":\"admin_settings\",\"set\":{\"approval\":true,\"autofriend\":false}}");
    CHECK(admin_settings_json(-1, 1, -1) == "{\"type\":\"admin_settings\",\"set\":{\"vouch\":true}}");
    CHECK(admin_settings_json(-1, -1, -1, 1) == "{\"type\":\"admin_settings\",\"set\":{\"strict_rooms\":true}}");
    CHECK(admin_settings_json(0, -1, -1, 0) == "{\"type\":\"admin_settings\",\"set\":{\"approval\":false,\"strict_rooms\":false}}");
    CHECK(admin_strict_rooms_help().find("1.31.11") != std::string::npos);

    // words
    CHECK(admin_ago(1000000, 0) == "never");
    CHECK(admin_ago(1000000, 1000000 - 30000) == "just now");
    CHECK(admin_ago(10000000, 10000000 - 5 * 60000) == "5 min ago");
    CHECK(admin_ago(100000000, 100000000 - 3 * 3600000LL) == "3 hours ago");
    CHECK(admin_ago(1000000000000LL, 1000000000000LL - 86400000LL) == "1 day ago");
    CHECK(admin_ago(1000000000000LL, 1000000000000LL - 90 * 86400000LL) == "3 months ago");
    CHECK(admin_how_text("auto", "friend").find("automatically") != std::string::npos);
    CHECK(admin_how_text("room", "friend").find("room") != std::string::npos);
    CHECK(admin_how_text("", "requested_you").find("friend request") != std::string::npos);
    CHECK(admin_approval_help(true).find("Your current friends are not affected") != std::string::npos);
    CHECK(admin_autofriend_help().find("Your current friends are not affected") != std::string::npos);
    CHECK(admin_ban_confirm("lucas").find("Ban lucas?") == 0);

    // accounts
    AdminAccount a; a.name = "lucas"; a.status = "banned";
    CHECK(a.blocked() && !a.pending() && a.shown_name() == "lucas");
    a.username = "Lucas99"; a.status = "pending";
    CHECK(a.pending() && !a.blocked() && a.shown_name() == "Lucas99");
    a.status = "denied";
    CHECK(a.blocked() && admin_status_label(a) == "Denied");

    // result sentences
    CHECK(admin_result_text("ban", true, "lucas", "", true, true) == "Banned lucas.");
    CHECK(admin_result_text("ban", true, "lucas", "", true, false).find("Not applied yet - retrying") != std::string::npos);
    AdminQuery tq; tq.show_test = true;
    CHECK(admin_list_json(tq).find("\"tests\":true") != std::string::npos && admin_list_json(AdminQuery()).find("\"tests\":false") != std::string::npos);
    CHECK(admin_result_text("ban", false, "lucas", "ban-cap", false, false).find("full") != std::string::npos);
    CHECK(admin_result_text("approve", true, "", "", false, false) == "Approved that account.");

    // ---- invite codes ----
    CHECK(invite_clean("abcd efgh") == "ABCDEFGH" && invite_clean("ab-cd--ef gh jk") == "ABCDEFGH" && invite_clean("") == "");
    CHECK(invite_format("k7qm2xdt") == "K7QM-2XDT" && invite_format("k7q") == "K7Q" && invite_format("k7qm") == "K7QM" && invite_format("k7qm2") == "K7QM-2");
    CHECK(invite_valid("K7QM-2XDT") && invite_valid("k7qm 2xdt") && invite_valid("k7qm2xdt"));
    CHECK(!invite_valid("K7QM-2XD") && invite_valid("K7QM-2XDT9"));   // a ninth character is cut off, the code stays valid
    CHECK(!invite_valid("K7QM-2XD0") && !invite_valid("K7QM-2XDO") && !invite_valid("K7QM-2XDI") && !invite_valid("K7QM-2XDL") && !invite_valid("K7QM-2XD1"));
    CHECK(invite_field_text("k7qm2xdt") == "K7QM-2XDT");
    CHECK(invite_field_text("Join me on RetroHub! enter invite code K7QM-2XDT on the waiting screen") == "K7QM-2XDT");
    CHECK(invite_field_text("retrohub://invite/K7QM-2XDT") == "K7QM-2XDT");
    {
        std::string c;
        CHECK(invite_find_in_text("code: k7qm-2xdt.", &c) && c == "K7QM-2XDT");
        CHECK(!invite_find_in_text("no code here, ABCD-EFG", &c));
        CHECK(!invite_find_in_text("XK7QM-2XDTX", &c));   // inside a longer word
    }
    // the link
    {
        std::string c;
        CHECK(invite_link_code("retrohub://invite/K7QM-2XDT", &c) && c == "K7QM-2XDT");
        CHECK(invite_link_code("RETROHUB://Invite/k7qm-2xdt/", &c) && c == "K7QM-2XDT");
        CHECK(invite_link_code("\"retrohub://invite/K7QM2XDT?x=1#y\"", &c) && c == "K7QM-2XDT");
        CHECK(invite_link_code("retrohub:invite/K7QM%2D2XDT", &c) && c == "K7QM-2XDT");
        CHECK(invite_link_code("<retrohub://invite/K7QM-2XDT>", &c) && c == "K7QM-2XDT");
        CHECK(!invite_link_code("retrohub://invite/K7QM-2XD", &c) && !invite_link_code("retrohub://invite/K7QM-2XD0", &c));
        CHECK(!invite_link_code("retrohub://join/ABCDEF", &c) && !invite_link_code("retrohub://K7QM-2XDT", &c) && !invite_link_code("https://x/invite/K7QM-2XDT", &c));
        CHECK(!invite_link_code("retrohub://invite/K7QM-2XDT/extra", &c));
        CHECK(invite_link("k7qm2xdt") == "retrohub://invite/K7QM-2XDT");
    }
    CHECK(invite_share_text("K7QM-2XDT") == "Join me on RetroHub! Install it from https://github.com/fatty8me2/retrohub/releases/latest , then enter invite code K7QM-2XDT on the waiting screen (or open retrohub://invite/K7QM-2XDT)");
    // friend codes typed by the owner
    {
        std::string f;
        CHECK(friend_code_normalize("rh-7kq2-m9xd", &f) && f == "RH-7KQ2-M9XD");
        CHECK(friend_code_normalize("7KQ2M9XD", &f) && f == "RH-7KQ2-M9XD");
        CHECK(friend_code_normalize(" RH 7KQ2 M9XD ", &f) && f == "RH-7KQ2-M9XD");
        CHECK(!friend_code_normalize("RH-7KQ2-M9X", &f) && !friend_code_normalize("", &f));
        CHECK(friend_code_field_text("rh7kq2m9xd") == "7KQ2-M9XD" && friend_code_field_text("7kq") == "7KQ" && friend_code_field_text("7kq2m") == "7KQ2-M" && friend_code_field_text("rh-7kq") == "7KQ" && friend_code_field_text("R") == "R" && friend_code_field_text("RH") == "RH");
    }
    // requests
    CHECK(redeem_invite_json("k7qm2xdt") == "{\"type\":\"redeem_invite\",\"code\":\"K7QM-2XDT\"}");
    CHECK(invite_create_json(1, 168, "Mia\"s") == "{\"type\":\"admin_invite_create\",\"uses\":1,\"hours\":168,\"note\":\"Mia\\\"s\"}");
    CHECK(invite_create_json(99, 0, "").find("\"uses\":20,\"hours\":1,") != std::string::npos && invite_create_json(0, 9999, "").find("\"uses\":1,\"hours\":720,") != std::string::npos);
    CHECK(invite_list_json() == "{\"type\":\"admin_invite_list\"}" && invite_revoke_json("k7qm2xdt") == "{\"type\":\"admin_invite_revoke\",\"code\":\"K7QM-2XDT\"}");
    CHECK(approve_code_json("RH-7KQ2-M9XD") == "{\"type\":\"admin_approve_code\",\"code\":\"RH-7KQ2-M9XD\"}");
    // the newcomer's answer (and not to be confused with a room invite result)
    {
        json::Value j; RedeemResult r;
        CHECK(json::parse("{\"type\":\"invite_result\",\"ok\":true}", &j) && parse_redeem_result(j, &r) && r.ok && r.reason.empty());
        CHECK(json::parse("{\"type\":\"invite_result\",\"ok\":false,\"reason\":\"used-up\"}", &j) && parse_redeem_result(j, &r) && !r.ok && r.reason == "used-up");
        CHECK(json::parse("{\"type\":\"invite_result\",\"results\":[{\"uid\":\"x\",\"ok\":true}]}", &j) && !parse_redeem_result(j, &r));
        CHECK(json::parse("{\"type\":\"invite_result\"}", &j) && !parse_redeem_result(j, &r));
        const char* rs[] = {"invalid", "expired", "used-up", "rate-limited", "busy", "weird"};
        for (const char* x : rs) CHECK(!redeem_result_text(false, x).empty() && redeem_result_text(false, x).find("invalid") == std::string::npos);
        CHECK(redeem_result_text(false, "rate-limited").find("Too many tries") == 0 && redeem_result_text(false, "busy").find("busy") != std::string::npos);
        CHECK(redeem_result_text(true, "").find("accepted") != std::string::npos);
    }
    // the owner's list
    {
        json::Value j; AdminInvites a;
        const char* t = "{\"type\":\"admin_invites\",\"invites\":[{\"code\":\"k7qm-2xdt\",\"note\":\"Mia\",\"uses\":3,\"used\":1,\"exp\":1900000000000,\"created\":1800000000,\"by_names\":[\"Jake\"]},"
                        "{\"code\":\"BAD0-CODE\"},{\"code\":\"H4NP-9WRB\",\"uses\":1,\"used\":1,\"exp\":5}],\"created\":{\"code\":\"K7QM-2XDT\",\"uses\":3,\"used\":0,\"exp\":1900000000000,\"created\":1800000000000}}";
        CHECK(json::parse(t, &j) && parse_admin_invites(j, &a) && a.valid);
        CHECK(a.list.size() == 2 && a.list[0].code == "K7QM-2XDT" && a.list[0].note == "Mia" && a.list[0].uses == 3 && a.list[0].used == 1 && a.list[0].by_names.size() == 1);
        CHECK(a.list[0].created == 1800000000000LL && a.list[1].spent());   // seconds are taken as seconds
        CHECK(a.has_created && a.created.code == "K7QM-2XDT" && a.created.uses == 3);
        CHECK(json::parse("{\"type\":\"admin_invites\",\"invites\":[]}", &j) && parse_admin_invites(j, &a) && a.valid && a.list.empty() && !a.has_created);
        CHECK(json::parse("{\"type\":\"admin_list\"}", &j) && !parse_admin_invites(j, &a));
        CHECK(json::parse("{\"type\":\"admin_invites\",\"invites\":\"x\"}", &j) && parse_admin_invites(j, &a) && a.list.empty());
    }
    {
        const int64_t now = 1000000000000LL;
        AdminInvite a; a.code = "AAAA-BBBB"; a.uses = 3; a.used = 1; a.exp = now + 6 * 86400000LL; a.by_names = {"Mia", "Jake"};
        CHECK(invite_left_text(a.exp, now) == "6 days left" && invite_left_text(now + 5 * 3600000LL, now) == "5 hours left" && invite_left_text(now + 40 * 60000LL, now) == "40 min left" && invite_left_text(now - 1, now) == "expired");
        CHECK(invite_used_text(a) == "1/3 used: Mia, Jake");
        AdminInvite b = a; b.code = "CCCC-DDDD"; b.used = 3; b.created = 5;
        AdminInvite c = a; c.code = "EEEE-FFFF"; c.created = 9; c.used = 0;
        std::vector<AdminInvite> v = {b, a, c};
        invite_sort(&v, now);
        CHECK(v[0].code == "EEEE-FFFF" && v[2].code == "CCCC-DDDD");   // live newest first, spent last
        CHECK(invite_options_text(1, 168) == "1 person, 7 days" && invite_options_text(5, 24) == "5 people, 1 day" && invite_options_text(2, 5) == "2 people, 5 hours");
    }
    // pushes to the owner
    {
        json::Value j;
        CHECK(json::parse("{\"type\":\"admin_pending\",\"pending\":2,\"latest\":{\"uid\":\"u1\",\"name\":\"newguy\",\"code\":\"RH-7KQ2-MNXD\",\"created\":1800000000}}", &j));
        PendingLatest p = parse_pending_latest(j);
        CHECK(p.valid && p.uid == "u1" && p.name == "newguy" && p.code == "RH-7KQ2-MNXD" && p.created == 1800000000000LL);
        CHECK(json::parse("{\"type\":\"admin_pending\",\"pending\":0,\"latest\":null}", &j) && !parse_pending_latest(j).valid);
        CHECK(json::parse("{\"type\":\"admin_pending\",\"pending\":1}", &j) && !parse_pending_latest(j).valid);
        CHECK(waiting_is_fresh(1000000, 1000000 + 60000) && !waiting_is_fresh(1000000, 1000000 + 3600000) && waiting_is_fresh(0, 5));
        CHECK(waiting_bell_text("newguy", "RH-7KQ2-MNXD") == "newguy (RH-7KQ2-MNXD) wants to join" && waiting_bell_text("", "") == "Someone wants to join");
        CHECK(json::parse("{\"type\":\"admin_invite_used\",\"code\":\"k7qm2xdt\",\"name\":\"Kai\",\"uid\":\"u2\"}", &j));
        InviteUsed u = parse_invite_used(j);
        CHECK(u.valid && u.code == "K7QM-2XDT" && u.name == "Kai" && joined_bell_text(u.name, u.code) == "Kai joined with invite K7QM-2XDT");
    }
    CHECK(admin_result_text("approve", false, "", "not-found", false, false).find("friend code") != std::string::npos);
    CHECK(admin_result_text("approve", false, "", "not-pending", false, false).find("isn't waiting") != std::string::npos);
    {
        json::Value j; AdminInvites a;
        CHECK(json::parse("{\"type\":\"admin_invites\",\"ok\":false,\"reason\":\"invite-cap\",\"invites\":[]}", &j) && parse_admin_invites(j, &a) && !a.ok && a.reason == "invite-cap");
        CHECK(json::parse("{\"type\":\"admin_invites\",\"ok\":true,\"invites\":[]}", &j) && parse_admin_invites(j, &a) && a.ok);
        CHECK(admin_result_text("invite", false, "", "not-found", false, false).find("gone") != std::string::npos);
    }
    CHECK(admin_result_text("x", false, "", "invite-cap", false, false).find("Revoke") != std::string::npos);

    printf(g_fail ? "%d FAILED\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
