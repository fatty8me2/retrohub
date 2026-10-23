/* Offline tests for link-a-new-PC: code normalization, seal/open round trip with two scratch identities,
 * wrong code / tampering, identity adoption with backup, and identity file export/import. No network. */
#include "identity_link.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace hubnet;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
    } while (0)

static std::string read_all(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main()
{
    /* normalization */
    CHECK(link_normalize_code("abcd-efgh") == "ABCDEFGH");
    CHECK(link_normalize_code(" ABCD EFGH ") == "ABCDEFGH");
    CHECK(link_normalize_code("ABCDEFG") == "");          /* too short */
    CHECK(link_normalize_code("ABCDEFGI") == "");         /* I is not in the alphabet */
    CHECK(link_normalize_code("0OOOOOOO") == "");         /* 0 and O are excluded */

    /* two scratch identities */
    Identity a = identity_generate();
    Identity b = identity_generate();
    CHECK(a.valid() && b.valid() && a.user_id != b.user_id);

    /* seal with one, open with the same code: identity restored exactly */
    LinkEnvelope env;
    CHECK(link_seal("ABCD-2345", a, &env));
    CHECK(!env.ct.empty() && env.ct.find(a.secret) == std::string::npos);   /* ciphertext, not plaintext */
    Identity got;
    CHECK(link_open("abcd2345", env, &got));
    CHECK(got.user_id == a.user_id && got.secret == a.secret && got.created == a.created);

    /* wrong code fails */
    Identity bad;
    CHECK(!link_open("ABCD2346", env, &bad));

    /* tampered ciphertext fails (flip one base64 char in the middle) */
    LinkEnvelope t = env;
    size_t mid = t.ct.size() / 2;
    t.ct[mid] = t.ct[mid] == 'A' ? 'B' : 'A';
    CHECK(!link_open("ABCD2345", t, &bad));

    /* a second seal of the same identity uses a fresh salt/iv */
    LinkEnvelope env2;
    CHECK(link_seal("ABCD2345", b, &env2));
    CHECK(env2.salt != env.salt && env2.iv != env.iv);
    Identity gotb;
    CHECK(link_open("ABCD2345", env2, &gotb) && gotb.user_id == b.user_id);

    /* adoption: PC holds a, links b -> identity.json is b, a is kept in a backup */
    fs::path dir = fs::temp_directory_path() / ("rh_link_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    std::string data_dir = dir.string() + "\\";
    std::string err;
    CHECK(identity_export_file(a, (dir / "a.json").string(), &err));
    CHECK(identity_import_file((dir / "a.json").string(), &got, &err) && got.user_id == a.user_id);

    CHECK(identity_adopt(data_dir, "", a));                    /* current identity = a */
    CHECK(identity_adopt(data_dir, "", b));                    /* replaced by b */
    Identity cur;
    CHECK(identity_read_file((dir / "identity.json").string(), &cur) && cur.user_id == b.user_id);
    bool backup_has_a = false;
    for (auto& e : fs::directory_iterator(dir)) {
        std::string n = e.path().filename().string();
        if (n.rfind("identity-backup-", 0) == 0 && read_all(e.path()).find(a.user_id) != std::string::npos)
            backup_has_a = true;
    }
    CHECK(backup_has_a);

    /* a file that is not an identity is rejected */
    { std::ofstream f(dir / "junk.json"); f << "{\"user_id\":\"nope\"}"; }
    CHECK(!identity_import_file((dir / "junk.json").string(), &got, &err));

    fs::remove_all(dir, ec);

    std::printf(g_fail ? "hublink_test: %d FAILED\n" : "hublink_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
