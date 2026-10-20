// Console test for the updater: pure logic + a live release check (no download/install).
#include "updater.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace hubupdate;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static const char* kSample = R"({
  "url": "https://api.github.com/repos/fatty8me2/smash-classic/releases/1",
  "tag_name": "v1.8.0",
  "name": "Smash Classic 1.8.0",
  "body": "## What's new\r\n- **Faster** menus\r\n- See [the notes](https://example.com/x) for `details`\r\né café 😀\r\n```\ncode block\n```",
  "assets": [
    {"name": "BattleShip-x86_64.AppImage", "browser_download_url": "https://github.com/fatty8me2/smash-classic/releases/download/v1.8.0/BattleShip-x86_64.AppImage", "size": 5},
    {"name": "BattleShip-windows.zip", "size": 123456789, "digest": "sha256:abcdef0123",
     "browser_download_url": "https://github.com/fatty8me2/smash-classic/releases/download/v1.8.0/BattleShip-windows.zip"}
  ]
})";

int main() {
    using namespace hubupdate::detail;
    // --- versions ---
    CHECK(compare_versions("v1.8.0", "v1.7.0") == 1);
    CHECK(compare_versions("v1.7.0", "1.7.0") == 0);
    CHECK(compare_versions("1.7", "v1.7.0") == 0);
    CHECK(compare_versions("v1.10.0", "v1.9.9") == 1);
    CHECK(compare_versions("v1.2.3", "v1.2.4") == -1);
    CHECK(compare_versions("dev", "v0.0.1") == -1);
    CHECK(compare_versions("v2.0.0", "dev") == 1);
    CHECK(compare_versions("dev", "dev") == 0);

    // --- release JSON ---
    Release r = parse_release(kSample);
    CHECK(r.ok && r.tag == "v1.8.0");
    CHECK(r.has_asset && r.size == 123456789 && r.digest == "sha256:abcdef0123");
    CHECK(r.url == "https://github.com/fatty8me2/smash-classic/releases/download/v1.8.0/BattleShip-windows.zip");
    CHECK(r.body.find("\xC3\xA9") != std::string::npos);         // é decoded
    CHECK(r.body.find("\xF0\x9F\x98\x80") != std::string::npos); // surrogate pair decoded
    CHECK(!parse_release("not json").ok);
    CHECK(!parse_release("{\"body\":\"x\"}").ok);
    CHECK(!parse_release("{\"tag_name\":\"v1\",\"assets\":[").ok);
    // wrong host => asset rejected, release still parsed
    Release bad = parse_release(R"({"tag_name":"v9.0.0","assets":[{"name":"BattleShip-windows.zip","size":10,
        "browser_download_url":"https://evil.example.com/BattleShip-windows.zip"}]})");
    CHECK(bad.ok && !bad.has_asset);
    Release zero = parse_release(R"({"tag_name":"v9.0.0","assets":[{"name":"BattleShip-windows.zip","size":0,
        "browser_download_url":"https://github.com/fatty8me2/smash-classic/releases/download/v9/BattleShip-windows.zip"}]})");
    CHECK(zero.ok && !zero.has_asset);

    // --- trust ---
    const std::string P = "https://github.com/fatty8me2/smash-classic/releases/download/";
    CHECK(is_trusted_download_url(P + "v1.8.0/BattleShip-windows.zip"));
    CHECK(!is_trusted_download_url("http://github.com/fatty8me2/smash-classic/releases/download/v1/x.zip"));
    CHECK(!is_trusted_download_url("https://github.com/other/repo/releases/download/v1/x.zip"));
    CHECK(!is_trusted_download_url(P + "v1/x.zip\" & calc"));
    CHECK(!is_trusted_download_url(P + "v1/a b.zip"));
    CHECK(!is_trusted_download_url(P + "v1/x'.zip"));
    CHECK(!is_trusted_download_url(""));

    // --- markdown ---
    std::string md = strip_markdown(r.body);
    CHECK(md.find("What's new") == 0);
    CHECK(md.find("- Faster menus") != std::string::npos);
    CHECK(md.find("See the notes for details") != std::string::npos);
    CHECK(md.find('*') == std::string::npos && md.find('`') == std::string::npos && md.find('#') == std::string::npos);
    CHECK(md.find("code block") != std::string::npos && md.find("```") == std::string::npos);

    std::printf("current_version=%s launch_check_disabled=%d\n", current_version().c_str(), (int)launch_check_disabled());
    std::printf("unit checks: %s\n", g_fail ? "FAILED" : "all passed");

    // --- live check (no download) ---
    check_async(true);
    for (int i = 0; i < 300 && (checking() || (!update_available() && !check_failed() && status().empty())); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("live: latest_version=%s available=%d failed=%d status=\"%s\"\n",
                latest_version().c_str(), (int)update_available(), (int)check_failed(), status().c_str());
    std::printf("live: notes (%zu chars): %.120s\n", release_notes().size(), release_notes().c_str());
    if (latest_version().empty()) { std::printf("FAIL: live check returned no version\n"); g_fail++; }
    return g_fail ? 1 : 0;
}
