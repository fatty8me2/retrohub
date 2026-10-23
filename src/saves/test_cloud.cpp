/* Console test for cloud saves. Unit checks always run. The end-to-end part (two simulated PCs syncing through a
 * local `wrangler dev` Worker with emulated R2) runs when RETROHUB_TEST_SAVES_ACCOUNTS points at the JSON written
 * by `node test_saves.mjs --serve <file>` (see qa\saves_cloud.ps1); otherwise it is skipped and the test passes. */
#include "cloud.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace hub::saves;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void write_file(const fs::path& p, const std::string& content, int secs_ago) {
    fs::create_directories(p.parent_path());
    { std::ofstream o(p, std::ios::binary | std::ios::trunc); o << content; }
    HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= (ULONGLONG)secs_ago * 10000000ULL;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}
static std::string read_file(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    std::stringstream s; s << i.rdbuf();
    return s.str();
}
static std::map<std::string, std::string> tree(const fs::path& root) {
    std::map<std::string, std::string> m;
    if (!fs::exists(root)) return m;
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        std::string r = fs::relative(e.path(), root).generic_u8string();
        if (r.rfind(".sync/", 0) == 0 || r.rfind("_identity/", 0) == 0) continue;   // identity is per-PC, never in the cloud
        m[r] = read_file(e.path());
    }
    return m;
}
static int conflicts(const fs::path& root) {
    int n = 0;
    for (auto& kv : tree(root)) if (kv.first.rfind("_conflicts/", 0) == 0) n++;
    return n;
}

/* minimal extraction from the flat accounts file */
static std::string jget(const std::string& j, const std::string& obj, const std::string& key) {
    size_t o = obj.empty() ? 0 : j.find("\"" + obj + "\"");
    if (o == std::string::npos) return "";
    size_t k = j.find("\"" + key + "\"", o);
    if (k == std::string::npos) return "";
    size_t q1 = j.find('"', j.find(':', k) + 1), q2 = j.find('"', q1 + 1);
    return j.substr(q1 + 1, q2 - q1 - 1);
}

static CloudConfig cfg_for(const std::string& url, const std::string& uid, const std::string& secret, const fs::path& mirror) {
    CloudConfig c;
    c.base_url = url; c.user_id = uid; c.secret = secret; c.mirror_dir = mirror.u8string();
    c.max_retries = 2;
    return c;
}

