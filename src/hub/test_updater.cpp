// Console test for the updater: pure logic + a live release check (no download/install).
#include "updater.h"
#include "update_apply.h"

#include <process.h>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>
#include <atomic>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>
#include <map>
#include <filesystem>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

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


// ---- throwaway TEST signing keys (generated here with Windows CNG; never the publisher's key) ----
struct TestKey { BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE key = nullptr; unsigned char pub[64] = {}; bool ok = false; };
static TestKey make_test_key() {
    TestKey k;
    if (BCryptOpenAlgorithmProvider(&k.alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) return k;
    if (BCryptGenerateKeyPair(k.alg, &k.key, 256, 0) != 0 || BCryptFinalizeKeyPair(k.key, 0) != 0) return k;
    unsigned char blob[8 + 64]; ULONG got = 0;
    if (BCryptExportKey(k.key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &got, 0) != 0 || got != sizeof(blob)) return k;
    std::memcpy(k.pub, blob + 8, 64);
    k.ok = true;
    return k;
}
static std::string der_int(const unsigned char* v32) {
    size_t at = 0;
    while (at < 31 && v32[at] == 0) at++;
    std::string body;
    if (v32[at] & 0x80) body += '\0';
    body.append(reinterpret_cast<const char*>(v32 + at), 32 - at);
    return std::string("\x02") + (char)body.size() + body;
}
static std::string sign_sums(const TestKey& k, const std::string& msg) {
    BCRYPT_ALG_HANDLE h = nullptr; unsigned char dg[32], rs[64]; ULONG n = 0;
    if (BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    BCryptHash(h, nullptr, 0, (PUCHAR)msg.data(), (ULONG)msg.size(), dg, 32);
    BCryptCloseAlgorithmProvider(h, 0);
    if (BCryptSignHash(k.key, nullptr, dg, 32, rs, 64, &n, 0) != 0 || n != 64) return "";
    std::string body = der_int(rs) + der_int(rs + 32);
    return std::string("\x30") + (char)body.size() + body;
}
static std::string from_hex(const std::string& h) {
    std::string o;
    for (size_t i = 0; i + 1 < h.size(); i += 2) o += (char)std::stoi(h.substr(i, 2), nullptr, 16);
    return o;
}
// A signature made by `openssl dgst -sha256 -sign` (a test key made with openssl; r and s both carry the 0x00 pad) over kFixSums.
static const char* kFixPubHex = "382593ecef59e048204b7540e916a7dc3c011469fe0c6e5d1005328368b1b216161b9be71425fd0b6e270f62cbf8d54b5de9f9cbdc770c2e1664c682a9d215ef";
static const char* kFixSigHex = "3046022100a9f677931ae6e108a8eb66c20b1cd95c7b94e6e164914155c630db92976093be022100cc4a40f12b80bf96a5c3793427de66d46c82cd7d1b884d1efeed1abfa7b85c1b";
static const std::string kFixSums = "# retrohub-only\n" + std::string(64, 'a') + "  RetroHub-windows.zip\n";

// ---- tiny 127.0.0.1-only HTTP server serving a fake release (path -> body); 404 otherwise ----
struct FakeServer {
    SOCKET ls = INVALID_SOCKET;
    int port = 0;
    std::map<std::string, std::string> routes;
    std::atomic<bool> stop{false};
    std::atomic<long long> last_range{-1};   // start offset of the last request's Range header (-1: none)
    std::atomic<bool> ignore_range{false};   // answer a ranged request with the whole file (a server that cannot resume)
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
                long long rangeFrom = -1;   // "Range: bytes=N-" (what curl -C - sends)
                { std::string low = req; for (char& ch : low) ch = (char)tolower((unsigned char)ch);
                  size_t rp = low.find("range: bytes="); if (rp != std::string::npos) rangeFrom = std::atoll(low.c_str() + rp + 13); }
                last_range.store(rangeFrom);
                if (it == routes.end()) out = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                else if (rangeFrom >= 0 && !ignore_range && (size_t)rangeFrom < it->second.size())
                    out = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(rangeFrom) + "-" + std::to_string(it->second.size() - 1) + "/" +
                          std::to_string(it->second.size()) + "\r\nContent-Length: " + std::to_string(it->second.size() - (size_t)rangeFrom) +
                          "\r\nConnection: close\r\n\r\n" + it->second.substr((size_t)rangeFrom);
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

    // --- curl.exe / tar.exe are resolved to the Windows system folder, never searched for ---
    {
        std::string sd = system_dir();
        wchar_t wbuf[MAX_PATH + 1]; UINT wn = GetSystemDirectoryW(wbuf, MAX_PATH);
        CHECK(!sd.empty() && wn > 0 && sd.size() >= 4 && sd[1] == ':' && sd.back() != '\\');   // absolute, no trailing slash
        std::string cp, tp, e;
        CHECK(find_system_tool("curl.exe", &cp, &e) && cp == sd + "\\curl.exe");              // Windows 10 1803+ ships both
        CHECK(find_system_tool("tar.exe", &tp, &e) && tp == sd + "\\tar.exe");
        // the folder the exe runs from is never consulted: a curl.exe planted in a scratch "install folder" is not what resolves
        std::filesystem::path pd = std::filesystem::temp_directory_path() / ("hub_updater_sys_test_" + std::to_string(_getpid()));
        std::error_code pec; std::filesystem::remove_all(pd, pec); std::filesystem::create_directories(pd, pec);
        std::string pds = pd.u8string();
        std::string out = "untouched";
        e.clear();
        CHECK(!resolve_system_tool(pds, "curl.exe", &out, &e) && out == "untouched" && e.find("curl.exe") != std::string::npos);   // missing: a clear message
        CHECK(e.find("not found") != std::string::npos && e.find(pds) != std::string::npos);
        { std::ofstream o(pd / "curl.exe", std::ios::binary); o << "x"; }
        CHECK(resolve_system_tool(pds, "curl.exe", &out, &e) && out == pds + "\\curl.exe");
        CHECK(resolve_system_tool(pds + "\\", "curl.exe", &out, &e) && out == pds + "\\curl.exe");   // trailing slash tolerated
        std::filesystem::create_directories(pd / "tar.exe", pec);
        CHECK(!resolve_system_tool(pds, "tar.exe", &out, &e));                                       // a folder is not a tool
        CHECK(!resolve_system_tool("", "curl.exe", &out, &e) && !resolve_system_tool(pds, "", &out, &e));
        std::filesystem::remove_all(pd, pec);
    }

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



    // --- shell-M2: release signature (ECDSA P-256 / SHA-256 over SHA256SUMS.txt, DER) ---
    TestKey tk = make_test_key(), otherKey = make_test_key();
    CHECK(tk.ok && otherKey.ok);
    {
        unsigned char fixPub[64]; { std::string pb = from_hex(kFixPubHex); CHECK(pb.size() == 64); std::memcpy(fixPub, pb.data(), 64); }
        const std::string fixSig = from_hex(kFixSigHex);
        CHECK(fixSig.size() == 72);
        // the built-in publisher key is not a test key, and is a plausible 64-byte value
        CHECK(std::strlen(kReleasePubKeyHex) == 128 && std::string(kReleasePubKeyHex) != kFixPubHex &&
              from_hex(kReleasePubKeyHex) != std::string(reinterpret_cast<const char*>(tk.pub), 64));
        // valid: a real openssl signature (interop), and one made by CNG here
        CHECK(verify_ecdsa_p256(fixPub, kFixSums.data(), kFixSums.size(), fixSig.data(), fixSig.size()));
        std::string mine = sign_sums(tk, kFixSums);
        CHECK(!mine.empty() && verify_ecdsa_p256(tk.pub, kFixSums.data(), kFixSums.size(), mine.data(), mine.size()));
        // SUMS tampered by one byte (flip each of a few positions); signature tampered by one byte
        for (size_t at : {size_t(0), size_t(20), kFixSums.size() - 1}) {
            std::string t = kFixSums; t[at] ^= 1;
            CHECK(!verify_ecdsa_p256(fixPub, t.data(), t.size(), fixSig.data(), fixSig.size()));
        }
        for (size_t at : {size_t(8), size_t(40), size_t(70)}) {
            std::string t = fixSig; t[at] ^= 1;
            CHECK(!verify_ecdsa_p256(fixPub, kFixSums.data(), kFixSums.size(), t.data(), t.size()));
        }
        std::string extra = kFixSums + "\n";                                                                   // an appended byte
        CHECK(!verify_ecdsa_p256(fixPub, extra.data(), extra.size(), fixSig.data(), fixSig.size()));
        // wrong key
        CHECK(!verify_ecdsa_p256(otherKey.pub, kFixSums.data(), kFixSums.size(), fixSig.data(), fixSig.size()));
        CHECK(!verify_ecdsa_p256(otherKey.pub, kFixSums.data(), kFixSums.size(), mine.data(), mine.size()));
        unsigned char junkKey[64]; std::memset(junkKey, 7, 64);                                                // not even a point on the curve
        CHECK(!verify_ecdsa_p256(junkKey, kFixSums.data(), kFixSums.size(), fixSig.data(), fixSig.size()));
        // malformed DER
        unsigned char rs[64];
        CHECK(der_to_raw_rs(fixSig.data(), fixSig.size(), rs) && rs[0] == 0xa9 && rs[32] == 0xcc && rs[31] == 0xbe && rs[63] == 0x1b);   // pad stripped, 32+32
        auto bad = [&](const std::string& d) { return !der_to_raw_rs(d.data(), d.size(), rs) && !verify_ecdsa_p256(fixPub, kFixSums.data(), kFixSums.size(), d.data(), d.size()); };
        CHECK(bad("") && bad(std::string(1, '\x30')) && bad(fixSig.substr(0, 10)) && bad(fixSig.substr(0, fixSig.size() - 1)));   // truncated
        CHECK(bad(fixSig + std::string(1, '\0')) && bad(fixSig + "ab"));                                       // extra bytes
        { std::string t = fixSig; t[1] = (char)0x49; CHECK(bad(t)); }                                          // length too big
        { std::string t = fixSig; t[1] = (char)0x45; CHECK(bad(t)); }                                          // length too small
        { std::string t = std::string("\x30\x81\x46", 3) + fixSig.substr(2); CHECK(bad(t)); }                  // overlong (long-form) length
        { std::string t = std::string("\x30\x82\x00\x46", 4) + fixSig.substr(2); CHECK(bad(t)); }
        { std::string t = fixSig; t[0] = 0x31; CHECK(bad(t)); }                                                // not a SEQUENCE
        { std::string t = fixSig; t[2] = 0x04; CHECK(bad(t)); }                                                // r is not an INTEGER
        { std::string t = fixSig; t[3] = 0x20; CHECK(bad(t)); }                                                // r length lies
        { std::string t = fixSig; t[3] = (char)0x82; CHECK(bad(t)); }                                          // r length overlong
        { std::string t = fixSig; t[37] = 0x03; CHECK(bad(t)); }                                               // s is not an INTEGER
        CHECK(!der_to_raw_rs(nullptr, 8, rs));
        CHECK(bad(std::string("\x30\x06\x02\x01\x00\x02\x01\x01", 8)));                                        // r = 0
        CHECK(bad(std::string("\x30\x06\x02\x01\x01\x02\x01\x00", 8)));                                        // s = 0
        CHECK(bad(std::string("\x30\x06\x02\x01\x80\x02\x01\x01", 8)));                                        // r negative
        CHECK(bad(std::string("\x30\x07\x02\x02\x00\x01\x02\x01\x01", 9)));                                    // r padded with a needless zero
        CHECK(bad(std::string("\x30\x06\x02\x00\x02\x01\x01\x00", 8)));                                        // empty INTEGER
        { std::string r33(33, '\x11'); r33[0] = 0x01; std::string t = std::string("\x02\x21", 2) + r33 + std::string("\x02\x01\x01", 3);
          t = std::string("\x30") + (char)t.size() + t; CHECK(bad(t)); }                                       // r wider than 32 bytes
        CHECK(der_to_raw_rs("\x30\x06\x02\x01\x01\x02\x01\x02", 8, rs) && rs[31] == 1 && rs[63] == 2 && rs[0] == 0 && rs[32] == 0);   // smallest valid one

        // the gate: sums_are_trusted uses the built-in key unless a test key is injected
        std::string why;
        CHECK(!sums_are_trusted("v1.40.0", kFixSums, std::string(fixSig), &why) && why == kUnsignedMessage);   // test signature, production key: refused
        set_pubkey_for_test(fixPub);
        CHECK(sums_are_trusted("v1.40.0", kFixSums, fixSig, nullptr));
        why.clear();
        CHECK(!sums_are_trusted("v1.40.0", kFixSums, "", &why) && why == kUnsignedMessage);                    // missing signature
        CHECK(!sums_are_trusted("v1.40.0", "", fixSig, nullptr));                                              // empty sums
        CHECK(!sums_are_trusted("v1.40.0", kFixSums, std::string("garbage"), nullptr));
        set_pubkey_for_test(nullptr);
        CHECK(std::string(kUnsignedMessage) == "This update isn't signed by RetroHub's publisher, so it wasn't installed.");
        // the release lists its signature asset
        Release sg = parse_release(R"({"tag_name":"v1.40.0","assets":[
            {"name":"SHA256SUMS.txt","size":10,"browser_download_url":"https://github.com/fatty8me2/retrohub/releases/download/v1.40.0/SHA256SUMS.txt"},
            {"name":"SHA256SUMS.txt.sig","size":71,"browser_download_url":"https://github.com/fatty8me2/retrohub/releases/download/v1.40.0/SHA256SUMS.txt.sig"}]})");
        CHECK(sg.sig_url.find("SHA256SUMS.txt.sig") != std::string::npos && sg.sums_url.size() && sg.sums_url.find(".sig") == std::string::npos);
        CHECK(parse_release(R"({"tag_name":"v1.40.0","assets":[{"name":"SHA256SUMS.txt.sig","size":71,"browser_download_url":"https://evil.example/SHA256SUMS.txt.sig"}]})").sig_url.empty());
    }

    // --- eng-2.1: the RetroHub-only payload (RetroHub-windows.zip) ---
    {
        const std::string RP = "https://github.com/fatty8me2/retrohub/releases/download/v1.40.0/";
        const std::string BSH = std::string(64, 'c');   // sha256 of the BattleShip.exe the hub-only release assumes
        const std::string HUBMARK = "# retrohub-only smash-sha256 " + BSH + " smash-since v1.39.0\n";
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
        CHECK(!parse_hub_only_marker(plain).valid && parse_hub_only_marker(HUBMARK + plain).valid && parse_hub_only_marker(plain + HUBMARK).valid);
        { HubOnlyMarker m = parse_hub_only_marker("\xEF\xBB\xBF" + HUBMARK.substr(0, HUBMARK.size() - 1) + "\r\n" + plain);
          CHECK(m.valid && m.smash_sha256 == BSH && m.smash_since == "v1.39.0"); }
        // malformed markers never count: bare, wrong words, short or non-hex hash, missing/garbled version, extra words, repeated lines
        for (const char* badm : {"# retrohub-only\n", "# retrohub-only-not smash-sha256 x smash-since v1\n",
                                 "# retrohub-only smash-sha256 abc smash-since v1.39.0\n", "# retrohub-only smash-since v1.39.0\n",
                                 "# retrohub-only smash-sha256 @@@@ smash-since v1.39.0\n",
                                 "# retrohub-only smash-sha256 CCC smash-since\n",
                                 "# retrohub-only smash-sha256 CCC smash-since vX\n",
                                 "#retrohub-only smash-sha256 CCC smash-since v1\n"})
            CHECK(!parse_hub_only_marker(std::string(badm) + plain).valid);
        for (const std::string& tail : {std::string(" smash-since\n"), std::string(" smash-since vX\n"), std::string(" smash-since v1 extra\n"), std::string(" smash-until v1\n")})
            CHECK(!parse_hub_only_marker("# retrohub-only smash-sha256 " + BSH + tail + plain).valid);
        CHECK(!parse_hub_only_marker("# retrohub-only smash-sha256 " + std::string(63, 'c') + "z smash-since v1.39.0\n").valid);
        CHECK(!parse_hub_only_marker(HUBMARK + HUBMARK + plain).valid && !parse_hub_only_marker("").valid);
        CHECK(!parse_hub_only_marker(HUBMARK + "# retrohub-only smash-sha256 " + std::string(64, 'd') + " smash-since v1.39.0\n").valid);
        // the choice: HubOnly only when the INSTALLED BattleShip.exe is the one the marker names
        CHECK(choose_payload(both, plain, BSH) == Payload::Full);                            // both zips, no marker: Smash Classic's files are updated too
        CHECK(choose_payload(both, HUBMARK + plain, BSH) == Payload::HubOnly);               // matching hash
        CHECK(choose_payload(both, HUBMARK + plain, std::string(64, 'C')) == Payload::HubOnly);   // hex case does not matter
        CHECK(choose_payload(both, HUBMARK + plain, std::string(64, 'd')) == Payload::Full);   // mismatched hash (an older Smash Classic): full package
        CHECK(choose_payload(both, HUBMARK + plain, "") == Payload::Full);                   // BattleShip.exe missing or unreadable
        CHECK(choose_payload(both, HUBMARK + plain, "short") == Payload::Full);
        CHECK(choose_payload(both, "# retrohub-only\n" + plain, BSH) == Payload::Full);       // bare (malformed) marker
        CHECK(choose_payload(both, "", BSH) == Payload::Full);
        CHECK(choose_payload(hubOnly, HUBMARK + plain, BSH) == Payload::HubOnly);
        CHECK(choose_payload(hubOnly, HUBMARK + plain, std::string(64, 'd')) == Payload::None);   // only a hub zip and the wrong Smash Classic: no usable payload
        CHECK(choose_payload(hubOnly, plain, BSH) == Payload::None);
        CHECK(choose_payload(parse_release(kSample), HUBMARK + plain, BSH) == Payload::Full);
        CHECK(choose_payload(Release(), plain, BSH) == Payload::None);

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

    // --- failed check retry spacing + unpack timeout ---
    {
        const unsigned long long T = 1000000;
        CHECK(check_retry_due(false, 0, T));                                  // never checked: runs
        CHECK(!check_retry_due(true, 0, T));                                  // an answer was received: not again this session
        CHECK(!check_retry_due(true, T, T + kCheckRetryMs * 10));
        CHECK(!check_retry_due(false, T, T + 1000));                          // just failed: a dead network is not hammered
        CHECK(!check_retry_due(false, T, T + kCheckRetryMs - 1));
        CHECK(check_retry_due(false, T, T + kCheckRetryMs));                  // spacing passed: the next natural trigger retries
        CHECK(check_retry_due(false, T, T - 5));                              // clock went backwards: do not stay blocked
        CHECK(kCheckRetryMs >= 60ull * 1000 && kCheckRetryMs <= 3600ull * 1000);
        CHECK(kTarTimeoutMs >= 2ul * 60 * 1000 && kTarTimeoutMs <= 30ul * 60 * 1000);   // generous (minutes), never infinite
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
        const std::string SH = std::string(64, 'c');
        CHECK(write_staged_manifest(ud, "v1.40.0", true));                               // hub-only with no hash to re-check Smash Classic with: not reusable
        CHECK(!staged_reusable(ud, "v1.40.0", &kind));
        CHECK(write_staged_manifest(ud, "v1.40.0", true, "not-a-hash") && !staged_reusable(ud, "v1.40.0", &kind));
        std::string gotHash;
        CHECK(write_staged_manifest(ud, "v1.40.0", true, SH));
        CHECK(staged_reusable(ud, "v1.40.0", &kind, &gotHash) && kind && gotHash == SH);   // the hash the signed marker named comes back for the hand-off
        CHECK(write_staged_manifest(ud, "v1.40.0", false));
        CHECK(!staged_reusable(ud, "v1.40.0", &kind));                                  // the same files recorded as a full payload need BattleShip.exe
        StagedInfo hi = scan_staged(ud + "\\staged"); hi.version = "v1.40.0"; hi.hub_only = true; hi.smash_sha256 = SH;
        StagedInfo hp;
        CHECK(parse_staged_manifest(staged_manifest_json(hi), &hp) && hp.hub_only && hp.smash_sha256 == SH);
        hi.hub_only = false;
        CHECK(parse_staged_manifest(staged_manifest_json(hi), &hp) && !hp.hub_only && staged_manifest_json(hi).find("payload") == std::string::npos);   // older manifests stay as they were
        std::filesystem::remove_all(d, ec);
    }

    // --- the staged folder is trusted only when every file still matches the SHA-256 taken at unpack time ---
    {
        std::filesystem::path d = std::filesystem::temp_directory_path() / ("hub_updater_stagedsha_test_" + std::to_string(_getpid()));
        std::error_code ec; std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d / "staged" / "sub" / "deeper", ec);
        auto put = [](const std::filesystem::path& p, const std::string& s) { std::ofstream o(p, std::ios::binary | std::ios::trunc); o << s; };
        auto reset = [&]() {
            std::filesystem::remove_all(d / "staged", ec);
            std::filesystem::create_directories(d / "staged" / "sub" / "deeper", ec);
            put(d / "staged" / "RetroHub.exe", "new-hub");
            put(d / "staged" / "BattleShip.exe", "new-bs");
            put(d / "staged" / "sub" / "x.dat", std::string(100, 'x'));
            put(d / "staged" / "sub" / "deeper" / "y.dat", "yy");
        };
        reset();
        const std::string ud = d.u8string(), sd = ud + "\\staged";
        std::vector<StagedFile> list;
        CHECK(hash_staged(sd, &list) && list.size() == 4);
        CHECK(list[0].path == "BattleShip.exe" && list[1].path == "RetroHub.exe" && list[2].path == "sub/deeper/y.dat" && list[3].path == "sub/x.dat");   // sorted, '/' separators
        CHECK(list[3].size == 100 && list[3].sha256.size() == 64);
        CHECK(verify_staged_list(sd, list) && !verify_staged_list(sd, {}));
        std::atomic<bool> cancelled{true};
        CHECK(!verify_staged_list(sd, list, &cancelled) && !hash_staged(sd, &list, &cancelled));   // cancel stops the hashing
        CHECK(!hash_staged(ud + "\\no_such_folder", &list));
        CHECK(write_staged_manifest(ud, "v1.41.1") && staged_reusable(ud, "v1.41.1"));
        CHECK(!write_staged_manifest(ud, "v1.41.1", false, "", &cancelled));
        // the manifest carries the list and round-trips it
        { std::ifstream in(d / "staged.json", std::ios::binary); std::stringstream ss; ss << in.rdbuf();
          StagedInfo m; CHECK(parse_staged_manifest(ss.str(), &m) && m.list.size() == 4 && m.list[3].path == "sub/x.dat" && m.list[3].sha256 == list[3].sha256 && m.files == 4); }
        // tamper cases: each one must make the staged folder not reusable
        put(d / "staged" / "sub" / "x.dat", std::string(100, 'y'));                        // same size, other content (counts and bytes still match)
        CHECK(!staged_reusable(ud, "v1.41.1"));
        reset(); CHECK(staged_reusable(ud, "v1.41.1"));                                      // restored: trusted again
        put(d / "staged" / "RetroHub.exe", "new-hub!");                                      // one byte longer
        CHECK(!staged_reusable(ud, "v1.41.1"));
        reset(); put(d / "staged" / "RetroHub.exe", "new-hux");                              // one byte flipped, same size
        CHECK(!staged_reusable(ud, "v1.41.1"));
        reset(); std::filesystem::remove(d / "staged" / "sub" / "deeper" / "y.dat", ec);     // missing file
        CHECK(!staged_reusable(ud, "v1.41.1"));
        reset(); put(d / "staged" / "extra.dll", "");                                        // extra file (even an empty one)
        CHECK(!staged_reusable(ud, "v1.41.1"));
        reset(); put(d / "staged" / "sub" / "deeper" / "y.dat", "zz"); std::filesystem::remove(d / "staged" / "sub" / "x.dat", ec); put(d / "staged" / "sub" / "z.dat", std::string(100, 'x'));   // a file swapped for another of the same size
        CHECK(!staged_reusable(ud, "v1.41.1"));
        reset(); CHECK(staged_reusable(ud, "v1.41.1"));
        // a tampered manifest: wrong hash, wrong size, duplicate path, file count lying, unreadable list
        auto manifest_with = [&](const std::function<void(StagedInfo&)>& edit) {
            StagedInfo m; { std::ifstream in(d / "staged.json", std::ios::binary); std::stringstream ss; ss << in.rdbuf(); parse_staged_manifest(ss.str(), &m); }
            edit(m); put(d / "staged.json", staged_manifest_json(m));
        };
        manifest_with([](StagedInfo& m) { m.list[0].sha256 = std::string(64, '0'); });
        CHECK(!staged_reusable(ud, "v1.41.1"));
        CHECK(write_staged_manifest(ud, "v1.41.1") && staged_reusable(ud, "v1.41.1"));
        manifest_with([](StagedInfo& m) { m.list[1].size += 1; });
        CHECK(!staged_reusable(ud, "v1.41.1"));
        CHECK(write_staged_manifest(ud, "v1.41.1"));
        manifest_with([](StagedInfo& m) { m.list[1] = m.list[0]; });
        CHECK(!staged_reusable(ud, "v1.41.1"));
        CHECK(write_staged_manifest(ud, "v1.41.1"));
        manifest_with([](StagedInfo& m) { m.list.pop_back(); });                             // list shorter than the manifest's own file count
        CHECK(!staged_reusable(ud, "v1.41.1"));
        CHECK(write_staged_manifest(ud, "v1.41.1"));
        manifest_with([](StagedInfo& m) { m.list[0].path = "../BattleShip.exe"; });         // a path that is not under the folder
        CHECK(!staged_reusable(ud, "v1.41.1"));
        CHECK(write_staged_manifest(ud, "v1.41.1"));
        { StagedInfo m; CHECK(!parse_staged_manifest("{\"version\":\"v1\",\"files\":1,\"bytes\":1,\"list\":[{\"p\":\"a\",\"n\":1,\"h\":\"short\"}]}", &m));
          CHECK(!parse_staged_manifest("{\"version\":\"v1\",\"files\":1,\"bytes\":1,\"list\":5}", &m)); }
        // a manifest written by RetroHub 1.41.0 (counts and bytes only, no list) is never trusted: download again
        put(d / "staged.json", "{\"v\":1,\"version\":\"v1.41.1\",\"files\":4,\"bytes\":" + std::to_string(7 + 6 + 100 + 2) + "}\n");
        { StagedInfo m; CHECK(parse_staged_manifest("{\"v\":1,\"version\":\"v1.41.1\",\"files\":4,\"bytes\":115}", &m) && m.list.empty()); }
        CHECK(!staged_reusable(ud, "v1.41.1"));
        // hub-only keeps working through the list
        CHECK(write_staged_manifest(ud, "v1.41.1", true, std::string(64, 'c')));
        bool kind = false; std::string h;
        CHECK(staged_reusable(ud, "v1.41.1", &kind, &h) && kind && h == std::string(64, 'c'));
        std::filesystem::remove_all(d, ec);
    }

    // --- free disk space: arithmetic, wording, and reading the unpacked size from a zip ---
    {
        const unsigned long long MB = 1ull << 20, GB = 1ull << 30;
        CHECK(kSpaceMargin >= 64 * MB && kSpaceMargin <= 2 * GB);
        CHECK(space_needed_for_download(100 * MB, 0) == 100 * MB + 100 * MB + kSpaceMargin);              // zip + unpacked estimate + margin
        CHECK(space_needed_for_download(100 * MB, 40 * MB) == 60 * MB + 100 * MB + kSpaceMargin);         // a resumed partial file already holds part of the zip
        CHECK(space_needed_for_download(100 * MB, 100 * MB) == 100 * MB + kSpaceMargin);
        CHECK(space_needed_for_download(100 * MB, 900 * MB) == 100 * MB + kSpaceMargin);                  // a bogus "have" never makes the sum wrap
        CHECK(space_needed_for_unpack(500 * MB) == 500 * MB + kSpaceMargin && space_needed_for_unpack(0) == kSpaceMargin);
        CHECK(enough_space(100, 100) && enough_space(100, 101) && !enough_space(100, 99) && !enough_space(100, 0));
        CHECK(enough_space(100, -1) && enough_space(~0ull >> 1, -1));                                      // unknown free space never blocks an update
        CHECK(format_bytes(300 * MB, false) == "300 MB" && format_bytes(300 * MB + 1, true) == "301 MB" && format_bytes(300 * MB + 1, false) == "300 MB");
        CHECK(format_bytes(GB + GB / 2, false) == "1.5 GB" && format_bytes(GB + 1, true) == "1.1 GB" && format_bytes(GB + 1, false) == "1.0 GB");
        CHECK(format_bytes(10, false) == "under 1 MB" && format_bytes(0, true) == "under 1 MB" && format_bytes(10, true) == "1 MB");
        std::string m = low_space_message(GB + GB / 2 + 1, 300 * MB + 5, "C:\\Games\\RetroHub\\updates");
        CHECK(m == "There is not enough free space to install this update. It needs about 1.6 GB free on drive C: but only 300 MB is free. Free up some space and try again.");
        CHECK(low_space_message(10 * MB, 1, "\\\\server\\share\\x").find("on the install drive") != std::string::npos);
        CHECK(m.find("\xE2\x80\x94") == std::string::npos);                                                // plain English, no em dash
        { const long long fb = free_bytes_on(std::filesystem::temp_directory_path().u8string()); CHECK(fb > 0); }
        CHECK(free_bytes_on("Q:\\no\\such\\drive\\at all\\") == -1 || free_bytes_on("Q:\\no\\such\\drive\\at all\\") >= 0);   // either way, no crash

        // zip_unpacked_size: a hand-built central directory (names, extra field, comment all skipped correctly)
        auto le16 = [](std::string& o, unsigned v) { o += (char)(v & 255); o += (char)((v >> 8) & 255); };
        auto le32 = [](std::string& o, unsigned long long v) { for (int i = 0; i < 4; i++) o += (char)((v >> (8 * i)) & 255); };
        auto build_zip = [&](const std::vector<std::pair<std::string, unsigned long long>>& files, unsigned eocdCount, unsigned long long cdOffOverride) {
            std::string cd;
            for (const auto& fe : files) {
                le32(cd, 0x02014b50ull); le16(cd, 20); le16(cd, 20); le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0);
                le32(cd, 0); le32(cd, 5); le32(cd, fe.second);
                le16(cd, (unsigned)fe.first.size()); le16(cd, 4); le16(cd, 3); le16(cd, 0); le16(cd, 0); le32(cd, 0); le32(cd, 0);
                cd += fe.first; cd += "EXTR"; cd += "cmt";
            }
            std::string z = "junk-before-the-index";
            const unsigned long long off = z.size();
            z += cd;
            le32(z, 0x06054b50ull); le16(z, 0); le16(z, 0); le16(z, eocdCount); le16(z, eocdCount); le32(z, cd.size());
            le32(z, cdOffOverride == ~0ull ? off : cdOffOverride); le16(z, 0);
            return z;
        };
        std::filesystem::path d = std::filesystem::temp_directory_path() / ("hub_updater_space_test_" + std::to_string(_getpid()));
        std::error_code ec; std::filesystem::remove_all(d, ec); std::filesystem::create_directories(d, ec);
        auto put = [](const std::filesystem::path& p, const std::string& s) { std::ofstream o(p, std::ios::binary | std::ios::trunc); o << s; };
        const std::string zp = (d / "t.zip").u8string();
        put(d / "t.zip", build_zip({{"RetroHub.exe", 1000}, {"dir/", 0}, {"dir/data.bin", 2500000000ull}}, 3, ~0ull));
        CHECK(zip_unpacked_size(zp) == 1000 + 2500000000ll);                                               // sums sizes above 2 GB without wrapping
        put(d / "t.zip", build_zip({}, 0, ~0ull));
        CHECK(zip_unpacked_size(zp) == 0);
        put(d / "t.zip", build_zip({{"a", 7}, {"b", 0xFFFFFFFFull}}, 2, ~0ull));
        CHECK(zip_unpacked_size(zp) == -1);                                                                // zip64 marker: the caller estimates instead
        put(d / "t.zip", build_zip({{"a", 7}}, 2, ~0ull));
        CHECK(zip_unpacked_size(zp) == -1);                                                                // the record promises more entries than exist
        put(d / "t.zip", build_zip({{"a", 7}}, 1, 99999));
        CHECK(zip_unpacked_size(zp) == -1);                                                                // an index offset outside the file
        put(d / "t.zip", "not a zip at all, just some bytes that are long enough to be read");
        CHECK(zip_unpacked_size(zp) == -1);
        put(d / "t.zip", "tiny");
        CHECK(zip_unpacked_size(zp) == -1 && zip_unpacked_size((d / "missing.zip").u8string()) == -1);
        // a real zip, made by Windows' own tar.exe (the same tool that unpacks the update)
        {
            std::filesystem::create_directories(d / "src" / "sub", ec);
            put(d / "src" / "a.bin", std::string(4000, 'a')); put(d / "src" / "sub" / "b.bin", std::string(123, 'b'));
            std::string tarExe, terr;
            CHECK(find_system_tool("tar.exe", &tarExe, &terr));
            int rc = run_command("\"" + tarExe + "\" -a -c -f \"" + (d / "real.zip").u8string() + "\" -C \"" + (d / "src").u8string() + "\" a.bin sub", [](const std::string&) {}, 60000);
            CHECK(rc == 0 && zip_unpacked_size((d / "real.zip").u8string()) == 4123);
        }
        std::filesystem::remove_all(d, ec);
    }

    // --- shell/antivirus droppings do not make a staged folder untrusted; a repeatedly discarded tag stops being retried ---
    {
        std::filesystem::path d = std::filesystem::temp_directory_path() / ("hub_updater_dropping_test_" + std::to_string(_getpid()));
        std::error_code ec; std::filesystem::remove_all(d, ec); std::filesystem::create_directories(d / "staged", ec);
        auto put = [](const std::filesystem::path& p, const std::string& s) { std::ofstream o(p, std::ios::binary | std::ios::trunc); o << s; };
        put(d / "staged" / "RetroHub.exe", "new-hub"); put(d / "staged" / "BattleShip.exe", "new-bs");
        const std::string ud = d.u8string();
        CHECK(write_staged_manifest(ud, "v1.42.0") && staged_reusable(ud, "v1.42.0"));
        put(d / "staged" / "desktop.ini", "[.ShellClassInfo]"); put(d / "staged" / "Thumbs.db", "x"); put(d / "staged" / "THUMBS.DB", "y");
        put(d / "staged" / "av_scan.tmp", "z"); SetFileAttributesW((d / "staged" / "av_scan.tmp").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        CHECK(staged_reusable(ud, "v1.42.0"));                                                  // all four ignored
        CHECK(scan_staged(ud + "\\staged").files == 2);
        put(d / "staged" / "hidden_only.txt", "h"); SetFileAttributesW((d / "staged" / "hidden_only.txt").c_str(), FILE_ATTRIBUTE_HIDDEN);
        CHECK(!staged_reusable(ud, "v1.42.0"));                                                 // hidden alone is a real file
        std::filesystem::remove(d / "staged" / "hidden_only.txt", ec);
        CHECK(staged_reusable(ud, "v1.42.0"));
        put(d / "staged" / "notes.txt", "n");
        CHECK(!staged_reusable(ud, "v1.42.0"));                                                 // an ordinary extra file still counts
        // droppings never hide a changed real file
        std::filesystem::remove(d / "staged" / "notes.txt", ec); put(d / "staged" / "RetroHub.exe", "new-hux");
        CHECK(!staged_reusable(ud, "v1.42.0"));
        std::filesystem::remove_all(d, ec);

        DiscardGuard g;
        CHECK(discard_allowed(g, "v1.42.0"));
        note_discard(g, "v1.42.0"); CHECK(discard_allowed(g, "v1.42.0"));                      // first discard: retry
        note_discard(g, "v1.42.0"); CHECK(!discard_allowed(g, "v1.42.0"));                     // second: stop for this tag
        CHECK(discard_allowed(g, "v1.42.1"));                                                   // another tag is unaffected
        note_discard(g, "v1.42.1"); CHECK(g.tag == "v1.42.1" && g.count == 1 && discard_allowed(g, "v1.42.1") && discard_allowed(g, "v1.42.0"));
        note_discard(g, "v1.42.1"); note_staged_ok(g); CHECK(discard_allowed(g, "v1.42.1") && g.count == 0);   // a good reuse resets
        std::string dm = discard_message("v1.42.0");
        CHECK(dm.find("v1.42.0") != std::string::npos && dm.find("\xE2\x80\x94") == std::string::npos);
    }

    // --- a version the installer rolled back is not offered again (updates\bad_version.json) ---
    {
        CHECK(bad_version_verdict("v1.42.0", "v1.42.0", false) == BadVerdict::Skip);
        CHECK(bad_version_verdict("v1.42.0", "1.42.0", false) == BadVerdict::Skip && bad_version_verdict("1.42.0", "V1.42.0", false) == BadVerdict::Skip);   // v prefix ignored
        CHECK(bad_version_verdict("v1.42.0", "v1.42.0", true) == BadVerdict::ForcedRetry);
        CHECK(bad_version_verdict("v1.42.1", "v1.42.0", false) == BadVerdict::Proceed && bad_version_verdict("v1.42.1", "v1.42.0", true) == BadVerdict::Proceed);
        CHECK(bad_version_verdict("v1.42.0", "", false) == BadVerdict::Proceed && bad_version_verdict("", "", false) == BadVerdict::Proceed);
        CHECK(bad_version_verdict("v1.42.0", "v1.4.20", false) == BadVerdict::Proceed);           // a different version that merely looks alike
        CHECK(bad_version_message("v1.42.0", "v1.41.0") == "Version 1.42.0 could not start on this PC, so RetroHub stayed on version 1.41.0.");

        std::filesystem::path d = std::filesystem::temp_directory_path() / ("hub_updater_bad_test_" + std::to_string(_getpid()));
        std::error_code ec; std::filesystem::remove_all(d, ec); std::filesystem::create_directories(d / "updates", ec);
        const std::wstring inst = d.wstring();
        auto write_bad = [&](const std::string& v) { apply::BadVersion b; b.version = v; b.from = "v1.41.0"; b.reason = "did not start"; b.at = 1700000000; b.exit_code = 70; return apply::bad_version_write(inst, b); };
        auto record_there = [&]() { apply::BadVersion b; return apply::bad_version_read(inst, &b); };
        std::string why;
        CHECK(!bad_version_gate(inst, "v1.42.0", false, &why) && why.empty());                  // no file: ignored
        CHECK(write_bad("v1.42.0") && record_there());
        CHECK(bad_version_gate(inst, "v1.42.0", false, &why) && why.find("could not start on this PC") != std::string::npos && record_there());   // same tag: skipped, record kept
        why.clear();
        CHECK(bad_version_gate(inst, "1.42.0", false, &why) && record_there());                  // automatic checks keep skipping it
        why.clear();
        CHECK(!bad_version_gate(inst, "v1.42.0", true, &why) && why.empty() && !record_there());   // "Try again": record cleared, this one try proceeds
        CHECK(!bad_version_gate(inst, "v1.42.0", false, &why));                                  // ... and nothing blocks it now
        CHECK(write_bad("v1.42.0") && !bad_version_gate(inst, "v1.43.0", false, &why) && !record_there());   // a newer tag proceeds and clears the stale record
        // malformed or unreadable files are ignored
        for (const char* junk : {"", "not json", "{\"v\":1}", "{\"v\":1,\"version\":\"\"}", "[1,2,3]", "{\"version\":5}"}) {
            { std::ofstream o(d / "updates" / "bad_version.json", std::ios::binary | std::ios::trunc); o << junk; }
            CHECK(!bad_version_gate(inst, "v1.42.0", false, &why));
        }
        std::filesystem::remove_all(d, ec);
    }

    // --- "Try again" never leaks into a later automatic check; a blocked tag's leftovers are deleted ---
    {
        set_busy_for_test(true, false);
        CHECK(!retry_bad_version());                                                  // a check is already running: nothing starts, nothing is kept
        set_busy_for_test(false, true);
        CHECK(!retry_bad_version());                                                  // a download is running
        set_busy_for_test(false, false);
        CHECK(!checking() && !downloading());                                         // and no stale state was left behind by those calls

        std::filesystem::path d = std::filesystem::temp_directory_path() / ("hub_updater_blocked_test_" + std::to_string(_getpid()));
        std::error_code ec; std::filesystem::remove_all(d, ec); std::filesystem::create_directories(d / "staged" / "sub", ec);
        auto put = [](const std::filesystem::path& p, const std::string& s) { std::ofstream o(p, std::ios::binary | std::ios::trunc); o << s; };
        put(d / "staged" / "RetroHub.exe", "hub"); put(d / "staged" / "BattleShip.exe", "bs"); put(d / "staged" / "sub" / "x.dat", "xx");
        const std::string ud = d.u8string();
        CHECK(write_staged_manifest(ud, "v1.42.0"));
        ResumeKey rk; rk.tag = "v1.42.0"; rk.asset = "BattleShip-windows.zip"; rk.sha256 = std::string(64, 'a'); rk.size = 100;
        put(d / "update.zip", "partial"); put(d / "update.zip.resume", resume_record_json(rk));
        discard_blocked_artifacts(ud, "v1.43.0");                                     // another tag: everything stays
        CHECK(exists(ud + "\\staged.json") && exists(ud + "\\staged\\RetroHub.exe") && exists(ud + "\\update.zip"));
        discard_blocked_artifacts(ud, "");
        CHECK(exists(ud + "\\staged.json") && exists(ud + "\\update.zip"));
        discard_blocked_artifacts(ud, "1.42.0");                                      // the blocked tag (v prefix ignored): staged folder, zip and record go
        CHECK(!exists(ud + "\\staged.json"));
        CHECK(!exists(ud + "\\staged"));
        CHECK(!exists(ud + "\\update.zip"));
        CHECK(!exists(ud + "\\update.zip.resume"));
        discard_blocked_artifacts(ud, "v1.42.0");                                     // nothing there: no harm
        std::filesystem::remove_all(d, ec);
    }

    // --- download robustness: no hard cap, stall detector, retries, resume only for the same release ---
    {
        const std::string fl = download_flags(false), flr = download_flags(true);
        CHECK(fl.find(" -m ") == std::string::npos && fl.find("--max-time") == std::string::npos);   // a slow link must be able to finish
        CHECK(fl.find("--speed-limit 1024") != std::string::npos && fl.find("--speed-time 60") != std::string::npos);
        CHECK(fl.find("--retry 3") != std::string::npos && fl.find("--retry-delay") != std::string::npos);
        CHECK(fl.find("-C -") == std::string::npos && flr.find(" -C -") != std::string::npos && flr.find("--speed-limit") != std::string::npos);
        ResumeKey k; k.tag = "v1.40.0"; k.asset = "BattleShip-windows.zip"; k.sha256 = std::string(64, 'a'); k.size = 1000;
        const std::string rec = resume_record_json(k);
        CHECK(resume_eligible(rec, k, 1) && resume_eligible(rec, k, 999));
        CHECK(!resume_eligible(rec, k, 0) && !resume_eligible(rec, k, -1));                     // nothing there
        CHECK(!resume_eligible(rec, k, 1000) && !resume_eligible(rec, k, 5000));                // full size or bigger: a failed file, never resumed
        { ResumeKey w = k; w.tag = "v1.40.1"; CHECK(!resume_eligible(rec, w, 500)); }
        { ResumeKey w = k; w.asset = "RetroHub-windows.zip"; CHECK(!resume_eligible(rec, w, 500)); }
        { ResumeKey w = k; w.size = 1001; CHECK(!resume_eligible(rec, w, 500)); }
        { ResumeKey w = k; w.sha256 = std::string(64, 'b'); CHECK(!resume_eligible(rec, w, 500)); }
        { ResumeKey w = k; w.sha256 = std::string(64, 'A'); CHECK(resume_eligible(rec, w, 500)); }   // hex case does not matter
        { ResumeKey w = k; w.sha256.clear(); ResumeKey r2 = w; CHECK(!resume_eligible(resume_record_json(r2), w, 500)); }   // no listed hash to bind to
        { ResumeKey w = k; w.tag.clear(); CHECK(!resume_eligible(resume_record_json(w), w, 500)); }
        CHECK(!resume_eligible("", k, 500) && !resume_eligible("not json", k, 500) && !resume_eligible("{}", k, 500));
        CHECK(!resume_eligible("{\"v\":2,\"tag\":\"v1.40.0\",\"asset\":\"BattleShip-windows.zip\",\"size\":1000,\"sha256\":\"" + std::string(64, 'a') + "\"}", k, 500));   // unknown record version
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
        set_pubkey_for_test(tk.pub);   // the server's lists are signed by the throwaway test key
        std::string base = "http://127.0.0.1:" + std::to_string(srv.port) + "/";
        srv.routes["/BattleShip-windows.zip"] = zipBody;
        srv.routes["/good.txt"] = real + "  BattleShip-windows.zip\n" + std::string(64, '1') + "  RetroHub-Setup.exe\n";
        srv.routes["/bad.txt"] = std::string(64, '0') + "  BattleShip-windows.zip\n";
        srv.routes["/other.txt"] = real + "  RetroHub-Setup.exe\n";
        for (const char* r : {"/good.txt", "/bad.txt", "/other.txt"}) srv.routes[std::string(r) + ".sig"] = sign_sums(tk, srv.routes[r]);
        srv.routes["/wrongkey.txt"] = srv.routes["/good.txt"];      srv.routes["/wrongkey.txt.sig"] = sign_sums(otherKey, srv.routes["/good.txt"]);
        srv.routes["/nosig.txt"] = srv.routes["/good.txt"];          // no .sig route at all: 404
        srv.routes["/garbage.txt"] = srv.routes["/good.txt"];        srv.routes["/garbage.txt.sig"] = "not a signature";
        srv.routes["/tampered.txt"] = srv.routes["/good.txt"];       srv.routes["/tampered.txt.sig"] = srv.routes["/good.txt.sig"];
        srv.routes["/tampered.txt"][5] ^= 1;                         // one byte changed after signing (the zip's hash line, say)
        FetchSpec f; f.zip_url = base + "BattleShip-windows.zip"; f.size = (long long)zipBody.size();
        std::string err;
        auto run = [&](const char* tag, const std::string& sums, const std::string& digest, long long size) {
            f.tag = tag; f.sums_url = sums.empty() ? "" : base + sums; f.sig_url = sums.empty() ? "" : base + sums + ".sig"; f.digest = digest; f.size = size; err.clear();
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
        CHECK(!run("v1.17.0", "", "", N) && !exists(zipPath));                         // every release needs signed sums now: even an old tag without them is refused
        CHECK(!run("v1.17.0", "", "sha256:" + std::string(64, 'f'), N) && !exists(zipPath));
        CHECK(!run("v1.17.0", "bad.txt", "", N) && !exists(zipPath));                  // sums enforced
        // signatures: the zip is never even downloaded unless the list verifies
        CHECK(!run("v1.18.0", "nosig.txt", "", N) && !exists(zipPath) && err == kUnsignedMessage);     // signature missing (404)
        CHECK(!run("v1.18.0", "garbage.txt", "", N) && !exists(zipPath) && err == kUnsignedMessage);   // malformed signature
        CHECK(!run("v1.18.0", "wrongkey.txt", "", N) && !exists(zipPath) && err == kUnsignedMessage);  // signed by someone else
        CHECK(!run("v1.18.0", "tampered.txt", "", N) && !exists(zipPath) && err == kUnsignedMessage);  // list changed by one byte after signing
        { std::string keep = f.sig_url; f.sig_url.clear(); f.sums_url = base + "good.txt"; err.clear();
          CHECK(!download_verified(f, zipPath, &err, [](const std::string&) {}) && err == kUnsignedMessage && !exists(zipPath)); f.sig_url = keep; }   // release lists no .sig asset
        CHECK(!run("v1.18.0", "good.txt", "", N + 1) && !exists(zipPath));             // size mismatch
        // the RetroHub-only zip: verified under its own name; tampered body, missing line, sums_text preset
        srv.routes["/RetroHub-windows.zip"] = zipBody;
        srv.routes["/hubgood.txt"] = std::string(64, '2') + "  BattleShip-windows.zip\n" + real + "  RetroHub-windows.zip\n";
        srv.routes["/hubmissing.txt"] = real + "  BattleShip-windows.zip\n";
        srv.routes["/hubbad.txt"] = real + "  BattleShip-windows.zip\n" + std::string(64, '0') + "  RetroHub-windows.zip\n";
        for (const char* r : {"/hubgood.txt", "/hubmissing.txt", "/hubbad.txt"}) srv.routes[std::string(r) + ".sig"] = sign_sums(tk, srv.routes[r]);
        f.zip_url = base + "RetroHub-windows.zip"; f.asset_name = kHubAssetName;
        CHECK(run("v1.40.0", "hubgood.txt", "", N) && exists(zipPath));                // hub zip matches ITS line (the full zip's line differs)
        CHECK(!run("v1.40.0", "hubmissing.txt", "", N) && !exists(zipPath));           // no SHA line for the hub zip: refused, file deleted
        CHECK(!run("v1.40.0", "hubbad.txt", "", N) && !exists(zipPath) && err.find("checksum") != std::string::npos);   // tampered: refused, file deleted
        CHECK(!run("v1.40.0", "good.txt", "", N) && !exists(zipPath));                 // sums that only list the full zip
        f.sums_text = std::string(64, '2') + "  BattleShip-windows.zip\n" + real + "  RetroHub-windows.zip\n";
        f.sig_bytes = sign_sums(tk, f.sums_text);
        CHECK(run("v1.40.0", "", "", N) && exists(zipPath));                           // sums already fetched (and verified): no second fetch
        f.sums_text = real + "  BattleShip-windows.zip\n";                              // a different list under the old signature: refused
        CHECK(!run("v1.40.0", "", "", N) && !exists(zipPath) && err == kUnsignedMessage);
        f.sig_bytes.clear();                                                           // preset sums without any signature: refused too
        f.sums_text = std::string(64, '2') + "  BattleShip-windows.zip\n" + real + "  RetroHub-windows.zip\n";
        CHECK(!run("v1.40.0", "", "", N) && !exists(zipPath));
        // --- resume: a partial file of the SAME release is continued; anything else starts over; a file that fails verification is never resumed ---
        {
            f.asset_name = kHubAssetName; f.zip_url = base + "RetroHub-windows.zip";
            f.sums_text = std::string(64, '2') + "  BattleShip-windows.zip\n" + real + "  RetroHub-windows.zip\n";
            f.sig_bytes = sign_sums(tk, f.sums_text);
            ResumeKey key; key.tag = "v1.40.0"; key.asset = kHubAssetName; key.sha256 = real; key.size = N;
            const std::string rec = zipPath + ".resume";
            const size_t half = zipBody.size() / 2;
            auto put_partial = [&](size_t n, const ResumeKey& k, bool corrupt) {
                std::string part = zipBody.substr(0, n);
                if (corrupt && !part.empty()) part[10] ^= 1;
                { std::ofstream o(std::filesystem::u8path(zipPath), std::ios::binary | std::ios::trunc); o.write(part.data(), (std::streamsize)part.size()); }
                { std::ofstream o(std::filesystem::u8path(rec), std::ios::binary | std::ios::trunc); o << resume_record_json(k); }
            };
            // A: same release: the request continues at the partial's size, and the verified result is the whole file
            put_partial(half, key, false); srv.last_range = -1;
            CHECK(run("v1.40.0", "", "", N) && srv.last_range.load() == (long long)half && exists(zipPath) && !exists(rec));
            // B: a record naming another tag, size or hash: not continued, downloaded afresh
            ResumeKey other = key; other.tag = "v1.39.0";
            put_partial(half, other, false); srv.last_range = -1;
            CHECK(run("v1.40.0", "", "", N) && srv.last_range.load() == -1 && !exists(rec));
            other = key; other.sha256 = std::string(64, '3');
            put_partial(half, other, false); srv.last_range = -1;
            CHECK(run("v1.40.0", "", "", N) && srv.last_range.load() == -1);
            // C: a partial file with no record at all (an older RetroHub left it): started afresh
            put_partial(half, key, false); std::filesystem::remove(std::filesystem::u8path(rec), ec); srv.last_range = -1;
            CHECK(run("v1.40.0", "", "", N) && srv.last_range.load() == -1);
            // D: a corrupt partial passes the record but fails the SHA-256: deleted with its record, and the next try starts from zero
            put_partial(half, key, true); srv.last_range = -1;
            CHECK(!run("v1.40.0", "", "", N) && srv.last_range.load() == (long long)half && !exists(zipPath) && !exists(rec) && err.find("checksum") != std::string::npos);
            srv.last_range = -1;
            CHECK(run("v1.40.0", "", "", N) && srv.last_range.load() == -1);
            // E: the server cannot continue: curl says so, the update starts over once and still succeeds
            put_partial(half, key, false); srv.ignore_range = true;
            CHECK(run("v1.40.0", "", "", N) && exists(zipPath));
            srv.ignore_range = false;
            // F: a failed transfer keeps the partial file and its record for the next try (which then continues)
            put_partial(half, key, false);
            f.zip_url = base + "nothere.zip";
            CHECK(!run("v1.40.0", "", "", N) && err == "download failed." && exists(zipPath) && exists(rec) && std::filesystem::file_size(std::filesystem::u8path(zipPath)) == half);
            f.zip_url = base + "RetroHub-windows.zip"; srv.last_range = -1;
            CHECK(run("v1.40.0", "", "", N) && srv.last_range.load() == (long long)half && !exists(rec));
        }
        f.sums_text.clear(); f.sig_bytes.clear(); f.asset_name.clear(); f.zip_url = base + "BattleShip-windows.zip";
        set_pubkey_for_test(nullptr);
        CHECK(!run("v1.18.0", "good.txt", "", N) && !exists(zipPath) && err == kUnsignedMessage);   // the real (built-in) key does not accept the test key's lists
        std::filesystem::remove_all(dir, ec);
    }

    // --- worker threads are owned and joined; shutdown() ends a running child by its handle ---
    {
        OwnedThread ot;
        std::atomic<bool> go{false};
        CHECK(!ot.running() && ot.join_for(0));                       // never started: nothing to join
        CHECK(ot.start([&] { while (!go.load()) Sleep(5); }));
        CHECK(ot.running() && !ot.join_for(50));                      // still going: a bounded wait gives up
        CHECK(!ot.start([] {}, 50) && ot.running());                  // a second run is refused while one is going
        go = true;
        CHECK(ot.join_for(3000) && !ot.running());
        int ran = 0;
        CHECK(ot.start([&] { ran++; }) && ot.join_for(3000) && ran == 1);   // restartable after a join
        CHECK(ot.start([] { throw 1; }) && ot.join_for(3000));       // an exception never escapes the thread
        CHECK(ot.start([&] { ran++; }));                              // start() joins the finished previous run itself
        CHECK(ot.join_for(3000) && ran == 2);

        const std::string sd = system_dir();
        std::atomic<int> code{12345};
        std::thread th([&] { code = run_command("\"" + sd + "\\ping.exe\" -n 60 127.0.0.1", [](const std::string&) {}); });   // harmless, long-running, loopback only
        for (int i = 0; i < 100 && live_child_count() == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(live_child_count() == 1);
        auto t0 = std::chrono::steady_clock::now();
        shutdown();                                                   // ends the child by its handle
        th.join();
        long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(code.load() != 0 && code.load() != 12345);              // it did not run its 60 s
        CHECK(ms < 8000);
        CHECK(live_child_count() == 0);
        // after shutdown() began, no new child is allowed to run
        t0 = std::chrono::steady_clock::now();
        CHECK(run_command("\"" + sd + "\\ping.exe\" -n 60 127.0.0.1", [](const std::string&) {}) != 0);
        CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 8000);
        reset_quit_for_test();
        CHECK(run_command("\"" + sd + "\\cmd.exe\" /c exit 0", [](const std::string&) {}) == 0);   // and works again once reset
        CHECK(live_child_count() == 0);
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
