/* Console test for the save vault: simulates two PCs (two vault dirs) sharing one sync folder. */
#include "savestore.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "miniz.h"

namespace fs = std::filesystem;
using namespace hub::saves;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static fs::path P(const std::string& u8) { return fs::u8path(u8); }

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
/* relpath -> content, for every regular file under root (excluding .sync) */
static std::map<std::wstring, std::string> tree(const fs::path& root0) {
    fs::path root = fs::path(L"\\\\?\\" + fs::absolute(root0).wstring());
    std::map<std::wstring, std::string> m;
    if (!fs::exists(root0)) return m;
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        std::wstring r = e.path().wstring().substr(root.wstring().size() + 1);
        for (auto& c : r) if (c == L'\\') c = L'/';
        if (r.rfind(L".sync/", 0) == 0) continue;
        m[r] = read_file(e.path());
    }
    return m;
}
static int count_conflicts(const fs::path& root) {
    int n = 0;
    for (auto& kv : tree(root)) if (kv.first.rfind(L"_conflicts/", 0) == 0) n++;
    return n;
}
static std::string conflict_content(const fs::path& root) {
    for (auto& kv : tree(root)) if (kv.first.rfind(L"_conflicts/", 0) == 0) return kv.second;
    return "";
}
static void dump(const SyncReport& r) {
    printf("    push=%d pull=%d conflicts=%d ok=%d\n", r.pushed, r.pulled, r.conflicts, (int)r.ok);
    for (auto& l : r.log) printf("      %s\n", l.c_str());
}

/* An exported zip, opened with miniz: entry names and (inflated) contents. */
struct ZipView {
    std::vector<std::string> names, bodies;
    bool ok = false;
    explicit ZipView(const std::string& path) {
        mz_zip_archive z; memset(&z, 0, sizeof z);
        if (!mz_zip_reader_init_file(&z, path.c_str(), 0)) return;
        ok = true;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); i++) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            size_t sz = 0;
            void* p = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
            names.push_back(st.m_filename);
            bodies.push_back(p ? std::string((const char*)p, sz) : std::string());
            if (p) mz_free(p);
        }
        mz_zip_reader_end(&z);
    }
    bool has(const std::string& n) const { for (auto& x : names) if (x == n) return true; return false; }
    bool name_contains(const std::string& s) const { for (auto& x : names) if (x.find(s) != std::string::npos) return true; return false; }
    bool body_contains(const std::string& s) const { for (auto& x : bodies) if (x.find(s) != std::string::npos) return true; return false; }
};
/* A zip with exactly these entries (name, content): the way a hostile or foreign zip could look. */
static bool make_zip(const std::string& path, const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive z; memset(&z, 0, sizeof z);
    if (!mz_zip_writer_init_file(&z, path.c_str(), 0)) return false;
    bool ok = true;
    for (auto& e : entries) ok = ok && mz_zip_writer_add_mem(&z, e.first.c_str(), e.second.data(), e.second.size(), MZ_DEFAULT_LEVEL);
    ok = ok && mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);
    return ok;
}

/* Enumeration-error injection (set_enum_error_hook): mode 1 = a battery folder cannot be opened, 2 = its listing breaks after the
 * first file, 3 = a file appears in the folder right after it was listed. Only folders whose path contains g_hook_match. */
