#include "cloud_ui_logic.h"
#include "logic.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubui::cloudui;

int main() {
    CloudAccountInfo on; on.known = true; on.online = true; on.cap = 50LL << 20; on.used = 18LL << 20;
    CloudAccountInfo off = on; off.online = false;
    GameCloudInfo g;

    g.state = CloudState::Synced; CHECK(game_chip(g, on).text == "Synced" && game_chip(g, on).tone == Tone::Good);
    g.state = CloudState::Conflict; CHECK(needs_attention(g));
    g.state = CloudState::Synced; CHECK(!needs_attention(g));
    g.state = CloudState::LocalOnly; g.local_only = LocalOnlyReason::User; CHECK(!needs_attention(g));
    g.local_only = LocalOnlyReason::OverCap; CHECK(needs_attention(g));
    { std::vector<GameCloudInfo> v{g, GameCloudInfo()}; CHECK(attention_count(v) == 1); }
    g.local_only = LocalOnlyReason::None;
    g.state = CloudState::Syncing; g.message = "Uploading"; CHECK(game_chip(g, on).text == "Uploading");
    g.message = "Downloading"; CHECK(game_chip(g, on).text == "Downloading");
    g.state = CloudState::Waiting; CHECK(game_chip(g, on).text == "Waiting to sync" && game_chip(g, off).text == "Offline");
    g.state = CloudState::LocalOnly; g.local_only = LocalOnlyReason::OverCap;
    CHECK(game_chip(g, on).text == "This PC only (over the limit)" && game_chip(g, on).tone == Tone::Warn);
    g.local_only = LocalOnlyReason::User; CHECK(game_chip(g, on).text == "This PC only (your choice)");
    g.state = CloudState::Conflict; CHECK(game_chip(g, on).text == "Conflict" && game_chip(g, on).tone == Tone::Bad);
    g.state = CloudState::PlayingElsewhere; CHECK(game_chip(g, on).text == "Open on another PC");
    g.state = CloudState::NeedsAttention; CHECK(game_chip(g, on).tone == Tone::Bad);
    g.state = CloudState::Unknown; CHECK(game_chip(g, on).text == "Checking..." && game_chip(g, off).text == "Offline");

    g.state = CloudState::Synced; CHECK(tile_pill(g, on) == "CLOUD");
    g.state = CloudState::Unknown; CHECK(tile_pill(g, on).empty());
    g.state = CloudState::Waiting; CHECK(tile_pill(g, off) == "OFFLINE" && tile_pill(g, on).empty());

    {   // linked PCs
        LinkedPc me{"d-me", "PC-DESK", 100, 100000, true}, tv{"d-tv", "PC-LIVING", 100, 100000 - 7200, false}, old{"d-old", "", 100, 0, false};
        const int64_t now = 100000;
        CHECK(!pc_can_revoke(me) && pc_can_revoke(tv) && pc_can_revoke(old) && !pc_can_revoke(LinkedPc()));
        CHECK(pc_last_seen(me, now) == "This PC" && pc_last_seen(tv, now) == "Last seen 2 hours ago" && pc_last_seen(old, now) == "Last seen: not yet");
        CHECK(pc_name(old) == "Unnamed PC" && pc_revoke_title(tv) == "Stop syncing on PC-LIVING?");
        CHECK(pc_revoke_body(tv).find("PC-LIVING") == 0 && pc_revoke_body(tv).find("Nothing is deleted") != std::string::npos && pc_revoke_body(tv).find("new account code") != std::string::npos && pc_revoke_body(tv).find("turns sync back on") != std::string::npos);
        auto s = pcs_sorted({old, tv, me});
        CHECK(s.size() == 3 && s[0].current && s[1].dev == "d-tv" && s[2].dev == "d-old");
        CloudAccountInfo rv = on; rv.needs_attention = true; rv.attention = "device-revoked";
        CHECK(account_headline(rv, true).text == "Cloud sync was stopped on this PC" && account_headline(rv, true).tone == Tone::Bad);
        CHECK(attention_help("device-revoked").find("stopped cloud sync on this PC") != std::string::npos);
    }
    CHECK(quota_text(on) == "18 MB of 50 MB");
    CHECK(quota_text(CloudAccountInfo()).empty());
    CHECK(mb_text(300 * 1024) == "300 KB");
    CHECK(std::abs(quota_fraction(on) - 0.36f) < 0.001f);
    CHECK(quota_tone(on) == Tone::Good);
    CloudAccountInfo tight = on; tight.used = 46LL << 20; CHECK(quota_tone(tight) == Tone::Warn);
    CloudAccountInfo full = on; full.pool = "full"; CHECK(quota_tone(full) == Tone::Bad);

    CHECK(account_headline(on, true).text == "Your saves are backed up");
    CHECK(account_headline(off, true).tone == Tone::Muted);
    CHECK(account_headline(on, false).text == "Cloud sync is paused");
    { CloudAccountInfo q = on; q.queued_games = 3; CHECK(account_headline(q, false).text == "Cloud sync is paused - 3 games waiting"); q.queued_games = 1; CHECK(paused_text(1) == "Cloud sync is paused - 1 game waiting"); }
    {
        std::vector<GameCloudInfo> v(3);
        v[0].state = CloudState::Synced; v[1].state = CloudState::Conflict; v[2].state = CloudState::Conflict;
        CHECK(conflict_count(v) == 2 && conflict_banner(2).find("2 games have saves that differ") == 0 && conflict_banner(1).find("1 game has") == 0 && conflict_banner(0).empty());
        v[1].local_only = LocalOnlyReason::None; CHECK(game_chip(v[1], on).text == "Conflict");   // plain conflict, not just "decide later"
    }
    {
        std::vector<CatalogName> cat = {{"oot", "The Legend of Zelda: Ocarina of Time", "n64"}, {"ootmq", "Zelda: Ocarina of Time Master Quest", "n64"},
                                         {"mm", "The Legend of Zelda: Majora's Mask", "n64"}, {"mp1", "Mario Party", "n64"}};
        CHECK(friendly_title("Mario Kart 64", "n64", "n64:abc", cat, "Nintendo 64") == "Mario Kart 64");
        CHECK(friendly_title("zelda-ocarina", "n64", "zelda-ocarina", cat, "Nintendo 64") == "The Legend of Zelda: Ocarina of Time");
        CHECK(friendly_title("", "n64", "mp1", cat, "Nintendo 64") == "Mario Party");
        CHECK(friendly_title("", "gba", "gba:bpee-1a2b3c4d", cat, "Game Boy Advance") == "Game Boy Advance game");
        CHECK(friendly_title("", "n64", "3f2a9c1e8b7d4a5c", cat, "") == "Game from another PC");
        CHECK(friendly_title("", "n64", "some-obscure-game", cat, "Nintendo 64") == "Some Obscure Game");
        CHECK(friendly_title("", "n64", "zelda-ocarina", cat, "x").find("zelda-") == std::string::npos);
    }
    CloudAccountInfo att = on; att.needs_attention = true; CHECK(account_headline(att, true).tone == Tone::Bad);
    CHECK(account_headline(CloudAccountInfo(), true).text.find("Checking") == 0);

    CHECK(over_limit_text(on, {}).empty());
    CHECK(over_limit_text(full, {}).find("full") != std::string::npos);
    CHECK(over_limit_text(on, {"A", "B"}) .find("older save states of A, B stay on this PC only") != std::string::npos);
    CHECK(over_limit_text(on, {"A", "B", "C", "D", "E"}).find("A, B, C and 2 more") != std::string::npos);

    CHECK(ago_text(1000, 0) == "never" && ago_text(1000, 990) == "just now");
    CHECK(ago_text(10000, 10000 - 300) == "5 minutes ago" && ago_text(10000, 10000 - 7300) == "2 hours ago");
    CHECK(ago_text(100, 70) == "just now");

    GameCloudInfo pe; pe.state = CloudState::PlayingElsewhere; pe.playing_on = "PC-LIVING";
    CHECK(game_detail(pe, on, 0).find("Open on PC-LIVING") == 0);
    GameCloudInfo sy; sy.state = CloudState::Synced; sy.last_synced = 9700; sy.cloud_bytes = 1 << 20; sy.pinned = true;
    CHECK(game_detail(sy, on, 10000) == "Last synced 5 minutes ago - 1 MB in the cloud - kept in cloud");

    ConflictItem ci; ci.local = {true, "a", 412 * 1024, 2000, "PC-DESK"}; ci.cloud = {true, "b", 410 * 1024, 3000, "PC-LIVING"};
    CHECK(!local_is_newer(ci));
    CHECK(!choice_keeps_local(ConflictChoice::UseNewer, ci) && choice_keeps_local(ConflictChoice::UseLocal, ci) &&
          !choice_keeps_local(ConflictChoice::UseCloud, ci) && choice_keeps_local(ConflictChoice::DecideLater, ci));
    CHECK(conflict_default() == ConflictChoice::UseNewer);
    CHECK(side_line(ci.cloud, true).find("Cloud: PC-LIVING, ") == 0 && side_line(ci.cloud, true).find("410 KB") != std::string::npos);
    CHECK(side_line(ci.local, false).find("This PC: played ") == 0);
    CHECK(side_line(FileSide(), true) == "Cloud: no save yet");
    CHECK(std::string(conflict_button(ConflictChoice::DecideLater)) == "Decide later");
    CHECK(lease_text("Tetris", "PC-LIVING", 0) == "Tetris is open on PC-LIVING. Playing here may create a conflict.");
    CHECK(std::string(lease_button(LeaseChoice::TakeOver)) == "Take over");

    PrelaunchResult pr;
    CHECK(prelaunch_notice(pr).empty());
    pr.files_pulled = 2; CHECK(prelaunch_notice(pr) == "Played on another PC - using that save.");
    pr.offline = true; CHECK(prelaunch_notice(pr).find("Offline") == 0);

    CloudEvent ev; ev.kind = CloudEvent::Pushed; ev.text = "x";
    CHECK(!event_toast(ev).show);
    ev.kind = CloudEvent::WentLocalOnly; ev.text = "Cloud space is full."; ev.good = true;
    CHECK(event_toast(ev).show && !event_toast(ev).good);
    ev.kind = CloudEvent::Pulled; ev.text.clear(); CHECK(!event_toast(ev).show);

    CHECK(normalize_code(" k7m2-qx9p 3fta") == "K7M2QX9P3FTA");
    CHECK(group_code("k7m2qx9p3fta") == "K7M2-QX9P-3FTA");
    CHECK(!code_plausible("K7M2-QX9P") && code_plausible("K7M2-QX9P-3FTA-W8RD-HN4C-ZB6Y-J2VE-5GL7"));
    CHECK(code_file_text("AAAA", "PC").find("AAAA") != std::string::npos);

    GameCloudInfo a, b; a.state = CloudState::Synced; b.state = CloudState::Conflict;
    CHECK(game_before(b, a) && !game_before(a, b));
    b.state = CloudState::Synced; a.last_synced = 5; b.last_synced = 9; CHECK(game_before(b, a));

    {   hubui::Settings s, t;   // migration: old files without cloud_enabled default to on
        CHECK(s.cloud_enabled);
        s.cloud_enabled = false;
        CHECK(hubui::settings_from_json(hubui::settings_to_json(s), &t) && !t.cloud_enabled);
        hubui::Settings u;
        CHECK(hubui::settings_from_json("{\"name\":\"x\"}", &u) && u.cloud_enabled);
        hubui::Settings v;
        CHECK(hubui::settings_from_json("{\"cloud_saves\":false}", &v) && v.cloud_enabled);   // legacy opt-in key is ignored
    }
    std::printf(fails ? "FAILED\n" : "cloud ui tests passed\n");
    return fails ? 1 : 0;
}
