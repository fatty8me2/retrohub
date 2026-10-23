// Console test: toast XML builder, activation parsing and the show/skip policy with a mock notifier (no real toasts).
#include "toast.h"

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

    std::string c;
    CHECK(parse_activation_code("retrohub://join/ABC234", &c) && c == "ABC234");
    CHECK(parse_activation_code("RetroHub://join/abc234/", &c) && c == "ABC234");
    CHECK(!parse_activation_code("retrohub://join/ABC23", &c));
    CHECK(!parse_activation_code("retrohub://join/ABC2341", &c));
    CHECK(!parse_activation_code("https://evil/ABC234", &c));
    CHECK(!parse_activation_code("", &c));

    CHECK(should_toast(true, true, true));
    CHECK(!should_toast(false, true, true));
    CHECK(!should_toast(true, false, true));
    CHECK(!should_toast(true, true, false));

    Mock m;
    CHECK(notify_invite(m, true, true, "Bob", "Smash 64", "ZXCV23") && m.shown == 1 && m.last_tag == "ZXCV23");
    CHECK(!notify_invite(m, false, true, "Bob", "Smash 64", "ZXCV23") && m.shown == 1);   // setting off
    CHECK(!notify_invite(m, true, false, "Bob", "Smash 64", "ZXCV23") && m.shown == 1);   // no game running
    m.avail = false;
    CHECK(!notify_invite(m, true, true, "Bob", "Smash 64", "ZXCV23") && m.shown == 1);    // dev build
    m.avail = true;
    CHECK(!notify_invite(m, true, true, "Bob", "Smash 64", "bad") && m.shown == 1);       // invalid code

    std::printf(fails ? "hub_toast_test FAILED\n" : "hub_toast_test OK\n");
    return fails ? 1 : 0;
}
