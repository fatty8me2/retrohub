// Console test: toast XML builder, activation parsing and the show/skip policy with a mock notifier (no real toasts).
#include "toast.h"
#include "popup_guard.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

struct Mock : hubtoast::INotifier {
    bool avail = true;
    int shown = 0;
    std::string last_xml, last_tag;
    bool available() const override { return avail; }
    bool show(const std::string& x, const std::string& t) override { ++shown; last_xml = x; last_tag = t; return true; }
};

int main() {
    using namespace hubtoast;
    CHECK(xml_escape("a<b>&\"'") == "a&lt;b&gt;&amp;&quot;&apos;");
    std::string x = build_invite_xml("Ann <3>", "Super & Smash", "ABC234");
    CHECK(x.find("Ann &lt;3&gt; invited you to Super &amp; Smash") != std::string::npos);
    CHECK(x.find("content=\"Join\" arguments=\"retrohub://join/ABC234\" activationType=\"protocol\"") != std::string::npos);
    CHECK(x.find("launch=\"retrohub://join/ABC234\"") != std::string::npos);
    CHECK(build_invite_xml("A", "B", "ABC10O").empty());
    CHECK(build_invite_xml("A", "B", "").empty());
    CHECK(build_invite_xml("", "", "ABC234").find("A friend invited you to a game") != std::string::npos);

    /* quiet toasts: default scenario only (incomingCall / alarm / reminder can take priority), silent audio */
    CHECK(x.find("scenario=\"default\"") != std::string::npos && x.find("incomingCall") == std::string::npos &&
          x.find("alarm") == std::string::npos && x.find("reminder") == std::string::npos && x.find("<audio silent=\"true\"/>") != std::string::npos);
    CHECK(build_online_xml("Mia").find("scenario=\"default\"") != std::string::npos && build_online_xml("Mia").find("<audio silent=\"true\"/>") != std::string::npos);

    std::string c;
    CHECK(parse_activation_code("retrohub://join/ABC234", &c) && c == "ABC234");
    CHECK(parse_activation_code("RetroHub://join/abc234/", &c) && c == "ABC234");
    CHECK(!parse_activation_code("retrohub://join/ABC23", &c));
    CHECK(!parse_activation_code("retrohub://join/ABC2341", &c));
    CHECK(!parse_activation_code("https://evil/ABC234", &c));
    CHECK(!parse_activation_code("", &c));

    /* Plan 4 (SO-F14, owner decision): NO Windows toast while a game runs, even when the player opted in. These toasts only ever existed
     * for the in-game case, so the one gate (should_toast) is false for every input; the bell and the runner OSD carry the message. */
    for (int s = 0; s < 2; ++s)
        for (int g = 0; g < 2; ++g)
            for (int a = 0; a < 2; ++a)
                CHECK(!should_toast(s != 0, g != 0, a != 0));
    CHECK(!should_toast(true, true, true));          // opted in, game running, notifier available: still no
    CHECK(!hubguard::may_windows_toast(true) && hubguard::may_windows_toast(false));   // the policy the gate asks

    Mock m;
    CHECK(!notify_invite(m, true, true, "Bob", "Smash 64", "ZXCV23") && m.shown == 0);    // opted in, game running: nothing reaches the notifier
    CHECK(!notify_invite(m, false, true, "Bob", "Smash 64", "ZXCV23") && m.shown == 0);   // setting off
    CHECK(!notify_invite(m, true, false, "Bob", "Smash 64", "ZXCV23") && m.shown == 0);   // no game running: the hub shows its own toast
    m.avail = false;
    CHECK(!notify_invite(m, true, true, "Bob", "Smash 64", "ZXCV23") && m.shown == 0);    // dev build
    m.avail = true;
    CHECK(!notify_invite(m, true, true, "Bob", "Smash 64", "bad") && m.shown == 0);       // invalid code

    /* "came online" toast: same gate as the invite toast */
    {
        std::string ox = build_online_xml("Mia <3>");
        CHECK(ox.find("Mia &lt;3&gt; came online") != std::string::npos && ox.find("<action") == std::string::npos);
        CHECK(build_online_xml("").find("A friend came online") != std::string::npos);
        Mock o;
        CHECK(!notify_online(o, true, true, "Mia") && o.shown == 0);    // opted in, game running: no Windows toast (bell entry + in-hub toast only)
        CHECK(!notify_online(o, false, true, "Mia") && o.shown == 0);   // in-game notifications off
        CHECK(!notify_online(o, true, false, "Mia") && o.shown == 0);   // no game running: the hub shows its own toast
        o.avail = false;
        CHECK(!notify_online(o, true, true, "Mia") && o.shown == 0);    // dev build
    }

    std::printf(fails ? "hub_toast_test FAILED\n" : "hub_toast_test OK\n");
    return fails ? 1 : 0;
}
