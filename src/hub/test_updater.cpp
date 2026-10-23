// Console test for the updater: pure logic + a live release check (no download/install).
#include "updater.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>
#include <atomic>
#include <fstream>
#include <map>
#include <filesystem>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

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

// ---- tiny 127.0.0.1-only HTTP server serving a fake release (path -> body); 404 otherwise ----
struct FakeServer {
    SOCKET ls = INVALID_SOCKET;
    int port = 0;
    std::map<std::string, std::string> routes;
    std::atomic<bool> stop{false};
    std::thread th;
    bool start() {
        WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = 0;
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);   // loopback only
        if (bind(ls, (sockaddr*)&a, sizeof(a)) != 0 || listen(ls, 8) != 0) return false;
        int len = sizeof(a); getsockname(ls, (sockaddr*)&a, &len); port = ntohs(a.sin_port);
        th = std::thread([this] {
            while (!stop.load()) {
                fd_set fds; FD_ZERO(&fds); FD_SET(ls, &fds); timeval tv{0, 100000};
                if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
                SOCKET c = accept(ls, nullptr, nullptr);
                if (c == INVALID_SOCKET) continue;
                std::string req; char buf[2048];
                while (req.find("\r\n\r\n") == std::string::npos) {
                    int n = recv(c, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    req.append(buf, n);
                }
                std::string path;
                size_t sp = req.find(' '), e = req.find(' ', sp + 1);
                if (sp != std::string::npos && e != std::string::npos) path = req.substr(sp + 1, e - sp - 1);
                auto it = routes.find(path);
                std::string out;
                if (it == routes.end()) out = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                else out = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(it->second.size()) + "\r\nConnection: close\r\n\r\n" + it->second;
                for (size_t off = 0; off < out.size();) { int n = send(c, out.data() + off, (int)(out.size() - off), 0); if (n <= 0) break; off += n; }
                shutdown(c, SD_SEND); closesocket(c);
            }
        });
        return true;
    }
    ~FakeServer() { stop = true; if (th.joinable()) th.join(); if (ls != INVALID_SOCKET) closesocket(ls); }
};

static bool exists(const std::string& p) { return std::filesystem::exists(std::filesystem::u8path(p)); }

