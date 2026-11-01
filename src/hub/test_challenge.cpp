/* hubchallenge_test: pure challenge logic (cards, leaderboard, rules) + attach helpers. No network. */
#include "attach_xfer.h"
#include "challenge_logic.h"

#include <cstdio>
#include <random>

using namespace hub::challenge;
static int g_fail = 0;
#define CHECK(c, ...) do { if (c) printf("  ok   %s\n", #c); else { g_fail++; printf("  FAIL %s  ", #c); printf("" __VA_ARGS__); printf("\n"); } } while (0)

static hubnet::Attach att(const char* idc, const char* kind, int64_t size) {
    hubnet::Attach a;
    a.id = std::string(32, idc[0]); a.name = std::string("name-") + kind; a.sha = std::string(64, 'a'); a.kind = kind;
    a.size = size; a.expires = 1900000000000LL;
    return a;
}
static Card chal() {
    Card c;
    c.kind = CardKind::Challenge; c.id = new_challenge_id();
    c.game_key = "n64/super-mario-64"; c.game_title = "Super Mario 64"; c.system = "n64"; c.rom_key = std::string(40, 'f');
    c.note = "bet you can't"; c.sender = "Mickey"; c.goal.kind = GoalKind::Score; c.goal.target = 12000;
    c.state = att("1", "state", 300000); c.has_shot = true; c.shot = att("2", "shot", 90000);
    return c;
}
static Card res(const std::string& id, const char* who, Outcome o, int64_t tm, int64_t sc, int64_t at) {
    Card c; c.kind = CardKind::Result; c.id = id; c.player = who; c.outcome = o; c.time_ms = tm; c.score = sc; c.at = at; return c;
}

