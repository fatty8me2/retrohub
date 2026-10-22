/* Console test for the save vault: simulates two PCs (two vault dirs) sharing one sync folder. */
#include "savestore.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

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
    CHECK(count_conflicts(cloud) == 2);
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

    printf("[11] identity: oldest 'created' wins on sync and import\n");
    {
        auto idj = [](const char* tag, long long created) {
            return std::string("{\"user_id\":\"") + tag + "\",\"secret\":\"s\",\"created\":" + std::to_string(created) + "}\n";
        };
        fs::path I1 = base / "idA", I2 = base / "idB", icloud = base / "idcloud";
        const char* rel = "_identity/identity.json";
        std::string orig = idj("orig", 1000), fresh = idj("fresh", 2000);
        // PC1 = original (OLD file mtime), PC2 = created later (NEW file mtime)
        write_file(I1 / rel, orig, 500);
        write_file(I2 / rel, fresh, 5);
        set_vault_dir(I1.u8string());
        r = sync_folder(icloud.u8string(), false);
        CHECK(r.ok && read_file(icloud / rel) == orig);
        set_vault_dir(I2.u8string());
        r = sync_folder(icloud.u8string(), false);
        dump(r);
        CHECK(r.ok && r.conflicts == 1);
        CHECK(read_file(I2 / rel) == orig);
        CHECK(read_file(icloud / rel) == orig);
        CHECK(count_conflicts(I2) == 1 && conflict_content(I2) == fresh);
        set_vault_dir(I1.u8string());
        r = sync_folder(icloud.u8string(), false);
        CHECK(r.ok && read_file(I1 / rel) == orig);   // never replaced by the newer identity
        // reverse order: the NEW PC syncs first, the original arrives second and still wins
        fs::path J1 = base / "idC", J2 = base / "idD", jcloud = base / "idcloud2";
        write_file(J1 / rel, orig, 500);
        write_file(J2 / rel, fresh, 5);
        set_vault_dir(J2.u8string());
        r = sync_folder(jcloud.u8string(), false);
        CHECK(r.ok && read_file(jcloud / rel) == fresh);
        set_vault_dir(J1.u8string());
        r = sync_folder(jcloud.u8string(), false);
        CHECK(r.ok && read_file(jcloud / rel) == orig && read_file(J1 / rel) == orig);
        set_vault_dir(J2.u8string());
        r = sync_folder(jcloud.u8string(), false);
        CHECK(r.ok && read_file(J2 / rel) == orig);
        // import_zip: zip holds the original, vault has the newer-mtime fresh one -> original wins
        fs::path Z1 = base / "idZ1", Z2 = base / "idZ2";
        write_file(Z1 / rel, orig, 500);
        set_vault_dir(Z1.u8string());
        CHECK(export_zip((base / "id.zip").u8string(), false, &err));
        write_file(Z2 / rel, fresh, 0);
        set_vault_dir(Z2.u8string());
        r = import_zip((base / "id.zip").u8string());
        CHECK(r.ok && read_file(Z2 / rel) == orig && count_conflicts(Z2) == 1 && conflict_content(Z2) == fresh);
        // import_zip the other way: zip holds the newer identity, vault keeps the original
        write_file(Z1 / rel, orig, 0);
        set_vault_dir(Z2.u8string());
        write_file(Z2 / rel, orig, 500);
        fs::remove_all(Z2 / "_conflicts");
        write_file(Z1 / "_identity/identity.json", fresh, 0);
        set_vault_dir(Z1.u8string());
        CHECK(export_zip((base / "id2.zip").u8string(), false, &err));
        set_vault_dir(Z2.u8string());
        r = import_zip((base / "id2.zip").u8string());
        CHECK(r.ok && read_file(Z2 / rel) == orig && conflict_content(Z2) == fresh);
    }

    { std::error_code ec; fs::remove_all(fs::path(L"\\\\?\\" + fs::absolute(base).wstring()), ec); }
    printf("\n%s: %d checks, %d failed\n", g_fail ? "FAILED" : "PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