static int g_hook_mode = 0;
static std::string g_hook_match;
static std::string g_hook_tail = "\\battery";
static unsigned long enum_hook(const std::string& dir, int seen) {
    const std::string& tail = g_hook_tail;
    if (!g_hook_mode || dir.find(g_hook_match) == std::string::npos) return 0;
    if (dir.size() < tail.size() || dir.compare(dir.size() - tail.size(), tail.size(), tail) != 0) return 0;
    if (g_hook_mode == 1 && seen == 0) return ERROR_ACCESS_DENIED;
    if (g_hook_mode == 2 && seen == 3) return ERROR_SHARING_VIOLATION;   // ".", "..", first file done
    if (g_hook_mode == 3 && seen == -1) { g_hook_mode = 0; write_file(P(dir) / "late.sav", "cloud-late", 20); }
    return 0;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    fs::path base = fs::temp_directory_path() / ("hubsaves_test_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(base);
    fs::path A = base / "pcA", B = base / "pcB", C = base / "pcC", cloud = base / fs::u8path("cloud ☁ sync");
    std::string cl = cloud.u8string();
    auto useA = [&] { set_vault_dir(A.u8string()); };
    auto useB = [&] { set_vault_dir(B.u8string()); };
    auto useC = [&] { set_vault_dir(C.u8string()); };

    printf("[1] first sync pushes (states excluded)\n");
    useA();
    CHECK(vault_dir() == A.u8string() + "\\");
    write_file(P(battery_dir("n64", "mp1")) / "mp1.eep", "A-save-1", 100);
    write_file(P(states_dir("n64", "mp1")) / "slot0.st", "state-A", 100);
    touch_game("n64", "mp1", "Mario Party");
    CHECK(fs::exists(P(game_dir("n64", "mp1")) / "meta.json"));
    {
        std::string meta = read_file(P(game_dir("n64", "mp1")) / "meta.json");
        CHECK(meta.find("\"title\": \"Mario Party\"") != std::string::npos);
        CHECK(meta.find("\"device\"") != std::string::npos);
        CHECK(meta.find("\"updated\"") != std::string::npos);
    }
    SyncReport r = sync_folder(cl, false);
    dump(r);
    CHECK(r.ok && r.pushed == 2 && r.pulled == 0 && r.conflicts == 0);
    CHECK(read_file(cloud / "n64/mp1/battery/mp1.eep") == "A-save-1");
    CHECK(!fs::exists(cloud / "n64/mp1/states"));

    printf("[2] second PC pulls\n");
    useB();
    r = sync_folder(cl, false);
    dump(r);
    CHECK(r.ok && r.pulled == 2 && r.pushed == 0 && r.conflicts == 0);
    CHECK(read_file(B / "n64/mp1/battery/mp1.eep") == "A-save-1");
    CHECK(!fs::exists(B / "n64/mp1/states"));

    printf("[3] edit on B propagates to A\n");
    write_file(B / "n64/mp1/battery/mp1.eep", "B-save-2", 50);
    r = sync_folder(cl, false);
    CHECK(r.ok && r.pushed == 1 && r.pulled == 0 && r.conflicts == 0);
    useA();
    r = sync_folder(cl, false);
    dump(r);
    CHECK(r.ok && r.pulled == 1 && r.pushed == 0 && r.conflicts == 0);
    CHECK(read_file(A / "n64/mp1/battery/mp1.eep") == "B-save-2");

    printf("[4] idempotent re-sync\n");
    r = sync_folder(cl, false);
    CHECK(r.ok && r.pushed == 0 && r.pulled == 0 && r.conflicts == 0);
    useB();
    r = sync_folder(cl, false);
    CHECK(r.ok && r.pushed == 0 && r.pulled == 0 && r.conflicts == 0);

    printf("[5] simultaneous edits -> one conflict copy, newer wins (B newer)\n");
    useA();
    write_file(A / "n64/mp1/battery/mp1.eep", "A-edit", 30);
    useB();
    write_file(B / "n64/mp1/battery/mp1.eep", "B-edit", 10);
    useA();
    r = sync_folder(cl, false);   // A pushes first (no conflict yet)
    CHECK(r.ok && r.pushed == 1 && r.conflicts == 0);
    useB();
    r = sync_folder(cl, false);   // B sees both changed
    dump(r);
    CHECK(r.ok && r.conflicts == 1);
    CHECK(read_file(cloud / "n64/mp1/battery/mp1.eep") == "B-edit");
    CHECK(read_file(B / "n64/mp1/battery/mp1.eep") == "B-edit");
    CHECK(count_conflicts(B) == 1 && count_conflicts(cloud) == 1);
    CHECK(conflict_content(B) == "A-edit");
    useA();
    r = sync_folder(cl, false);
    CHECK(r.ok && r.conflicts == 0);
    CHECK(read_file(A / "n64/mp1/battery/mp1.eep") == "B-edit");
    CHECK(count_conflicts(A) == 1);  // conflict copy follows to the other PC
    r = sync_folder(cl, false);
    CHECK(r.pushed == 0 && r.pulled == 0 && r.conflicts == 0);

    printf("[5b] conflict where the earlier-syncing PC is newer\n");
    useA();
    write_file(A / "n64/mp1/battery/mp1.eep", "A-newer", 5);
    useB();
    write_file(B / "n64/mp1/battery/mp1.eep", "B-older", 60);
    sync_folder(cl, false);            // B pushes its (older) edit
    useA();
    r = sync_folder(cl, false);
    dump(r);
    CHECK(r.conflicts == 1);
    CHECK(read_file(cloud / "n64/mp1/battery/mp1.eep") == "A-newer");
    CHECK(count_conflicts(cloud) == 3);   // [5]'s copy + the older B edit that replaced a newer sync-folder file (now backed up) + this conflict
    useB();
    sync_folder(cl, false);
    CHECK(read_file(B / "n64/mp1/battery/mp1.eep") == "A-newer");
    {
        auto ta = tree(A), tb = tree(B); for (auto* t : {&ta, &tb}) for (auto it = t->begin(); it != t->end();) it = it->first.find(L"/states/") != std::wstring::npos ? t->erase(it) : std::next(it);
        for (auto& kv : ta) if (!tb.count(kv.first) || tb[kv.first] != kv.second) printf("   A-only/diff: %ls\n", kv.first.c_str());
        for (auto& kv : tb) if (!ta.count(kv.first)) printf("   B-only: %ls\n", kv.first.c_str());
        CHECK(ta == tb);
    }

    printf("[6] states excluded vs included\n");
    useA();
    write_file(P(states_dir("n64", "mp1")) / "slot1.st", "state-1", 20);
    r = sync_folder(cl, false);
    CHECK(r.pushed == 0);
    r = sync_folder(cl, true);
    dump(r);
    CHECK(r.pushed == 2);  // slot0 + slot1
    CHECK(read_file(cloud / "n64/mp1/states/slot1.st") == "state-1");
    useB();
    r = sync_folder(cl, true);
    CHECK(r.pulled == 2);
    CHECK(read_file(B / "n64/mp1/states/slot0.st") == "state-A");

    printf("[7] unicode names\n");
    useA();
    write_file(P(battery_dir("gba", u8"マリオ")) / fs::u8path("セーブ é.sav"), "uni", 10);
    write_file(P(battery_dir("gba", u8"マリオ")) / fs::u8path("sub ünï/deep.sav"), "deep", 10);
    r = sync_folder(cl, false);
    CHECK(r.ok && r.pushed == 2);
    useB();
    r = sync_folder(cl, false);
    CHECK(r.ok && r.pulled == 2);
    CHECK(read_file(B / fs::u8path("gba/マリオ/battery/セーブ é.sav")) == "uni");
    CHECK(read_file(B / fs::u8path("gba/マリオ/battery/sub ünï/deep.sav")) == "deep");

    printf("[8] long path\n");
    {
        useA();
        std::string deep(60, 'd');
        fs::path lp = P(battery_dir("n64", "mp1")) / deep / deep / deep / deep / deep / "long.sav";
        std::wstring ext = L"\\\\?\\" + fs::absolute(lp).wstring();
        fs::create_directories(fs::path(ext).parent_path());
        { std::ofstream o(fs::path(ext), std::ios::binary); o << "long"; }
        r = sync_folder(cl, false);
        dump(r);
        CHECK(r.ok && r.pushed == 1);
        useB();
        r = sync_folder(cl, false);
        CHECK(r.ok && r.pulled == 1);
        fs::path bl = B / "n64/mp1/battery" / deep / deep / deep / deep / deep / "long.sav";
        std::ifstream in(fs::path(L"\\\\?\\" + fs::absolute(bl).wstring()), std::ios::binary);
        std::stringstream s; s << in.rdbuf();
        CHECK(s.str() == "long");
    }

    printf("[9] export -> import round trip into empty vault\n");
    useA();
    sync_folder(cl, false);
    std::string zip = (base / fs::u8path("backup テスト.zip")).u8string();
    std::string err;
    CHECK(export_zip(zip, true, &err));
    CHECK(fs::exists(base / fs::u8path("backup テスト.zip")) && fs::file_size(base / fs::u8path("backup テスト.zip")) > 100);
    useC();
    r = import_zip(zip);
    dump(r);
    CHECK(r.ok && r.conflicts == 0 && r.pulled > 0);
    CHECK(tree(A) == tree(C));
    CHECK(!tree(C).empty());
    r = import_zip(zip);
    CHECK(r.ok && r.pulled == 0 && r.conflicts == 0);   // idempotent
    // no-states export
    CHECK(export_zip((base / "nostates.zip").u8string(), false, &err));
    set_vault_dir((base / "pcD").u8string());
    r = import_zip((base / "nostates.zip").u8string());
    CHECK(r.ok);
    CHECK(!fs::exists(base / "pcD/n64/mp1/states"));
    CHECK(fs::exists(base / "pcD/n64/mp1/battery/mp1.eep"));
    // import conflict: local differs and is newer
    useC();
    write_file(C / "n64/mp1/battery/mp1.eep", "C-local-newer", 0);
    r = import_zip(zip);
    CHECK(r.ok && r.conflicts == 1);
    CHECK(read_file(C / "n64/mp1/battery/mp1.eep") == "C-local-newer");
    CHECK(count_conflicts(C) >= 1);
    // bad zip
    r = import_zip((base / "nostates.zip.missing").u8string());
    CHECK(!r.ok);

    printf("[10] sync folder equal to vault is rejected\n");
    useA();
    r = sync_folder(A.u8string(), false);
    CHECK(!r.ok);

    printf("[11] the account secret never travels: sync folder, export and import (F6)\n");
    {
        const std::string secret = "TOPSECRET-DO-NOT-LEAK";
        auto idj = [&](const char* tag) {
            return std::string("{\"user_id\":\"") + tag + "\",\"secret\":\"" + secret + "-" + tag + "\",\"created\":1000}\n";
        };
        const char* rel = "_identity/identity.json";
        fs::path V1 = base / "idV1", V2 = base / "idV2", V3 = base / "idV3", sf = base / "idsync";
        /* a vault with a real save plus every shape of identity file */
        write_file(V1 / "n64/zelda/battery/zelda.eep", "zelda-save", 100);
        write_file(V1 / rel, idj("me"), 50);
        write_file(V1 / "_identity/old-pc.json", idj("old"), 50);
        write_file(V1 / "n64/zelda/battery/Identity-Backup-1.JSON", "lookalike-save", 50);   // a game save that merely has such a name: it is a save, it travels
        write_file(V1 / "_conflicts/_identity/identity.json.OLDPC.20261101-120000", idj("loser"), 50);   // what older builds kept as the losing identity of a sync conflict
        set_vault_dir(V1.u8string());
        r = sync_folder(sf.u8string(), false);
        dump(r);
        CHECK(r.ok && r.pushed == 2 && r.conflicts == 0);                    // the save and the look-alike, never the identity files
        { bool note = false; for (auto& l : r.log) if (l.find("Settings > Account > Add this account to another PC") != std::string::npos) note = true; CHECK(note); }   // F6: the player is told
        CHECK(read_file(sf / "n64/zelda/battery/zelda.eep") == "zelda-save");
        CHECK(!fs::exists(sf / "_identity") && fs::exists(sf / "n64/zelda/battery/Identity-Backup-1.JSON") && !fs::exists(sf / "_conflicts"));
        /* a sync folder written by an older build still holds an identity: ignored, never pulled, and left in place (we never delete in the user's folder) */
        write_file(sf / rel, idj("foreign"), 5);
        set_vault_dir(V2.u8string());
        r = sync_folder(sf.u8string(), false);
        CHECK(r.ok && r.pulled == 2 && r.conflicts == 0);
        CHECK(!fs::exists(V2 / "_identity") && read_file(V2 / "n64/zelda/battery/zelda.eep") == "zelda-save");
        CHECK(read_file(sf / rel) == idj("foreign"));
        /* a vault that has its own identity keeps it untouched, even when the folder's is older or newer */
        write_file(V2 / rel, idj("mine"), 500);
        r = sync_folder(sf.u8string(), false);
        CHECK(r.ok && r.pulled == 0 && r.pushed == 0 && r.conflicts == 0 && read_file(V2 / rel) == idj("mine"));
        CHECK(count_conflicts(V2) == 0);
        /* export: no identity entry, and the secret is in no entry at all */
        set_vault_dir(V1.u8string());
        std::string ez = (base / "id-export.zip").u8string();
        CHECK(export_zip(ez, true, &err));
        {
            ZipView z(ez);
            CHECK(z.ok && z.has("n64/zelda/battery/zelda.eep"));
            CHECK(!z.name_contains("_identity") && z.has("n64/zelda/battery/Identity-Backup-1.JSON"));
            CHECK(!z.body_contains("TOPSECRET"));
        }
        /* a hostile zip carries identity entries in several spellings: all skipped (and logged), the real save comes in, the identity stays */
        std::string hz = (base / "id-hostile.zip").u8string();
        CHECK(make_zip(hz, {{"_identity/identity.json", idj("attacker")}, {"_Identity/other.json", idj("attacker2")},
                            {"n64/pkmn/battery/identity.json", idj("attacker3")}, {"_conflicts/_identity/identity.json.PC.20260101-000000", idj("attacker4")},
                            {"n64/pkmn/battery/pkmn.sav", "pokemon-save"}}));
        set_vault_dir(V2.u8string());
        r = import_zip(hz);
        dump(r);
        int skipped_logged = 0;
        for (auto& l : r.log) if (l.find("never imported") != std::string::npos) skipped_logged++;
        CHECK(r.ok && r.pulled == 2 && r.conflicts == 0 && skipped_logged == 3);
        CHECK(read_file(V2 / rel) == idj("mine"));                           // the account did not change
        CHECK(!fs::exists(V2 / "_identity/other.json") && fs::exists(V2 / "n64/pkmn/battery/identity.json"));   // (NTFS: _Identity is the same folder as _identity)
        CHECK(read_file(V2 / "n64/pkmn/battery/pkmn.sav") == "pokemon-save");
        /* the "new PC imports a friend's zip" case: an empty vault gets the save and no account at all */
        set_vault_dir(V3.u8string());
        r = import_zip(hz);
        CHECK(r.ok && r.pulled == 2 && !fs::exists(V3 / "_identity") && !fs::exists(V3 / "_conflicts"));
        CHECK(read_file(V3 / "n64/pkmn/battery/pkmn.sav") == "pokemon-save");
    }

    printf("[nds] DS saves: vault key/path wiring and cross-PC sync (retrorun writes <rom stem>.srm into battery_dir)\n");
    {
        const std::string key = "nds:adae-05ca37";   // library key: game code + ROM version + header CRC16
        fs::path N1 = base / "ndsA", N2 = base / "ndsB", ncloud = base / "ndscloud";
        set_vault_dir(N1.u8string());
        std::string bd = battery_dir("nds", key);
        CHECK(bd == N1.u8string() + "\\nds\\nds_adae-05ca37\\battery\\");   // ':' is sanitized, same on every PC
        CHECK(states_dir("nds", key) == N1.u8string() + "\\nds\\nds_adae-05ca37\\states\\");
        std::string save(524288, '\xFF');
        save.replace(0, 8, "DSSAVE01");
        write_file(P(bd) / "Pokemon - Diamond Version (USA) (Rev 5).srm", save, 60);
        write_file(P(states_dir("nds", key)) / "auto.state", std::string(4096, 'x'), 60);
        touch_game("nds", key, "Pokemon Diamond");
        SyncReport nr = sync_folder(ncloud.u8string(), false);
        CHECK(nr.ok && nr.pushed == 2 && nr.conflicts == 0);   // .srm + meta.json; states stay local
        CHECK(!fs::exists(ncloud / "nds/nds_adae-05ca37/states"));
        set_vault_dir(N2.u8string());
        nr = sync_folder(ncloud.u8string(), false);
        CHECK(nr.ok && nr.pulled == 2 && nr.conflicts == 0);
        CHECK(read_file(N2 / "nds/nds_adae-05ca37/battery/Pokemon - Diamond Version (USA) (Rev 5).srm") == save);
        CHECK(battery_dir("nds", key) == N2.u8string() + "\\nds\\nds_adae-05ca37\\battery\\");
    }

    printf("[12] one-side-changed: an older / empty incoming file never overwrites a newer one without a backup copy\n");
    {
        fs::path V = base / "s1V", sf = base / "s1sync";
        set_vault_dir(V.u8string());
        auto conflicts_of = [&](const fs::path& root, const std::wstring& what) {
            int n = 0;
            for (auto& kv : tree(root)) if (kv.first.rfind(L"_conflicts/", 0) == 0 && kv.first.find(what) != std::wstring::npos) n++;
            return n;
        };
        auto conflict_of = [&](const fs::path& root, const std::wstring& what) {
            for (auto& kv : tree(root)) if (kv.first.rfind(L"_conflicts/", 0) == 0 && kv.first.find(what) != std::wstring::npos) return kv.second;
            return std::string("<none>");
        };
        write_file(V / "n64/g/battery/x.sav", "new-vault-save", 100);
        write_file(V / "n64/g/battery/y.sav", "good-vault-save", 100);
        write_file(V / "n64/g/battery/z.sav", "z-v1", 100);
        write_file(V / "n64/g/battery/w.sav", "w-old-vault", 5000);
        SyncReport s = sync_folder(sf.u8string(), false);
        CHECK(s.ok && s.pushed == 4 && s.conflicts == 0);
        /* the cloud client restored an old version: only the sync-folder side differs from the manifest; the winner stays the same */
        write_file(sf / "n64/g/battery/x.sav", "old-restored", 5000);
        /* a half-written (empty) file that is even NEWER than the vault's */
        write_file(sf / "n64/g/battery/y.sav", "", 50);
        /* a normal, newer edit from the other PC */
        write_file(sf / "n64/g/battery/z.sav", "z-v2", 10);
        /* the other direction: the vault file changed to something OLDER than the sync folder's unchanged copy */
        write_file(V / "n64/g/battery/w.sav", "w-restored-older", 9000);
        s = sync_folder(sf.u8string(), false);
        dump(s);
        CHECK(s.ok);
        CHECK(read_file(V / "n64/g/battery/x.sav") == "old-restored");     // who wins is unchanged
        CHECK(read_file(V / "n64/g/battery/y.sav") == "");
        CHECK(read_file(V / "n64/g/battery/z.sav") == "z-v2");
        CHECK(read_file(sf / "n64/g/battery/w.sav") == "w-restored-older");
        CHECK(conflicts_of(V, L"x.sav") == 1 && conflict_of(V, L"x.sav") == "new-vault-save" && conflicts_of(sf, L"x.sav") == 1);
        CHECK(conflicts_of(V, L"y.sav") == 1 && conflict_of(V, L"y.sav") == "good-vault-save");
        CHECK(conflicts_of(V, L"w.sav") == 1 && conflict_of(V, L"w.sav") == "w-old-vault");   // the overwritten sync-folder copy is on both sides
        CHECK(conflict_of(sf, L"w.sav") == "w-old-vault");
        CHECK(conflicts_of(V, L"z.sav") == 0 && conflicts_of(sf, L"z.sav") == 0);             // normal newer save: no extra copy
        s = sync_folder(sf.u8string(), false);
        CHECK(s.ok && s.pushed == 0 && s.pulled == 0 && s.conflicts == 0);                    // settles
    }

    printf("[13] an enumeration error changes nothing; an unexpectedly present destination is never replaced\n");
    {
        fs::path V = base / "s2V", sf = base / "s2sync";
        set_vault_dir(V.u8string());
        write_file(V / "n64/g/battery/a.sav", "a-v1", 100);
        write_file(V / "n64/g/battery/b.sav", "b-v1", 100);
        write_file(V / "n64/g/battery/c.sav", "c-v1", 100);
        write_file(V / "n64/h/battery/h.sav", "h-v1", 100);   // a second game: it must keep syncing while game g cannot be read
        SyncReport s = sync_folder(sf.u8string(), false);
        CHECK(s.ok && s.pushed == 4);
        write_file(V / "n64/g/battery/a.sav", "a-edit", 10);
        write_file(V / "n64/g/battery/b.sav", "b-edit", 10);
        write_file(V / "n64/g/battery/c.sav", "c-edit", 10);
        write_file(V / "n64/h/battery/h.sav", "h-edit", 10);
        auto same = [&](const char* tag) {
            CHECK(read_file(V / "n64/g/battery/a.sav") == "a-edit"); CHECK(read_file(V / "n64/g/battery/b.sav") == "b-edit"); CHECK(read_file(V / "n64/g/battery/c.sav") == "c-edit");
            CHECK(read_file(sf / "n64/g/battery/a.sav") == "a-v1" && read_file(sf / "n64/g/battery/b.sav") == "b-v1" && read_file(sf / "n64/g/battery/c.sav") == "c-v1");
            CHECK(count_conflicts(V) == 0 && count_conflicts(sf) == 0);
            (void)tag;
        };
        /* the vault's battery folder cannot be opened (antivirus / sync-client lock) */
        g_hook_mode = 1; g_hook_match = "s2V\\n64\\g";
        set_enum_error_hook(enum_hook);
        s = sync_folder(sf.u8string(), false);
        dump(s);
        CHECK(s.ok && s.error.empty() && s.warning.find("n64/g") != std::string::npos);   // game g is skipped and reported, by name
        CHECK(s.pushed == 1 && s.pulled == 0);                                            // game h still synced
        CHECK(read_file(sf / "n64/h/battery/h.sav") == "h-edit");
        same("open");
        /* the listing breaks after the first file */
        g_hook_mode = 2;
        s = sync_folder(sf.u8string(), false);
        CHECK(s.ok && s.warning.find("n64/g") != std::string::npos && s.pushed == 0 && s.pulled == 0);
        same("mid");
        /* the sync folder's listing breaks */
        g_hook_mode = 2; g_hook_match = "s2sync\\n64\\g";
        s = sync_folder(sf.u8string(), false);
        CHECK(s.ok && s.warning.find("n64/g") != std::string::npos && s.pushed == 0 && s.pulled == 0);
        same("root");
        /* no error: the normal push of the edits works again */
        g_hook_mode = 0;
        s = sync_folder(sf.u8string(), false);
        CHECK(s.ok && s.pushed == 3 && s.conflicts == 0);
        CHECK(read_file(sf / "n64/g/battery/b.sav") == "b-edit");

        /* a file that appears in the sync folder right after it was listed is not overwritten by the "only the vault has it" push */
        write_file(V / "n64/g/battery/late.sav", "vault-late", 10);
        g_hook_mode = 3; g_hook_match = "s2sync\\n64\\g";
        s = sync_folder(sf.u8string(), false);
        dump(s);
        set_enum_error_hook(nullptr);
        CHECK(s.ok && s.conflicts == 1);
        CHECK(read_file(sf / "n64/g/battery/late.sav") == "vault-late");      // the newer one wins
        { bool kept = false; for (auto& kv : tree(sf)) if (kv.first.rfind(L"_conflicts/", 0) == 0 && kv.second == "cloud-late") kept = true; CHECK(kept); }
        { bool kept = false; for (auto& kv : tree(V)) if (kv.first.rfind(L"_conflicts/", 0) == 0 && kv.second == "cloud-late") kept = true; CHECK(kept); }
    }

    printf("[14] cloud / OS junk files are not synced, in either direction, and nothing is deleted\n");
    {
        fs::path V = base / "s3V", sf = base / "s3sync";
        set_vault_dir(V.u8string());
        const char* junk[] = {"desktop.ini", "Thumbs.db", ".DS_Store", "~$lock.sav", ".~lock.real.sav#", ".dropbox", ".dropbox.attr", ".sync", ".syncthing.real.sav.tmp", "x.crdownload", "x.sav.!sync"};
        for (const char* j : junk) write_file(V / "n64/j/battery" / j, std::string("junk-v-") + j, 100);
        write_file(V / "n64/j/battery/real.sav", "real", 100);
        write_file(V / "n64/j/battery/keep.tmp", "tmp-save", 100);      // a game's own file name: still a save
        write_file(V / "n64/j/battery/game.sav~", "tilde-save", 100);
        write_file(sf / "n64/j/battery/THUMBS.DB", "junk-from-cloud", 100);
        write_file(sf / "n64/j/battery/Desktop.INI", "junk-from-cloud", 100);
        SyncReport s = sync_folder(sf.u8string(), false);
        dump(s);
        CHECK(s.ok && s.pushed == 3 && s.pulled == 0 && s.conflicts == 0);
        for (const char* j : junk) CHECK(!fs::exists(sf / "n64/j/battery" / j) || std::string(j) == "Thumbs.db" || std::string(j) == "desktop.ini");
        CHECK(read_file(sf / "n64/j/battery/real.sav") == "real" && read_file(sf / "n64/j/battery/keep.tmp") == "tmp-save" && read_file(sf / "n64/j/battery/game.sav~") == "tilde-save");
        CHECK(read_file(sf / "n64/j/battery/THUMBS.DB") == "junk-from-cloud");   // left alone in the folder, never deleted
        CHECK(read_file(V / "n64/j/battery/desktop.ini") == "junk-v-desktop.ini" && fs::exists(V / "n64/j/battery/.DS_Store"));   // and in the vault
        CHECK(!fs::exists(V / "n64/j/battery/Thumbs.db") || read_file(V / "n64/j/battery/Thumbs.db") == "junk-v-Thumbs.db");
        { std::ifstream t(sf / "n64/j/battery/Desktop.INI", std::ios::binary); CHECK((bool)t); }
        { bool pulled = false; for (auto& kv : tree(V)) if (kv.second == "junk-from-cloud") pulled = true; CHECK(!pulled); }
    }

    printf("[15] a listing error outside the save trees does not fail the sync; in a system folder or _conflicts it does\n");
    {
        fs::path V = base / "s4V", sf = base / "s4sync";
        set_vault_dir(V.u8string());
        write_file(V / "n64/g/battery/g.sav", "g-v1", 100);
        write_file(sf / "foreign stuff/sub/x.dat", "foreign-cloud", 100);       // a top-level folder that is no system
        SyncReport s = sync_folder(sf.u8string(), false);
        CHECK(s.ok && s.pushed == 1 && s.pulled == 1);
        write_file(V / "n64/g/battery/g.sav", "g-v2", 10);
        set_enum_error_hook(enum_hook);
        g_hook_mode = 1; g_hook_tail = "foreign stuff"; g_hook_match = "s4sync";
        s = sync_folder(sf.u8string(), false);
        dump(s);
        CHECK(s.ok && s.error.empty() && s.pushed == 1);                         // the rest synced
        CHECK(read_file(sf / "n64/g/battery/g.sav") == "g-v2");
        { bool said = false; for (auto& l : s.log) if (l.find("foreign stuff") != std::string::npos && l.find("skip") != std::string::npos) said = true; CHECK(said); }
        /* the system folder itself, or _conflicts, cannot be listed: nothing can be told apart, the whole sync stops and changes nothing */
        write_file(V / "n64/g/battery/g.sav", "g-v3", 5);
        g_hook_tail = "s4V\\n64"; g_hook_match = "s4V\\n64";
        s = sync_folder(sf.u8string(), false);
        CHECK(!s.ok && !s.error.empty() && s.pushed == 0 && read_file(sf / "n64/g/battery/g.sav") == "g-v2");
        g_hook_tail = "s4sync\\_conflicts"; g_hook_match = "s4sync";
        fs::create_directories(sf / "_conflicts");
        s = sync_folder(sf.u8string(), false);
        CHECK(!s.ok && !s.error.empty() && s.pushed == 0 && read_file(sf / "n64/g/battery/g.sav") == "g-v2");
        g_hook_mode = 0; g_hook_tail = "\\battery";
        set_enum_error_hook(nullptr);
    }

    printf("[16] a half-failed backup removes the vault copy it just made (no pile-up on retries)\n");
    {
        fs::path V = base / "s5V", sf = base / "s5sync";
        set_vault_dir(V.u8string());
        write_file(V / "n64/g/battery/x.sav", "newer-vault", 100);
        SyncReport s = sync_folder(sf.u8string(), false);
        CHECK(s.ok && s.pushed == 1);
        write_file(sf / "n64/g/battery/x.sav", "old-restored", 5000);          // needs a backup of the vault file
        fs::create_directories(sf / "_conflicts");
        std::string user = getenv("USERNAME") ? getenv("USERNAME") : "";
        std::string cmd = "icacls \"" + (sf / "_conflicts").string() + "\" /deny \"" + user + "\":(OI)(CI)(WD,AD) >nul 2>&1";
        bool denied = !user.empty() && system(cmd.c_str()) == 0;
        bool really = false;
        if (denied) { std::ofstream t(sf / "_conflicts/probe.txt"); really = !t.good(); }
        if (!really) printf("  (could not make the folder unwritable here: the failing part of this test is skipped)\n");
        else {
            for (int i = 0; i < 3; i++) {
                s = sync_folder(sf.u8string(), false);
                CHECK(s.ok && s.pulled == 0);
                CHECK(read_file(V / "n64/g/battery/x.sav") == "newer-vault");   // left untouched
                CHECK(count_conflicts(V) == 0);                                  // no stray vault-side copy
            }
        }
        std::string reset = "icacls \"" + (sf / "_conflicts").string() + "\" /remove:d \"" + user + "\" >nul 2>&1";
        if (denied) system(reset.c_str());
        s = sync_folder(sf.u8string(), false);                                   // writable again: backup made, same winner as before
        CHECK(s.ok && s.pulled == 1 && count_conflicts(V) == 1 && count_conflicts(sf) == 1 && read_file(V / "n64/g/battery/x.sav") == "old-restored");
    }

    { std::error_code ec; fs::remove_all(fs::path(L"\\\\?\\" + fs::absolute(base).wstring()), ec); }
    printf("\n%s: %d checks, %d failed\n", g_fail ? "FAILED" : "PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