int main() {
    printf("[cards]\n");
    Card c = chal();
    std::string w = encode_card(c);
    CHECK(!w.empty() && w.size() <= hubnet::kCardMax && w.compare(0, 5, "RHC1:") == 0, "%zu", w.size());
    Card d;
    CHECK(decode_card(w, &d));
    CHECK(d.kind == CardKind::Challenge && d.id == c.id && d.game_key == c.game_key && d.game_title == "Super Mario 64" && d.rom_key == c.rom_key);
    CHECK(d.goal.kind == GoalKind::Score && d.goal.target == 12000 && d.note == c.note && d.sender == "Mickey");
    CHECK(d.state.id == c.state.id && d.state.size == 300000 && d.state.sha == c.state.sha && d.state.kind == "state" && d.state.expires == c.state.expires);
    CHECK(d.has_shot && d.shot.id == c.shot.id && d.shot.kind == "shot");
    Card c2 = chal(); c2.has_shot = false; c2.goal.kind = GoalKind::FastestTime;
    CHECK(decode_card(encode_card(c2), &d) && !d.has_shot && d.goal.kind == GoalKind::FastestTime);
    Card r = res(c.id, "Kai", Outcome::Done, 83450, -1, 1700000000000LL);
    CHECK(decode_card(encode_card(r), &d) && d.kind == CardKind::Result && d.player == "Kai" && d.time_ms == 83450 && d.score == -1 && d.at == 1700000000000LL && d.outcome == Outcome::Done);
    CHECK(decode_card(encode_card(res(c.id, "Kai", Outcome::GaveUp, -1, -1, 5)), &d) && d.outcome == Outcome::GaveUp);
    Card big = chal();
    big.game_title = std::string(500, 'x'); big.note = std::string(900, 'n'); big.sender = std::string(400, 's');
    big.state.name = std::string(300, 'q'); big.shot.name = std::string(300, 'p');
    std::string bw = encode_card(big);
    CHECK(!bw.empty() && bw.size() <= hubnet::kCardMax && decode_card(bw, &d) && d.game_title.size() <= 64, "long fields are trimmed, %zu", bw.size());
    Card uni = chal(); uni.game_title = "Pok\xC3\xA9mon \xE2\x80\x93 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
    CHECK(decode_card(encode_card(uni), &d) && d.game_title == uni.game_title, "utf8 survives");
    CHECK(looks_like_card(w) && !looks_like_card("hello") && !looks_like_card("RHC1:"));
    CHECK(!card_fallback_text(c).empty() && card_fallback_text(c).find("Super Mario 64") != std::string::npos);

    printf("[decode is defensive]\n");
    CHECK(!decode_card("", &d) && !decode_card("RHC1:", &d) && !decode_card("RHC2:abc", &d) && !decode_card("RHC1:!!!!", &d));
    CHECK(!decode_card(w.substr(0, w.size() / 2), &d), "truncated");
    CHECK(!decode_card(w + std::string(2000, 'A'), &d), "oversize");
    CHECK(!decode_card(w, nullptr));
    std::mt19937 rng(7);
    int bad = 0, accepted = 0;
    for (int i = 0; i < 4000; i++) {
        std::string m = w;
        int n = 1 + rng() % 4;
        for (int k = 0; k < n; k++) {
            size_t p = 5 + rng() % (m.size() - 5);
            switch (rng() % 3) { case 0: m[p] = "ABCxyz09-_"[rng() % 10]; break; case 1: m.erase(p, 1 + rng() % 5); break; default: m.insert(p, 1, (char)(rng() % 256)); }
            if (m.size() <= 6) break;
        }
        Card t;
        bool ok = decode_card(m, &t);
        accepted += ok;
        if (ok && (t.id.empty() || t.id.size() > 32 || t.state.id.size() != 32)) bad++;
    }
    CHECK(bad == 0, "fuzz: %d malformed accepted (accepted total %d)", bad, accepted);
    for (int i = 0; i < 2000; i++) {
        std::string g = "RHC1:"; int n = rng() % 300;
        for (int k = 0; k < n; k++) g += "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"[rng() % 64];
        Card t; (void)decode_card(g, &t);
    }
    CHECK(true, "random garbage did not crash");
    CHECK(new_challenge_id().size() == 16 && new_challenge_id() != new_challenge_id());

    printf("[text]\n");
    CHECK(format_time(83450) == "1:23.45", "%s", format_time(83450).c_str());
    CHECK(format_time(5070) == "0:05.07" && format_time(0) == "0:00.00" && format_time(3723450) == "1:02:03.45");
    CHECK(format_score(12000) == "12,000" && format_score(7) == "7" && format_score(1234567) == "1,234,567");
    CHECK(osd_timer_text(5300) == "0:05.3" && osd_timer_text(83450) == "1:23.4" && osd_timer_text(3723000) == "1:02:03");
    CHECK(goal_text(Goal{GoalKind::Score, 12000}) == "Score 12,000" && goal_text(Goal{GoalKind::FastestTime, 0}) == "Fastest time");

    printf("[leaderboard]\n");
    {
        Goal g{GoalKind::FastestTime, 0};
        std::vector<Card> rs = {
            res("A", "Kai", Outcome::Done, 90000, -1, 10), res("A", "Mia", Outcome::Done, 80000, -1, 20), res("A", "Kai", Outcome::Done, 70000, -1, 30),
            res("A", "Zed", Outcome::GaveUp, -1, -1, 5), res("A", "Bo", Outcome::Done, 80000, -1, 25), res("B", "Other", Outcome::Done, 1, -1, 1),
            res("A", "mia", Outcome::GaveUp, -1, -1, 40)};
        auto rows = leaderboard(g, "A", rs);
        CHECK(rows.size() == 4, "%zu", rows.size());
        CHECK(rows[0].player == "Kai" && rows[0].rank == 1 && rows[0].text == "1:10.00");
        CHECK(rows[1].player == "Mia" && rows[1].rank == 2 && rows[2].player == "Bo" && rows[2].rank == 2, "tie shares rank (Mia, Bo)");
        CHECK(rows[3].player == "Zed" && rows[3].gave_up && rows[3].rank == 0 && rows[3].text == "Gave up");
    }
    {
        Goal g{GoalKind::Score, 0};
        std::vector<Card> rs = {res("A", "Kai", Outcome::Done, -1, 500, 1), res("A", "Mia", Outcome::Done, -1, 12000, 2), res("A", "Kai", Outcome::Done, -1, 300, 3),
                                res("A", "Bo", Outcome::Done, -1, -1, 4)};
        auto rows = leaderboard(g, "A", rs);
        CHECK(rows.size() == 3 && rows[0].player == "Mia" && rows[0].text == "12,000" && rows[1].player == "Kai" && rows[1].text == "500" && rows[2].player == "Bo" && rows[2].text == "Done");
    }
    {
        Goal g{GoalKind::JustBeatIt, 0};
        std::vector<Card> rs = {res("A", "Late", Outcome::Done, -1, -1, 50), res("A", "Early", Outcome::Done, -1, -1, 10), res("A", "Early", Outcome::Done, -1, -1, 99)};
        auto rows = leaderboard(g, "A", rs);
        CHECK(rows.size() == 2 && rows[0].player == "Early" && rows[0].rank == 1 && rows[1].player == "Late" && rows[1].rank == 2, "completion order");
        std::vector<Card> many;
        for (int i = 0; i < 20; i++) many.push_back(res("A", ("p" + std::to_string(i)).c_str(), Outcome::Done, -1, -1, i));
        CHECK(leaderboard(g, "A", many).size() == 8 && leaderboard(g, "A", many, 3).size() == 3, "cap");
        CHECK(leaderboard(g, "nope", rs).empty());
    }
    CHECK(result_text(res("A", "x", Outcome::GaveUp, -1, -1, 0), Goal{}) == "Gave up");

    printf("[rules]\n");
    CHECK(rom_check("ABCDEF", "abcdef") == RomCheck::Same && rom_check("a", "b") == RomCheck::Different && rom_check("", "") == RomCheck::Different && rom_check("a", "") == RomCheck::Different);
    CHECK(rom_check_message("Super Mario 64") == "You need the same game (Super Mario 64) to take this challenge.");
    CHECK(runner_supports_challenge("n64") && runner_supports_challenge("gba") && runner_supports_challenge("nds") && runner_supports_challenge("psx"));
    CHECK(!runner_supports_challenge("gc") && !runner_supports_challenge("wiiu") && !runner_supports_challenge("3ds") && !runner_supports_challenge("ps2") && !runner_supports_challenge(""));
    std::string why;
    CHECK(challenge_state_ok(8 * 1024 * 1024) && !challenge_state_ok(8 * 1024 * 1024 + 1, &why) && why.find("8 MB") != std::string::npos, "%s", why.c_str());
    CHECK(!challenge_state_ok(0, &why) && !why.empty());
    CHECK(challenge_eligible("n64", 500000) && !challenge_eligible("nds", 20 * 1024 * 1024, &why) && !challenge_eligible("gc", 100, &why) && challenge_eligible("gba", 0));
    CHECK(kChallengeDisablesHardcore && hardcore_note().find("hardcore") != std::string::npos && !offline_only_note().empty());
    CHECK(scratch_dir("C:\\data") == "C:\\data\\challenge\\" && scratch_dir("C:\\data\\") == "C:\\data\\challenge\\");
    CHECK(scratch_state_name("AB12", true) == "ch-ab12-recv.state" && scratch_state_name("AB12", false) == "ch-ab12-send.state" && scratch_shot_name("ab12") == "ch-ab12.png");
    CHECK(scratch_state_name("../../x", true).find("..") == std::string::npos && scratch_path("C:\\d", "..\\evil").find("..") == std::string::npos);

    printf("[attach helpers]\n");
    using namespace hub::attach;
    CHECK(attach_size_problem(1000, "shot").empty() && attach_size_problem(kMaxBytes, "state").empty());
    CHECK(attach_size_problem(kMaxBytes + 1, "shot").find("8 MB") != std::string::npos && attach_size_problem(20 * 1024 * 1024, "state").find("20.0 MB") != std::string::npos);
    CHECK(!attach_size_problem(0, "shot").empty());
    CHECK(attach_error_text(413, "attachment-too-large") == "Attachments are limited to 8 MB.");
    const char* codes[] = {"attachment-too-large", "attachment-daily-limit", "account-daily-limit", "attachment-pool-full", "attachment-pool-tight", "quota-exceeded",
        "cloud-saves-full", "cloud-saves-paused", "cloud-saves-busy-today", "cloud-saves-unavailable", "rate-limited", "not-friends", "bad-recipient", "bad-kind",
        "bad-sha", "sha-required", "bad-body", "attachment-expired", "not-found", "cloud-saves-disabled", "cloud-saves-error", "cloud-saves-retry", "unauthorized", "device-revoked", "weird"};
    bool all = true; for (const char* k : codes) { std::string t = attach_error_text(500, k); if (t.empty() || t.find("error") != std::string::npos && std::string(k) != "weird" && std::string(k).find("cloud") == std::string::npos) all = false; }
    CHECK(all, "every code has a plain text");
    CHECK(attach_error_text(410, "") .find("expired") != std::string::npos && attach_error_text(0, "").find("reach") != std::string::npos && attach_error_text(429, "attachment-daily-limit").find("tomorrow") != std::string::npos);
    CHECK(attach_error_text(507, "cloud-saves-full").find("full") != std::string::npos);
    CHECK(sanitize_name("C:\\a\\b\\My  shot?.png") == "My shot.png" && sanitize_name("") == "file" && sanitize_name("///") == "file" && sanitize_name(std::string(200, 'x')).size() == 64);
    CHECK(url_encode("a b&c/\xC3\xA9") == "a%20b%26c%2F%C3%A9");
    CHECK(http_base_from_server("wss://x.dev/") == "https://x.dev" && http_base_from_server("ws://127.0.0.1:8772") == "http://127.0.0.1:8772");
    CHECK(valid_kind("shot") && valid_kind("state") && valid_kind("patch") && !valid_kind("movie"));
    CHECK(size_text(2516582) == "2.4 MB" && size_text(310 * 1024) == "310 KB");
    {   // client-side refusal happens before any request: no account needed
        set_account_provider(nullptr);
        int j = upload("C:\\nonexistent\\file.png", "shot", "x", {"a"});
        Result rr;
        CHECK(wait(j, &rr, 5000) && rr.done && !rr.ok && !rr.error.empty(), "missing file: %s", rr.error.c_str());
        int j2 = upload("x", "movie", "x", {"a"});
        CHECK(poll(j2, &rr) && rr.done && !rr.ok);
        int j3 = upload("x", "shot", "x", {});
        CHECK(poll(j3, &rr) && rr.done && !rr.ok && rr.error.find("friend") != std::string::npos, "%s", rr.error.c_str());
        hubnet::Attach old = att("3", "shot", 10); old.expires = 1000;
        int j4 = download(old, "C:\\nope\\x.png");
        CHECK(poll(j4, &rr) && rr.done && !rr.ok && rr.error.find("expired") != std::string::npos, "%s", rr.error.c_str());
    }
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
