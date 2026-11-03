/* hubadmin_test: owner dashboard + account access rules (pure logic, no network). */
#include "admin_logic.h"

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
    CHECK(access_message(Access::Ok).empty() && access_title(Access::Ok).empty());

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
    CHECK(admin_result_text("ban", true, "lucas", "", true, false).find("next checks in") != std::string::npos);
    CHECK(admin_result_text("ban", false, "lucas", "ban-cap", false, false).find("full") != std::string::npos);
    CHECK(admin_result_text("approve", true, "", "", false, false) == "Approved that account.");

    printf(g_fail ? "%d FAILED\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