int main() {
    /* ---- unit ---- */
    CHECK(sha256_hex_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string s1 = cloud_sign("secret", "GET", "/saves/list", 1000, ""), s2 = cloud_sign("secret", "GET", "/saves/list", 1000, "-");
    CHECK(s1.size() == 64 && s1 == s2);
    CHECK(s1 != cloud_sign("secret", "PUT", "/saves/list", 1000, ""));
    CHECK(s1 != cloud_sign("secret2", "GET", "/saves/list", 1000, ""));
    CHECK(s1 != cloud_sign("secret", "GET", "/saves/list", 1001, ""));

    CHECK(s1 != cloud_sign("secret", "GET", "/saves/list", 1000, "", "e"));
    CHECK(s1 != cloud_sign("secret", "GET", "/saves/list", 1000, "", "", "*"));
    CHECK(s1 != cloud_sign("secret", "GET", "/saves/list", 1000, "", "", "", "5"));
    CHECK(s1 != cloud_sign("secret", "GET", "/saves/list", 1000, "", "", "", "", "9"));
    for (const char* k : {"a/b.sav", "n64/smash/battery/a.sav", "x"}) CHECK(cloud_key_safe(k));
    for (const char* k : {"", "..", "../x", "a/../b", "a/./b", "/abs", "\\abs", "a//b", "a/", "C:/x", "C:x", "a\\b", "a:stream", "a/b\x01"})
        CHECK(!cloud_key_safe(k));
    CHECK(cloud_sha_ok(sha256_hex_of("abc")));
    CHECK(!cloud_sha_ok("") && !cloud_sha_ok("abc") && !cloud_sha_ok(std::string(64, 'G')) && !cloud_sha_ok(std::string(64, 'A')));

    const char* accf = std::getenv("RETROHUB_TEST_SAVES_ACCOUNTS");
    if (!accf || !*accf) {
        printf("hubsaves_cloud_test: unit checks done, end-to-end skipped (no RETROHUB_TEST_SAVES_ACCOUNTS)\n");
        printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
        return g_fail ? 1 : 0;
    }
    std::string acc = read_file(accf);
    std::string url = jget(acc, "", "url");
    std::string uidA = jget(acc, "a", "uid"), secA = jget(acc, "a", "secret");
    std::string uidB = jget(acc, "b", "uid"), secB = jget(acc, "b", "secret");
    CHECK(!url.empty() && uidA.size() == 26 && secA.size() == 52 && uidB.size() == 26);

    fs::path W = fs::temp_directory_path() / ("rh_cloud_test_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(W);
    fs::path VA = W / "A" / "saves", VB = W / "B" / "saves";
    fs::path MA = W / "A" / "mirror", MB = W / "B" / "mirror";
    fs::create_directories(VA); fs::create_directories(VB);
    CloudConfig ca = cfg_for(url, uidA, secA, MA), cb = cfg_for(url, uidA, secA, MB);

    auto sync = [&](const fs::path& vault, const CloudConfig& c) {
        set_vault_dir(vault.u8string());
        SyncReport r = sync_cloud(c);
        for (auto& l : r.log) printf("    %s\n", l.c_str());
        if (!r.ok) printf("    ERROR: %s\n", r.error.c_str());
        return r;
    };

    printf("account status\n");
    CloudStatus st = cloud_status(ca);
    CHECK(st.enabled && st.files == 0 && st.quota > 0);
    CloudConfig bad = ca; bad.secret = std::string(52, 'a');
    CloudStatus sb = cloud_status(bad);
    CHECK(!sb.enabled && sb.unavailable);

    printf("PC A pushes, PC B pulls\n");
    write_file(VA / "n64/smash/battery/save.sav", "from-A", 100);
    write_file(VA / "gba/game/battery/a.sav", "gba-A", 100);
    write_file(VA / "_identity/identity.json", "{\"user_id\":\"x\",\"secret\":\"TOPSECRET\"}", 100);
    SyncReport r = sync(VA, ca);
    CHECK(r.ok && r.pushed == 2);
    CHECK(cloud_status(ca).files == 2);   // identity never uploaded
    r = sync(VB, cb);
    CHECK(r.ok && r.pulled == 2);
    CHECK(read_file(VB / "n64/smash/battery/save.sav") == "from-A");
    CHECK(!fs::exists(VB / "_identity"));
    r = sync(VB, cb);
    CHECK(r.ok && r.pushed == 0 && r.pulled == 0);   // steady state

    printf("update flows B -> A\n");
    write_file(VB / "n64/smash/battery/save.sav", "B-newer", 10);
    r = sync(VB, cb); CHECK(r.ok && r.pushed == 1);
    r = sync(VA, ca); CHECK(r.ok && r.pulled == 1 && r.conflicts == 0);
    CHECK(read_file(VA / "n64/smash/battery/save.sav") == "B-newer");

    printf("conflict: both PCs edit offline\n");
    write_file(VA / "n64/smash/battery/save.sav", "A-edit", 50);
    write_file(VB / "n64/smash/battery/save.sav", "B-edit-newer", 5);
    r = sync(VA, ca); CHECK(r.ok && r.pushed == 1);          // A gets there first
    r = sync(VB, cb); CHECK(r.ok && r.conflicts >= 1);       // B detects both changed
    CHECK(read_file(VB / "n64/smash/battery/save.sav") == "B-edit-newer");   // newer wins
    CHECK(conflicts(VB) >= 1);
    bool kept = false;
    for (auto& kv : tree(VB)) if (kv.first.rfind("_conflicts/", 0) == 0 && kv.second == "A-edit") kept = true;
    CHECK(kept);                                              // loser kept, not lost
    r = sync(VA, ca); CHECK(r.ok);
    CHECK(read_file(VA / "n64/smash/battery/save.sav") == "B-edit-newer");
    r = sync(VB, cb); CHECK(r.ok);
    CHECK(tree(VA) == tree(VB));                              // both PCs converge

    printf("stale pending upload: remote changed while mirror has an unsent edit\n");
    write_file(VA / "gba/game/battery/a.sav", "A-second", 20);
    {   // simulate the upload being lost: server unreachable
        CloudConfig dead = ca; dead.base_url = "http://127.0.0.1:1"; dead.max_retries = 1;
        r = sync(VA, dead);
        CHECK(!r.ok);                                          // bad network: error, nothing destroyed
        CHECK(read_file(VA / "gba/game/battery/a.sav") == "A-second");
    }
    r = sync(VA, ca); CHECK(r.ok && r.pushed == 1);            // resumes cleanly once the network is back
    r = sync(VB, cb); CHECK(r.ok && read_file(VB / "gba/game/battery/a.sav") == "A-second");

    printf("account isolation\n");
    fs::path VC = W / "C" / "saves", MC = W / "C" / "mirror";
    fs::create_directories(VC);
    CloudConfig cc = cfg_for(url, uidB, secB, MC);
    r = sync(VC, cc);
    CHECK(r.ok && r.pulled == 0 && !fs::exists(VC / "n64"));
    CHECK(cloud_status(cc).files == 0);

    printf("wrong secret is refused and changes nothing\n");
    CloudConfig evil = cfg_for(url, uidA, std::string(52, 'b'), W / "E" / "mirror");
    fs::path VE = W / "E" / "saves"; fs::create_directories(VE);
    r = sync(VE, evil);
    CHECK(!r.ok && !fs::exists(VE / "n64"));

    set_vault_dir("");
    fs::remove_all(W);
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
