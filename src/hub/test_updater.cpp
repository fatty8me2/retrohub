// Console test for the updater: pure logic + a live release check (no download/install).
#include "updater.h"

#include <process.h>
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


    // --- eng-2.1: the RetroHub-only payload (RetroHub-windows.zip) ---
    {
        const std::string RP = "https://github.com/fatty8me2/retrohub/releases/download/v1.40.0/";
        const std::string HUBMARK = "# retrohub-only\n";
        Release both = parse_release(R"({"tag_name":"v1.40.0","assets":[
            {"name":"SHA256SUMS.txt","size":10,"browser_download_url":"https://github.com/fatty8me2/retrohub/releases/download/v1.40.0/SHA256SUMS.txt"},
            {"name":"BattleShip-windows.zip","size":500,"digest":"sha256:aa","browser_download_url":"https://github.com/fatty8me2/retrohub/releases/download/v1.40.0/BattleShip-windows.zip"},
            {"name":"RetroHub-windows.zip","size":50,"digest":"sha256:bb","browser_download_url":"https://github.com/fatty8me2/retrohub/releases/download/v1.40.0/RetroHub-windows.zip"}]})");
        CHECK(both.ok && both.has_asset && both.has_hub_asset);
        CHECK(both.url == RP + "BattleShip-windows.zip" && both.size == 500 && both.digest == "sha256:aa");
        CHECK(both.hub_url == RP + "RetroHub-windows.zip" && both.hub_size == 50 && both.hub_digest == "sha256:bb");
        Release hubOnly = parse_release(R"({"tag_name":"v1.40.0","assets":[
            {"name":"RetroHub-windows.zip","size":50,"browser_download_url":"https://github.com/fatty8me2/retrohub/releases/download/v1.40.0/RetroHub-windows.zip"}]})");
        CHECK(hubOnly.ok && !hubOnly.has_asset && hubOnly.has_hub_asset);
        Release evil = parse_release(R"({"tag_name":"v1.40.0","assets":[
            {"name":"RetroHub-windows.zip","size":50,"browser_download_url":"https://evil.example.com/RetroHub-windows.zip"},
            {"name":"BattleShip-windows.zip","size":5,"browser_download_url":"https://github.com/fatty8me2/retrohub/releases/download/v1.40.0/BattleShip-windows.zip"}]})");
        CHECK(!evil.has_hub_asset && evil.has_asset);                                        // untrusted host: the hub zip is ignored
        Release zero = parse_release(R"({"tag_name":"v1.40.0","assets":[
            {"name":"RetroHub-windows.zip","size":0,"browser_download_url":"https://github.com/fatty8me2/retrohub/releases/download/v1.40.0/RetroHub-windows.zip"}]})");
        CHECK(!zero.has_hub_asset);
        CHECK(!parse_release(kSample).has_hub_asset && parse_release(kSample).has_asset);   // an old-style release: exactly as before

        // which payload: the full zip unless the release says "hub-only" (or has nothing else)
        const std::string H = std::string(64, 'a');
        const std::string plain = H + "  BattleShip-windows.zip\n" + H + "  RetroHub-windows.zip\n";
        CHECK(!sums_mark_hub_only(plain) && sums_mark_hub_only(HUBMARK + plain) && sums_mark_hub_only(plain + HUBMARK));
        CHECK(sums_mark_hub_only("\xEF\xBB\xBF# retrohub-only\r\n" + plain) && !sums_mark_hub_only("# retrohub-only-not\n" + plain) && !sums_mark_hub_only(""));
        CHECK(choose_payload(both, plain) == Payload::Full);                                  // both zips, no marker: Smash Classic's files are updated too
        CHECK(choose_payload(both, HUBMARK + plain) == Payload::HubOnly);
        CHECK(choose_payload(both, "") == Payload::Full);
        CHECK(choose_payload(hubOnly, plain) == Payload::HubOnly);                            // the only payload there is
        CHECK(choose_payload(parse_release(kSample), plain) == Payload::Full);
        CHECK(choose_payload(Release(), plain) == Payload::None);
        CHECK(sums_are_trusted("v1.40.0", plain, nullptr));                                   // the M2 hook (no signing yet)

        // each zip is verified under its own name; a missing SHA line or a wrong hash fails
        const std::string A = std::string(64, 'a'), B = std::string(64, 'b');
        const std::string sums2 = A + "  BattleShip-windows.zip\n" + B + "  RetroHub-windows.zip\n";
        CHECK(verify_hashes("v1.40.0", B, &sums2, "", kHubAssetName) == Verdict::Ok);
        CHECK(verify_hashes("v1.40.0", A, &sums2, "", kHubAssetName) == Verdict::ZipMismatch);   // the full zip's hash is not the hub zip's
        CHECK(verify_hashes("v1.40.0", A, &sums2, "") == Verdict::Ok);                           // default name unchanged
        const std::string noHub = A + "  BattleShip-windows.zip\n";
        CHECK(verify_hashes("v1.40.0", B, &noHub, "", kHubAssetName) == Verdict::BadSums);       // missing SHA line
        const std::string dup = B + "  RetroHub-windows.zip\n" + A + "  RetroHub-windows.zip\n";
        CHECK(verify_hashes("v1.40.0", B, &dup, "", kHubAssetName) == Verdict::BadSums);         // poisoned (two different hashes)
        CHECK(verify_hashes("v1.40.0", B, nullptr, "", kHubAssetName) == Verdict::NoSums);
    }

    // --- H1: the hand-off to the installer waits for the game(s) to close (pure state machine, time injected) ---
    {
        Gate g;
        const unsigned long long T = 100000;
        CHECK(gate_step(g, false, false, false, T) == GateState::NotReady);              // nothing downloaded yet
        CHECK(gate_step(g, true, true, false, T) == GateState::Held);                    // a session runs (A.proc, GameCube NetPlay, 3DS room, remote play, Kanto)
        CHECK(gate_step(g, true, false, true, T + 1000) == GateState::Held);             // a game from an earlier RetroHub run is still running from the install folder
        CHECK(gate_step(g, true, true, true, T + 2000) == GateState::Held);
        // the game closes: nothing starts at once, the gate settles first (post-game save work, a game that restarts)
        CHECK(gate_step(g, true, false, false, T + 5000) == GateState::Settling);
        CHECK(gate_step(g, true, false, false, T + 5000 + kSettleMs - 1) == GateState::Settling);
        // ... and a new game starting during the settle time starts the wait over
        CHECK(gate_step(g, true, true, false, T + 7000) == GateState::Held);
        CHECK(gate_step(g, true, false, false, T + 8000) == GateState::Settling);
        CHECK(gate_step(g, true, false, false, T + 8000 + kSettleMs) == GateState::Go);  // the hand-off starts, exactly once
        CHECK(gate_step(g, true, false, false, T + 20000) == GateState::Done);
        CHECK(gate_step(g, true, true, false, T + 30000) == GateState::Done);            // a game after the hand-off began changes nothing here
        // a failed hand-off (ready goes false again) re-arms the gate for the retry
        CHECK(gate_step(g, false, false, false, T + 40000) == GateState::NotReady);
        CHECK(gate_step(g, true, false, false, T + 41000) == GateState::Settling);
        // nothing running from the very first tick still waits out the settle time, then goes
        Gate h;
        CHECK(gate_step(h, true, false, false, 0) == GateState::Settling);
        CHECK(gate_step(h, true, false, false, kSettleMs) == GateState::Go);
        // a game that never ends never lets it go, however long it takes
        Gate k;
        bool never_went = true;
        for (unsigned long long t = 0; t < 6ull * 3600 * 1000; t += 500) if (gate_step(k, true, true, false, t) != GateState::Held) never_went = false;
        CHECK(never_went && !k.fired);
        CHECK(std::string(kReadyText) == "Update ready: installs when you finish playing");   // About page / update popup status
        CHECK(std::string(kReadyBellText) == "Update ready: installs after your game" && std::strlen(kReadyBellText) <= 40);   // fits the bell uncut
    }

    // --- M1: staged files are kept for a retry, and reused only when they are exactly what was unpacked ---
    {
        std::filesystem::path d = std::filesystem::temp_directory_path() / ("hub_updater_staged_test_" + std::to_string(_getpid()));
        std::error_code ec; std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d / "staged" / "sub", ec);
        auto put = [](const std::filesystem::path& p, const std::string& s) { std::ofstream o(p, std::ios::binary | std::ios::trunc); o << s; };
        put(d / "staged" / "RetroHub.exe", "new-hub");
        put(d / "staged" / "BattleShip.exe", "new-bs");
        put(d / "staged" / "sub" / "x.dat", std::string(100, 'x'));
        const std::string ud = d.u8string();
        CHECK(!staged_reusable(ud, "v1.30.0"));                                 // no manifest yet
        StagedInfo si = scan_staged(ud + "\\staged");
        CHECK(si.files == 3 && si.bytes == 7 + 6 + 100);
        CHECK(write_staged_manifest(ud, "v1.30.0") && exists(ud + "\\staged.json"));
        CHECK(staged_reusable(ud, "v1.30.0"));                                  // same release: no download needed
        CHECK(!staged_reusable(ud, "v1.31.0") && !staged_reusable(ud, ""));     // another release (or none): download
        put(d / "staged" / "sub" / "x.dat", std::string(101, 'x'));             // anything changed since the unpack: download again
        CHECK(!staged_reusable(ud, "v1.30.0"));
        put(d / "staged" / "sub" / "x.dat", std::string(100, 'x'));
        CHECK(staged_reusable(ud, "v1.30.0"));
        std::filesystem::remove(d / "staged" / "sub" / "x.dat", ec);            // ... or a file went missing
        CHECK(!staged_reusable(ud, "v1.30.0"));
        put(d / "staged" / "sub" / "x.dat", std::string(100, 'x'));
        std::filesystem::remove(d / "staged" / "BattleShip.exe", ec);           // a required exe missing, even if the counts were fixed up
        CHECK(!staged_reusable(ud, "v1.30.0"));
        StagedInfo pi;
        si.version = "v1.30.0";
        CHECK(parse_staged_manifest(staged_manifest_json(si), &pi) && pi.version == "v1.30.0" && pi.files == 3 && pi.bytes == 113);
        CHECK(!parse_staged_manifest("not json", &pi) && !parse_staged_manifest("{\"version\":\"\",\"files\":3,\"bytes\":1}", &pi) &&
              !parse_staged_manifest("{\"version\":\"v1\",\"files\":0,\"bytes\":0}", &pi));
        // a hub-only payload: no BattleShip.exe needed, and the kind round-trips through the manifest
        put(d / "staged" / "sub" / "x.dat", std::string(100, 'x'));
        bool kind = true;
        CHECK(write_staged_manifest(ud, "v1.40.0", true));
        CHECK(staged_reusable(ud, "v1.40.0", &kind) && kind);
        CHECK(write_staged_manifest(ud, "v1.40.0", false));
        CHECK(!staged_reusable(ud, "v1.40.0", &kind));                                  // the same files recorded as a full payload need BattleShip.exe
        StagedInfo hi = scan_staged(ud + "\\staged"); hi.version = "v1.40.0"; hi.hub_only = true;
        StagedInfo hp;
        CHECK(parse_staged_manifest(staged_manifest_json(hi), &hp) && hp.hub_only);
        hi.hub_only = false;
        CHECK(parse_staged_manifest(staged_manifest_json(hi), &hp) && !hp.hub_only && staged_manifest_json(hi).find("payload") == std::string::npos);   // older manifests stay as they were
        std::filesystem::remove_all(d, ec);
    }

    // --- integration dry run: download_verified against a local server (127.0.0.1) ---
    {
        std::string zipBody(5000, '\0');
        for (size_t i = 0; i < zipBody.size(); i++) zipBody[i] = (char)(i * 31 + 7);
        std::filesystem::path dir = std::filesystem::temp_directory_path() / ("hub_updater_verify_test_" + std::to_string(_getpid()));
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
        // the RetroHub-only zip: verified under its own name; tampered body, missing line, sums_text preset
        srv.routes["/RetroHub-windows.zip"] = zipBody;
        srv.routes["/hubgood.txt"] = std::string(64, '2') + "  BattleShip-windows.zip\n" + real + "  RetroHub-windows.zip\n";
        srv.routes["/hubmissing.txt"] = real + "  BattleShip-windows.zip\n";
        srv.routes["/hubbad.txt"] = real + "  BattleShip-windows.zip\n" + std::string(64, '0') + "  RetroHub-windows.zip\n";
        f.zip_url = base + "RetroHub-windows.zip"; f.asset_name = kHubAssetName;
        CHECK(run("v1.40.0", "hubgood.txt", "", N) && exists(zipPath));                // hub zip matches ITS line (the full zip's line differs)
        CHECK(!run("v1.40.0", "hubmissing.txt", "", N) && !exists(zipPath));           // no SHA line for the hub zip: refused, file deleted
        CHECK(!run("v1.40.0", "hubbad.txt", "", N) && !exists(zipPath) && err.find("checksum") != std::string::npos);   // tampered: refused, file deleted
        CHECK(!run("v1.40.0", "good.txt", "", N) && !exists(zipPath));                 // sums that only list the full zip
        f.sums_text = std::string(64, '2') + "  BattleShip-windows.zip\n" + real + "  RetroHub-windows.zip\n";
        CHECK(run("v1.40.0", "", "", N) && exists(zipPath));                           // sums already fetched (and trusted): no second fetch
        f.sums_text = real + "  BattleShip-windows.zip\n";
        CHECK(!run("v1.40.0", "", "", N) && !exists(zipPath));
        f.sums_text.clear(); f.asset_name.clear(); f.zip_url = base + "BattleShip-windows.zip";
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
