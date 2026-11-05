/* hubidbackup_test: the per-Windows-user identity backup (offline; every path is a temp dir via RETROHUB_IDENTITY_BACKUP). */
#include "friends.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace hubnet;
namespace fs = std::filesystem;
static int g_fail = 0;
#define CHECK(c) do { if (c) printf("  ok   %s\n", #c); else { g_fail++; printf("  FAIL %s\n", #c); } } while (0)

static void setenv_(const char* k, const char* v) { _putenv_s(k, v ? v : ""); }
static const char SEP = '\\';
static std::string ident(const std::string& dir) { return dir + SEP + "identity.json"; }
static void write_ident(const fs::path& p, const Identity& i)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::trunc) << "{\"user_id\":\"" << i.user_id << "\",\"secret\":\"" << i.secret << "\",\"created\":1}";
}

int main()
{
    fs::path root = fs::temp_directory_path() / ("rh_idbackup_" + std::to_string(std::rand()));
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string bp = (root / "bk" / "identity-backup.json").string();
    setenv_("RETROHUB_QA", ""); setenv_("RETROHUB_AUTOTEST", ""); setenv_("RETROHUB_TEST_SERVER", ""); setenv_("RETROHUB_TEST", "");
    setenv_("RETROHUB_IDENTITY_BACKUP", bp.c_str());

    Identity a = identity_generate(), b = identity_generate();
    // save + adopt on a fresh install
    CHECK(identity_backup_save(a));
    CHECK(fs::exists(bp));
    CHECK(!fs::exists(bp + ".tmp"));
    std::string d1 = (root / "install1").string();
    Identity got;
    CHECK(identity_backup_adopt_if_fresh(d1, "", &got));
    CHECK(got.user_id == a.user_id && got.secret == a.secret);
    Identity r;
    CHECK(identity_read_file(ident(d1), &r) && r.user_id == a.user_id);
    CHECK(identity_load_or_create(d1, "").user_id == a.user_id);

    // never overwrite an existing identity
    std::string d2 = (root / "install2").string();
    identity_load_or_create(d2, "");   // makes its own
    Identity own; identity_read_file(ident(d2), &own);
    CHECK(own.user_id != a.user_id);
    CHECK(!identity_backup_adopt_if_fresh(d2, "", nullptr));
    Identity own2; identity_read_file(ident(d2), &own2);
    CHECK(own2.user_id == own.user_id && own2.secret == own.secret);

    // a vault mirror's identity wins over the backup
    std::string d3 = (root / "install3").string(), v3 = (root / "vault3").string();
    write_ident(root / "vault3" / "_identity" / "identity.json", identity_generate());
    CHECK(!identity_backup_adopt_if_fresh(d3, v3, nullptr));

    // corrupt / missing backup is ignored
    { std::ofstream(bp, std::ios::trunc) << "{not json"; }
    CHECK(!identity_backup_adopt_if_fresh((root / "install4").string(), "", nullptr));
    CHECK(!fs::exists(root / "install4" / "identity.json"));
    { std::ofstream(bp, std::ios::trunc) << "{\"user_id\":\"short\",\"secret\":\"x\"}"; }
    CHECK(!identity_backup_adopt_if_fresh((root / "install4").string(), "", nullptr));
    fs::remove(bp);
    CHECK(!identity_backup_adopt_if_fresh((root / "install5").string(), "", nullptr));

    // save follows the current confirmed account
    CHECK(identity_backup_save(a) && identity_backup_save(a) && identity_backup_save(b));
    Identity rb; CHECK(identity_read_file(bp, &rb) && rb.user_id == b.user_id);
    Identity bad; bad.user_id = "x"; bad.secret = "y";
    CHECK(!identity_backup_save(bad));
    CHECK(identity_read_file(bp, &rb) && rb.user_id == b.user_id);

    // policy: qa- names and non-production servers never use it
    CHECK(identity_backup_allowed("", "Mia"));
    CHECK(!identity_backup_allowed("", "qa-bot"));
    CHECK(!identity_backup_allowed("ws://127.0.0.1:8761", "Mia"));
    CHECK(identity_backup_allowed("wss://smash-netplay.jumpcircuit.workers.dev/friends", "Mia"));

    // QA mode without an override: no path at all, nothing read or written
    setenv_("RETROHUB_IDENTITY_BACKUP", "");
    setenv_("RETROHUB_QA", "1");
    CHECK(identity_backup_path().empty());
    CHECK(!identity_backup_allowed("", "Mia"));
    CHECK(!identity_backup_save(a));
    CHECK(!identity_backup_adopt_if_fresh((root / "install6").string(), "", nullptr));
    setenv_("RETROHUB_QA", ""); setenv_("RETROHUB_AUTOTEST", "1");
    CHECK(identity_backup_path().empty());
    setenv_("RETROHUB_AUTOTEST", "");
    // normal mode: the default path lives under LOCALAPPDATA/RetroHub
    setenv_("LOCALAPPDATA", root.string().c_str());
    CHECK(identity_backup_path() == root.string() + SEP + "RetroHub" + SEP + "identity-backup.json");

    fs::remove_all(root);
    printf(g_fail ? "FAILED %d\n" : "all ok\n", g_fail);
    return g_fail ? 1 : 0;
}
