// Console test: game night plans (S6): DM tag round trip, reminder/start timing, dedupe, persistence.
#include "gamenight_logic.h"
#include "gamenight_sched.h"

#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
using namespace hubui::night;

int main() {
    const int64_t M = 60000, H = 60 * M, now = 1000000LL * 1000;
    Night n; n.host_uid = "u1"; n.host_name = "Mia"; n.game_id = "mk64"; n.game_title = "Mario Kart 64"; n.start_ms = now + 30 * M;
    n.invitees = {"u2"};
    std::string t = encode(n, "Fri 8:00 PM");
    CHECK(t.size() < 500 && t.find("Game night: Mario Kart 64 on Fri 8:00 PM") == 0);
    Night p;
    CHECK(parse(t, &p) && p.game_id == "mk64" && p.game_title == "Mario Kart 64" && p.start_ms == n.start_ms);
    CHECK(display_text(t) == "Game night: Mario Kart 64 on Fri 8:00 PM - open RetroHub to join");
    CHECK(!parse("hello", &p) && !parse("[RHGN1|abc|x|y]", &p) && !parse("[RHGN1|123|x]", &p) && display_text("hi") == "hi");
    n.game_title = "Evil|]Title"; CHECK(parse(encode(n, "x"), &p) && p.game_title == "Evil  Title");

    n.game_title = "Mario Kart 64"; n.id = make_id("u1", n.start_ms);
    CHECK(phase_of(n, now) == Phase::Upcoming && !reminder_due(n, now));
    CHECK(phase_of(n, now + 20 * M) == Phase::Reminder && reminder_due(n, now + 20 * M) && !start_due(n, now + 20 * M));
    CHECK(phase_of(n, now + 30 * M) == Phase::Live && start_due(n, now + 30 * M) && reminder_due(n, now + 30 * M));  // PC was off: still reminded once
    CHECK(phase_of(n, now + 30 * M + kLiveMs) == Phase::Over);
    n.reminded = true; CHECK(!reminder_due(n, now + 25 * M));
    n.dismissed = true; CHECK(phase_of(n, now + 30 * M) == Phase::Over);
    n.dismissed = false;
    CHECK(when_text(n, now) == "in 30 mins" && when_text(n, now + 30 * M) == "now" && when_text(n, now + 42 * M) == "started 12 mins ago");
    CHECK(plan_error(n, now).empty());
    Night bad = n; bad.start_ms = now + 10000; CHECK(!plan_error(bad, now).empty());
    bad = n; bad.start_ms = now + 20 * 24 * H; CHECK(!plan_error(bad, now).empty());
    bad = n; bad.invitees.clear(); CHECK(!plan_error(bad, now).empty());
    bad = n; bad.game_id.clear(); CHECK(!plan_error(bad, now).empty());

    Plans pl;
    CHECK(pl.add(n, now) && !pl.add(n, now));          // a re-delivered DM does not re-add
    Night old = n; old.id = "x@1"; old.start_ms = now - 5 * H; CHECK(!pl.add(old, now));
    Night later = n; later.id = "y@2"; later.start_ms = now + 2 * H; pl.add(later, now);
    CHECK(pl.v.size() == 2 && pl.v[0].id == n.id && pl.active(now).size() == 2);
    pl.find(n.id)->reminded = true;
    std::string path = (std::filesystem::temp_directory_path() / "gn_test" / "gamenights.json").string();
    CHECK(save(path, pl));
    Plans r; CHECK(load(path, &r) && r.v.size() == 2 && r.v[0].reminded && r.v[0].invitees.size() == 1 && r.v[1].id == "y@2");
    std::filesystem::remove_all(std::filesystem::path(path).parent_path());
    {   // UTF-8 path round trip + legacy (ANSI-misread) migration + failed save keeps the old file
        namespace fs = std::filesystem; std::error_code ec;
        auto u8 = [](const fs::path& p) { auto q = p.u8string(); return std::string(q.begin(), q.end()); };
        const std::string dir = u8(fs::temp_directory_path()) + "\\gn_utf8_Zo\xC3\xAB_\xD0\x96_\xE6\x97\xA5", f = dir + "\\gamenights.json";
        const std::wstring good = durable::utf8_to_wide(f), old = durable::legacy_ansi_to_wide(f);
        fs::remove_all(fs::path(durable::utf8_to_wide(dir)), ec);
        CHECK(save(f, pl));   // creates the (non-ASCII) folder itself
        Plans q; CHECK(load(f, &q) && q.v.size() == 2 && q.v[0].reminded);
        std::string before; { std::ifstream in(fs::path(good), std::ios::binary); std::stringstream ss; ss << in.rdbuf(); before = ss.str(); }
        fs::create_directories(fs::path(good + L".tmp"), ec);
        Plans one = pl; one.v.pop_back(); CHECK(!save(f, one));
        { std::ifstream in(fs::path(good), std::ios::binary); std::stringstream ss; ss << in.rdbuf(); CHECK(ss.str() == before); }
        fs::remove_all(fs::path(good + L".tmp"), ec);
        fs::remove_all(fs::path(durable::utf8_to_wide(dir)), ec);
        const fs::path olddir = fs::path(old).parent_path();
        if (old == good) std::printf("  gamenight: path is ASCII-equivalent here, legacy case n/a\n");
        else {
            fs::create_directories(olddir, ec);
            if (!fs::exists(olddir)) std::printf("  SKIP gamenight legacy migration: ANSI path not creatable\n");
            else {
                { std::ofstream o(fs::path(old), std::ios::binary); o << to_json(pl); }
                Plans m; CHECK(load(f, &m) && m.v.size() == 2 && fs::exists(fs::path(good)) && fs::exists(fs::path(old)));
                std::printf("  gamenight legacy migration: ok\n");
            }
            fs::remove_all(olddir, ec);
        }
        fs::remove_all(fs::path(durable::utf8_to_wide(dir)), ec);
    }
    CHECK(!from_json("garbage", &r));
    // ---- scheduled toast decisions (social-F15), mock scheduler: nothing real is created ----
    struct Mock : hubtoast::IScheduler {
        bool ok = true, fail = false;
        int adds = 0, cancels = 0;
        std::string last_xml, last_tag; int64_t last_fire = 0;
        bool available() const override { return ok; }
        bool schedule(const std::string& x, const std::string& t, const std::string&, int64_t f) override {
            if (fail) return false; ++adds; last_xml = x; last_tag = t; last_fire = f; return true; }
        bool cancel(const std::string&, const std::string&) override { if (fail) return false; ++cancels; return true; }
    };
    {
        Night a = n; a.id = make_id("u1", a.start_ms); a.host_name = "Mia"; a.reminded = false; a.dismissed = false; a.toast_sched = false;
        // 30 min ahead: schedule; with the setting off or no scheduler: skip
        CHECK(toast_decide(a, now, true, true, false) == ToastAct::Schedule);
        CHECK(toast_decide(a, now, false, true, false) == ToastAct::None && toast_decide(a, now, true, false, false) == ToastAct::None);
        // too close: inside the guard window RetroHub itself does the reminder
        CHECK(toast_decide(a, now + 20 * M - kToastGuardMs + 1000, true, true, false) == ToastAct::None);
        CHECK(toast_decide(a, now + 20 * M - kToastGuardMs + 1000, true, true, true) == ToastAct::Schedule);   // closing: Windows takes over
        CHECK(toast_decide(a, now + 20 * M - 5000, true, true, true) == ToastAct::None);                        // already due: no future time
        a.toast_sched = true;
        CHECK(toast_decide(a, now, true, true, false) == ToastAct::None);                                       // stays pending
        CHECK(toast_decide(a, now + 20 * M - kToastGuardMs + 1000, true, true, false) == ToastAct::Remove);     // pulled before it fires
        CHECK(toast_decide(a, now, false, true, false) == ToastAct::Remove);                                    // setting turned off
        Night d = a; d.dismissed = true; CHECK(toast_decide(d, now, true, true, false) == ToastAct::Remove);    // declined / cancelled
        Night o = a; CHECK(toast_decide(o, now + 3 * H, true, true, false) == ToastAct::Remove);                // night passed
        Night rm = a; rm.reminded = true; CHECK(toast_decide(rm, now, true, true, false) == ToastAct::Remove);

        // a game or session is up: never schedule (also at exit); removals still happen
        CHECK(toast_decide(a, now, true, true, false, true) == ToastAct::Remove);   // a.toast_sched is true here
        { Night u = a; u.toast_sched = false;
          CHECK(toast_decide(u, now, true, true, false, true) == ToastAct::None);
          CHECK(toast_decide(u, now + 20 * M - kToastGuardMs + 1000, true, true, true, true) == ToastAct::None);   // exit during a game
          CHECK(toast_decide(u, now, true, true, true, false) == ToastAct::Schedule); }

        Plans sp; sp.v.push_back(a); sp.v[0].toast_sched = false;
        Mock m;
        SyncResult r = sync_toasts(sp, now, true, m);
        CHECK(r.changed && !r.failed && m.adds == 1 && sp.v[0].toast_sched && m.last_fire == a.start_ms - kReminderMs);
        CHECK(m.last_xml.find("retrohub://night/") != std::string::npos && m.last_xml.find("Mario Kart 64") != std::string::npos);
        CHECK(m.last_tag.size() <= 24 && m.last_tag == toast_tag(a));
        r = sync_toasts(sp, now + 1000, true, m); CHECK(!r.changed && m.adds == 1 && m.cancels == 0);          // idempotent
        r = sync_toasts(sp, now + 20 * M - kToastGuardMs + 1000, true, m);
        CHECK(r.changed && m.cancels == 1 && !sp.v[0].toast_sched);                                            // RetroHub running: pulled
        r = sync_toasts(sp, now + 20 * M - kToastGuardMs + 1000, true, m, true);
        CHECK(r.changed && m.adds == 2 && sp.v[0].toast_sched);                                                // exit: handed back
        {   // exit while a game runs: nothing is scheduled, the pending flag is not touched wrongly
            Plans gp; Night g = a; g.toast_sched = false; gp.v.push_back(g);
            int before = m.adds;
            r = sync_toasts(gp, now, true, m, true, true);
            CHECK(!r.changed && m.adds == before && !gp.v[0].toast_sched);
            r = sync_toasts(gp, now, true, m, false, true); CHECK(!r.changed && m.adds == before);
            gp.v[0].toast_sched = true;   // a pending toast is pulled once a game is up
            r = sync_toasts(gp, now, true, m, false, true); CHECK(r.changed && !gp.v[0].toast_sched);
        }
        m.fail = true;
        r = sync_toasts(sp, now + 20 * M - kToastGuardMs + 1000, true, m);
        CHECK(r.failed && r.remove_failed && !r.changed && sp.v[0].toast_sched);                                                  // failed cancel keeps the flag for a retry
        m.fail = false; m.ok = false;
        r = sync_toasts(sp, now, true, m); CHECK(r.changed && !sp.v[0].toast_sched);                           // no scheduler: flag dropped, no calls
        Plans q; Night s2 = a; s2.toast_sched = true; q.v.push_back(s2);
        std::string pj = to_json(q); Plans q2; CHECK(from_json(pj, &q2) && q2.v[0].toast_sched);
        CHECK(hubtoast::build_gamenight_xml("A&B", "", "x@1").find("A&amp;B") != std::string::npos);
    }
    {   // ---- iCalendar export ----
        CHECK(ics_utc(1794513600) == "20261112T200000Z" && ics_utc(0) == "19700101T000000Z" && ics_utc(951868799) == "20000229T235959Z");
        CHECK(ics_escape("a,b;c\\d\ne") == "a\\,b\\;c\\\\d\\ne" && ics_escape("x\r\ny") == "x\\ny");
        Night g; g.id = "hostuid42@1794513600"; g.host_uid = "hostuid42"; g.host_name = "Mia"; g.game_id = "mk64"; g.game_title = "Mario Kart 64"; g.start_ms = 1794513600000LL;
        const int64_t stamp = 1794312000000LL;
        const std::string want =
            "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//RetroHub//Game night//EN\r\nCALSCALE:GREGORIAN\r\nBEGIN:VEVENT\r\n"
            "UID:" + ics_uid(g) + "\r\nDTSTAMP:20261110T120000Z\r\nDTSTART:20261112T200000Z\r\nDTEND:20261112T213000Z\r\n"
            "SUMMARY:Game night: Mario Kart 64\r\n"
            "DESCRIPTION:Game night in RetroHub: Mario Kart 64.\\nPlanned by Mia.\\nOpen R\r\n"
            " etroHub a few minutes before the start to host or join.\r\n"
            "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT15M\r\nDESCRIPTION:Game night starts in 15 minutes: Mario Kart 64\r\nEND:VALARM\r\n"
            "END:VEVENT\r\nEND:VCALENDAR\r\n";
        CHECK(make_ics(g, stamp) == want);
        // UID / file name: stable, hashed, and free of account ids
        CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" && sha256_hex("").size() == 64);
        {
            Night g2 = g; g2.invitees = {"frienduid77"}; g2.mine = true;
            const std::string u = ics_uid(g), f = ics_file_name(g);
            CHECK(u == ics_uid(g2) && f == ics_file_name(g2));                        // same night, same UID and name, whatever else changes
            CHECK(u.size() == 10 + 32 + 9 && u.compare(0, 10, "gamenight-") == 0 && u.find("@retrohub") == 42);
            CHECK(f.find("game-night-20261112-2000-") == 0 && f.size() == 25 + 8 + 4 && f.substr(f.size() - 4) == ".ics");
            Night g3 = g; g3.id = "other@1794513600";                                  // same second, different night: different UID and name
            CHECK(ics_uid(g3) != u && ics_file_name(g3) != f);
            const std::string all = make_ics(g2, stamp) + f;
            CHECK(all.find("hostuid42") == std::string::npos && all.find("frienduid77") == std::string::npos && all.find("1794513600") == std::string::npos);
            g2.mine = false; CHECK(make_ics(g2, stamp).find("frienduid77") == std::string::npos);
        }
        g.mine = true; CHECK(make_ics(g, stamp).find("Planned by") == std::string::npos);

        // commas, semicolons, non-ASCII and a very long title: every physical line <= 75 octets, CRLF only, unfolds back to the escaped text
        auto check_lines = [&](const std::string& ics) {
            bool ok = ics.size() > 2 && ics.substr(ics.size() - 2) == "\r\n";
            size_t pos = 0;
            while (pos < ics.size()) {
                size_t e = ics.find("\r\n", pos);
                if (e == std::string::npos) { ok = false; break; }
                if (e - pos > 75) ok = false;
                if (ics.substr(pos, e - pos).find('\n') != std::string::npos) ok = false;
                pos = e + 2;
            }
            return ok;
        };
        auto unfold = [](const std::string& ics) {
            std::string o;
            for (size_t i = 0; i < ics.size(); ++i) { if (ics.compare(i, 3, "\r\n ") == 0) i += 2; else o += ics[i]; }
            return o;
        };
        g.game_title = "Super Smash Bros., Melee; Ultimate \xC3\xA9\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA";   // e-acute + katakana
        std::string ics = make_ics(g, stamp);
        CHECK(check_lines(ics));
        CHECK(unfold(ics).find("SUMMARY:Game night: Super Smash Bros.\\, Melee\\; Ultimate \xC3\xA9\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA\r\n") != std::string::npos);
        std::string longt(300, 'x'); for (size_t i = 5; i < longt.size(); i += 7) longt[i] = ',';
        for (int lead = 0; lead < 4; ++lead) {   // shift the multi-byte characters across every fold position
            g.game_title = std::string(lead, 'a') + longt + "\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA";
            std::string li = make_ics(g, stamp);
            CHECK(check_lines(li));
            CHECK(unfold(li).find("SUMMARY:" + ics_escape("Game night: " + g.game_title) + "\r\n") != std::string::npos);
            // no fold lands inside a UTF-8 character: every physical line is valid on its own (continuation bytes never start a line body)
            for (size_t p = li.find("\r\n "); p != std::string::npos; p = li.find("\r\n ", p + 3)) CHECK(((unsigned char)li[p + 3] & 0xC0) != 0x80);
        }
        CHECK(ics_fold("").size() == 2 && ics_fold(std::string(75, 'a')) == std::string(75, 'a') + "\r\n");
        CHECK(ics_fold(std::string(76, 'a')) == std::string(75, 'a') + "\r\n a\r\n");
    }
    std::printf(fails ? "FAILED (%d)\n" : "gamenight tests passed\n", fails);
    return fails ? 1 : 0;
}