int main(int argc, char** argv) {
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

    // --- SHA256SUMS parsing ---
    const std::string H1(64, 'a'), H2 = std::string(63, 'b') + "C";
    auto sm = parse_sha256sums("\xEF\xBB\xBF" + H1 + "  BattleShip-windows.zip\r\n" + H2 + " *RetroHub-Setup.exe\n# junk\nzz  nope\n" + H1 + "\n");
    CHECK(sm.size() == 2 && sm["BattleShip-windows.zip"] == H1);
    CHECK(sm["RetroHub-Setup.exe"] == std::string(63, 'b') + "c");               // lowercased
    auto dup = parse_sha256sums(H1 + "  x.zip\n" + std::string(64, 'c') + "  x.zip\n");
    CHECK(dup.count("x.zip") == 1 && dup["x.zip"].empty());                        // conflicting duplicates poisoned
    CHECK(parse_sha256sums("").empty());

    // --- version gating ---
    CHECK(!checksums_required("v1.17.0") && !checksums_required("v1.16.0") && !checksums_required("v1.9.9"));
    CHECK(checksums_required("v1.17.1") && checksums_required("v1.18.0") && checksums_required("v2.0.0"));

    // --- verify_hashes ---
    const std::string S = H1 + "  BattleShip-windows.zip\n";
    CHECK(verify_hashes("v1.18.0", H1, &S, "") == Verdict::Ok);
    CHECK(verify_hashes("v1.18.0", std::string(64, 'e'), &S, "") == Verdict::ZipMismatch);      // mismatch refused
    CHECK(verify_hashes("v1.18.0", H1, nullptr, "") == Verdict::NoSums);                       // missing refused (new release)
    CHECK(verify_hashes("v1.17.0", H1, nullptr, "") == Verdict::OkLegacy);                     // legacy release tolerated
    CHECK(verify_hashes("v1.17.0", H1, nullptr, "sha256:" + std::string(64, 'd')) == Verdict::DigestMismatch);
    CHECK(verify_hashes("v1.17.0", H1, nullptr, "sha256:" + H1) == Verdict::OkLegacy);
    CHECK(verify_hashes("v1.18.0", H1, &S, "sha256:" + std::string(64, 'd')) == Verdict::DigestMismatch);   // digest checked even when sums agree
    CHECK(verify_hashes("v1.18.0", H1, &S, "sha256:" + H1) == Verdict::Ok);
    std::string other = H1 + "  RetroHub-Setup.exe\n";
    CHECK(verify_hashes("v1.18.0", H1, &other, "") == Verdict::BadSums);                       // zip not listed
    CHECK(verify_hashes("v1.18.0", "", &S, "") == Verdict::BadSums);                            // hashing failed
    // sums asset parsed from the API JSON
    Release rs = parse_release(R"({"tag_name":"v1.18.0","assets":[
        {"name":"SHA256SUMS.txt","size":10,"browser_download_url":"https://github.com/fatty8me2/smash-classic/releases/download/v1.18.0/SHA256SUMS.txt"},
        {"name":"BattleShip-windows.zip","size":50,"browser_download_url":"https://github.com/fatty8me2/smash-classic/releases/download/v1.18.0/BattleShip-windows.zip"}]})");
    CHECK(rs.has_asset && rs.sums_url.size() && rs.sums_url.find("SHA256SUMS.txt") != std::string::npos);
    CHECK(parse_release(R"({"tag_name":"v1.18.0","assets":[{"name":"SHA256SUMS.txt","size":1,"browser_download_url":"https://evil.example/SHA256SUMS.txt"}]})").sums_url.empty());

    // --- integration dry run: download_verified against a local server (127.0.0.1) ---
    {
        std::string zipBody(5000, '\0');
        for (size_t i = 0; i < zipBody.size(); i++) zipBody[i] = (char)(i * 31 + 7);
        std::filesystem::path dir = std::filesystem::temp_directory_path() / "hub_updater_verify_test";
        std::error_code ec; std::filesystem::remove_all(dir, ec); std::filesystem::create_directories(dir, ec);
        std::string zipPath = (dir / "update.zip").u8string();
        std::string refFile = (dir / "ref.bin").u8string();
        { std::ofstream o(std::filesystem::u8path(refFile), std::ios::binary); o.write(zipBody.data(), zipBody.size()); }
        std::string real = sha256_file_hex(refFile);
        CHECK(real.size() == 64);
        FakeServer srv;
        CHECK(srv.start());
        std::string base = "http://127.0.0.1:" + std::to_string(srv.port) + "/";
        srv.routes["/BattleShip-windows.zip"] = zipBody;
        srv.routes["/good.txt"] = real + "  BattleShip-windows.zip\n" + std::string(64, '1') + "  RetroHub-Setup.exe\n";
        srv.routes["/bad.txt"] = std::string(64, '0') + "  BattleShip-windows.zip\n";
        srv.routes["/other.txt"] = real + "  RetroHub-Setup.exe\n";
        FetchSpec f; f.zip_url = base + "BattleShip-windows.zip"; f.size = (long long)zipBody.size();
        std::string err;
        auto run = [&](const char* tag, const std::string& sums, const std::string& digest, long long size) {
            f.tag = tag; f.sums_url = sums.empty() ? "" : base + sums; f.digest = digest; f.size = size; err.clear();
            return download_verified(f, zipPath, &err, [](const std::string&) {});
        };
        const long long N = (long long)zipBody.size();
        CHECK(run("v1.18.0", "good.txt", "", N) && exists(zipPath));                   // verified, left for staging
        CHECK(run("v1.18.0", "good.txt", "sha256:" + real, N));
        CHECK(!run("v1.18.0", "good.txt", "sha256:" + std::string(64, 'f'), N) && !exists(zipPath));
        CHECK(!run("v1.18.0", "bad.txt", "", N) && !exists(zipPath) && err.find("checksum") != std::string::npos);   // mismatch -> refused, zip deleted
        CHECK(!run("v1.18.0", "other.txt", "", N) && !exists(zipPath));                // zip not listed
        CHECK(!run("v1.18.0", "missing.txt", "", N) && !exists(zipPath));              // sums asset 404
        CHECK(!run("v1.18.0", "", "", N) && !exists(zipPath) && err.find("SHA256SUMS") != std::string::npos);   // new release without sums
        CHECK(run("v1.17.0", "", "", N) && exists(zipPath));                           // legacy release, no sums: allowed
        CHECK(!run("v1.17.0", "", "sha256:" + std::string(64, 'f'), N) && !exists(zipPath));   // legacy but API digest wrong
        CHECK(!run("v1.17.0", "bad.txt", "", N) && !exists(zipPath));                  // legacy WITH sums: sums enforced
        CHECK(!run("v1.18.0", "good.txt", "", N + 1) && !exists(zipPath));             // size mismatch
        std::filesystem::remove_all(dir, ec);
    }

    std::printf("current_version=%s launch_check_disabled=%d\n", current_version().c_str(), (int)launch_check_disabled());
    std::printf("unit checks: %s\n", g_fail ? "FAILED" : "all passed");

    // --offline: unit checks only (used by CTest; the live check needs GitHub).
    if (argc > 1 && std::strcmp(argv[1], "--offline") == 0) return g_fail ? 1 : 0;

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
