// QA harness (agent/hs-qa-saves): the Hookshot save path against Lichii through ONE vault folder.
// Scenario: PC A plays with Lichii -> PC A plays with Hookshot -> PC B plays with Lichii, then a three-way Lichii -> Hookshot -> Longshot -> Lichii run
// through one vault, two PCs (A plays Longshot, B plays Hookshot), then edge cases (empty vault with a
// populated Hookshot folder, a vault file newer than Hookshot's, read-only files on either side).
// Only temp dirs are written. The real save files are READ from the backup folder (argv[1]) and copied; that folder is
// checked byte-for-byte before and after. hookshot::user_dir() is exe_dir\userdata\emulators\hookshot\user, so run this
// from the QA build folder (never from a RetroHub install) and it writes nowhere else.
#include <windows.h>
#include "emulators.h"
#include "games.h"
#include "hookshot.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        } else {                                                        \
            ++g_pass;                                                   \
        }                                                               \
    } while (0)

static std::string U8(const fs::path& p) { return p.u8string(); }

static std::string rd(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void wr(const fs::path& p, const std::string& t) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(t.data(), (std::streamsize)t.size());
}
static void set_mtime(const fs::path& p, fs::file_time_type t) {
    std::error_code ec;
    fs::last_write_time(p, t, ec);
}
static void set_readonly(const fs::path& p, bool ro) {
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_write, ro ? fs::perm_options::remove : fs::perm_options::add, ec);
}
static bool has_text(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

static hub::GameEntry oot() {
    hub::GameEntry e;
    e.system = hub::System::Switch;
    e.key = "switch:0400486024dec000";
    return e;
}
static hubemu::Install lichii_at(const fs::path& dir) {
    hubemu::Install i;
    i.exe = U8(dir / "lichii.exe");
    i.dir = U8(dir);
    i.how = "qa harness";
    return i;
}

using Snap = std::map<std::string, std::string>;   // relative path -> content
static Snap snap(const fs::path& root) {
    Snap m;
    std::error_code ec;
    if (!fs::exists(root, ec)) return m;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) m[U8(fs::relative(it->path(), root, ec))] = rd(it->path());
    return m;
}
// Nothing is ever deleted: every path present in `before` is still there in `after` (content may change).
static void check_none_deleted(const Snap& before, const Snap& after, const char* what) {
    for (const auto& kv : before) {
        if (!after.count(kv.first)) {
            std::printf("FAIL %s: deleted %s\n", what, kv.first.c_str());
            ++g_fail;
        } else {
            ++g_pass;
        }
    }
}
static void check_no_tmp(const fs::path& root, const char* what) {
    std::error_code ec;
    bool any = false;
    if (fs::exists(root, ec))
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".hubtmp") { any = true; std::printf("  leftover: %s\n", U8(it->path()).c_str()); }
    CHECK(!any);
    (void)what;
}

// The 14 real files, relative to the backup root (e.g. "slot_02\\progress.sav").
static std::vector<std::string> real_files(const fs::path& real) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(real, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) out.push_back(U8(fs::relative(it->path(), real, ec)));
    std::sort(out.begin(), out.end());
    return out;
}
static void copy_real(const fs::path& real, const std::vector<std::string>& files, const fs::path& dst) {
    for (const auto& rel : files) {
        std::error_code ec;
        fs::create_directories(fs::u8path(rel).parent_path().empty() ? dst : (dst / fs::u8path(rel)).parent_path(), ec);
        fs::copy_file(real / fs::u8path(rel), dst / fs::u8path(rel), fs::copy_options::overwrite_existing, ec);
    }
}
static bool is_progress_slot2(const std::string& rel) { return rel == "slot_02\\progress.sav"; }

struct Vault {
    fs::path root, battery, tid, conflicts, syncdir;
    fs::path lichii_manifest() const { return syncdir / "emu-switch-switch_0400486024dec000.json"; }
    fs::path hookshot_manifest() const { return syncdir / "emu-switch-switch_0400486024dec000-hookshot.json"; }
    fs::path longshot_manifest() const { return syncdir / "emu-switch-switch_0400486024dec000-longshot.json"; }
};
static Vault make_vault(const fs::path& root) {
    Vault v;
    v.root = root;
    v.battery = root / "switch" / "switch_0400486024dec000" / "battery";
    v.tid = v.battery / "save" / "0400486024DEC000";
    v.conflicts = root / "_conflicts";
    v.syncdir = root / ".sync";
    return v;
}
static fs::path hookshot_user() { return fs::u8path(hookshot::user_dir()); }
static fs::path hookshot_save() { return hookshot_user() / hookshot::kSaveRel; }
static fs::path longshot_user() { return fs::u8path(hookshot::user_dir(hookshot::Flavor::Longshot)); }
static fs::path longshot_save() { return longshot_user() / hookshot::kSaveRel; }

static int count_backups_named(const fs::path& dir, const std::string& prefix, const std::string& content) {
    int n = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return 0;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && U8(it->path().filename()).compare(0, prefix.size(), prefix) == 0 && rd(it->path()) == content) ++n;
    return n;
}

static void clear_readonly_tree(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) set_readonly(it->path(), false);
}

int main(int argc, char** argv) {
    const fs::path real = argc > 1 ? fs::u8path(argv[1])
                                   : fs::u8path("C:\\Users\\fatty\\AppData\\Local\\RetroHub\\save-backups\\oot-switch2-before-hookshot-2026-11-07\\lichii-nand");
    const fs::path root = fs::temp_directory_path() / ("hs_qa_saves_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    fs::remove_all(fs::u8path(hookshot::game_dir()), ec);   // QA data folder only (under this exe's userdata)
    fs::remove_all(fs::u8path(hookshot::game_dir(hookshot::Flavor::Longshot)), ec);

    const std::vector<std::string> files = real_files(real);
    CHECK(files.size() == 14);
    const Snap real_before = snap(real);
    const hub::GameEntry e = oot();
    const Vault V = make_vault(root / "vault");
    const fs::path pcA = root / "pcA" / "lichii", pcB = root / "pcB" / "lichii";

    // ---- 1. PC A plays with Lichii: NAND holds the real files, end_session puts them in the vault
    hubemu::SaveSession a1 = hubemu::begin_session(e, lichii_at(pcA), U8(V.battery), "", 0);
    CHECK(a1.active && a1.error.empty());
    CHECK(a1.rels.size() == 1 && a1.vault_rels.size() == 1 && a1.vault_rels[0] == "save/0400486024DEC000");
    const fs::path nandA = fs::u8path(a1.emu_root) / fs::u8path(a1.rels[0]);
    copy_real(real, files, nandA);                    // "the game saved": the real files
    hubemu::end_session(a1);
    CHECK(a1.error.empty());
    std::printf("  step1 lichii end: %s\n", a1.summary.c_str());
    CHECK(has_text(a1.summary, "14 save file(s) copied into the vault"));
    for (const auto& rel : files) CHECK(rd(V.tid / fs::u8path(rel)) == rd(real / fs::u8path(rel)));
    CHECK(fs::exists(V.lichii_manifest()));
    const std::string lman1 = rd(V.lichii_manifest());
    const Snap after1 = snap(root);

    // ---- 2. PC A plays with Hookshot
    fs::remove_all(hookshot_user(), ec);   // fresh Hookshot folder on PC A
    hubemu::SaveSession h2 = hookshot::begin_save_session(e, U8(V.battery));
    CHECK(h2.active && h2.error.empty());
    CHECK(h2.rels.size() == 1 && h2.rels[0] == "save/5a3c9e1f0b7d24680000000000000001");
    CHECK(h2.vault_rels.size() == 1 && h2.vault_rels[0] == "save/0400486024DEC000");
    CHECK(fs::u8path(h2.manifest_path).filename() == "emu-switch-switch_0400486024dec000-hookshot.json");
    for (const auto& rel : files)
        CHECK(rd(hookshot_save() / fs::u8path(rel)) == rd(real / fs::u8path(rel)));   // vault -> Hookshot, byte-identical
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const std::string played = "HOOKSHOT-PLAYED-SLOT2 new bytes from Hookshot " + std::string(4096, 'H');
    wr(hookshot_save() / "slot_02" / "progress.sav", played);
    set_mtime(hookshot_save() / "slot_02" / "progress.sav", fs::file_time_type::clock::now());
    const Snap hs_mid = snap(hookshot_save());
    hubemu::end_session(h2);
    CHECK(h2.error.empty());
    std::printf("  step2 hookshot end: %s\n", h2.summary.c_str());
    CHECK(has_text(h2.summary, "1 save file(s) copied into the vault"));
    CHECK(rd(V.tid / "slot_02" / "progress.sav") == played);   // Hookshot -> vault
    for (const auto& rel : files)
        if (!is_progress_slot2(rel)) CHECK(rd(V.tid / fs::u8path(rel)) == rd(real / fs::u8path(rel)));
    CHECK(rd(V.lichii_manifest()) == lman1);                   // Hookshot never touches Lichii's manifest
    CHECK(!fs::exists(V.battery / "save" / "5a3c9e1f0b7d24680000000000000001"));   // vault holds the title id folder only
    check_none_deleted(hs_mid, snap(hookshot_save()), "hookshot folder after end_session");
    check_none_deleted(after1, snap(root), "step 2 (PC A Hookshot)");

    // ---- 3. PC B plays with Lichii: its empty NAND receives the Hookshot-made bytes
    const Snap vault_before_b = snap(V.tid);
    hubemu::SaveSession b3 = hubemu::begin_session(e, lichii_at(pcB), U8(V.battery), "", 0);
    CHECK(b3.active && b3.error.empty());
    const fs::path nandB = fs::u8path(b3.emu_root) / fs::u8path(b3.rels[0]);
    for (const auto& rel : files) {
        const std::string want = is_progress_slot2(rel) ? played : rd(real / fs::u8path(rel));
        CHECK(rd(nandB / fs::u8path(rel)) == want);
    }
    hubemu::end_session(b3);
    CHECK(b3.error.empty());
    std::printf("  step3 lichii B end: %s\n", b3.summary.c_str());
    CHECK(snap(V.tid) == vault_before_b);                      // nothing changed by a PC B session with no play
    check_none_deleted(vault_before_b, snap(V.tid), "step 3 (PC B Lichii)");

    // ---- 4. the overwritten vault copy (the real slot_02) is in _conflicts
    {
        const int n = count_backups_named(V.conflicts, "progress.sav.local.", rd(real / "slot_02" / "progress.sav"));
        CHECK(n == 1);
        CHECK(count_backups_named(V.conflicts, "progress.sav.local.", played) == 0);
    }

    // ---- 5. Lichii's and Hookshot's manifests are separate files
    CHECK(fs::exists(V.lichii_manifest()) && fs::exists(V.hookshot_manifest()));
    CHECK(V.lichii_manifest() != V.hookshot_manifest());
    CHECK(has_text(rd(V.hookshot_manifest()), "save/0400486024DEC000/slot_02\\progress.sav"));
    CHECK(has_text(rd(V.lichii_manifest()), "save/0400486024DEC000/slot_02\\progress.sav"));

    // ---- 6. nothing deleted anywhere so far, and no stray .hubtmp files
    check_none_deleted(after1, snap(root), "whole scenario");
    check_no_tmp(root, "scenario");
    CHECK(snap(real) == real_before);   // the real backup was only read

    // ---- Edge A: empty vault, populated Hookshot folder
    {
        const Vault VA = make_vault(root / "edgeA" / "vault");
        fs::create_directories(VA.battery);   // the battery folder exists, nothing in it
        fs::remove_all(hookshot_user(), ec);
        copy_real(real, files, hookshot_save());
        wr(hookshot_save() / "hs_only.sav", "HS-ONLY");
        const Snap before = snap(hookshot_save());
        hubemu::SaveSession ha = hookshot::begin_save_session(e, U8(VA.battery));
        CHECK(ha.active && ha.error.empty());
        CHECK(snap(hookshot_save()) == before);                 // begin did not touch Hookshot's files
        hubemu::end_session(ha);
        CHECK(ha.error.empty());
        std::printf("  edgeA end: %s\n", ha.summary.c_str());
        CHECK(has_text(ha.summary, "15 save file(s) copied into the vault"));
        for (const auto& kv : before) CHECK(rd(VA.tid / fs::u8path(kv.first)) == kv.second);
        check_none_deleted(before, snap(hookshot_save()), "edge A hookshot folder");
    }

    // ---- Edge B: a vault file newer than Hookshot's (no agreed manifest yet), then the reverse
    {
        const Vault VB = make_vault(root / "edgeB" / "vault");
        wr(VB.tid / "slot_00" / "progress.sav", "VAULT-NEWER");
        set_mtime(VB.tid / "slot_00" / "progress.sav", fs::file_time_type::clock::now());
        fs::remove_all(hookshot_user(), ec);
        wr(hookshot_save() / "slot_00" / "progress.sav", "HS-OLDER");
        set_mtime(hookshot_save() / "slot_00" / "progress.sav", fs::file_time_type::clock::now() - std::chrono::hours(1));
        hubemu::SaveSession hb = hookshot::begin_save_session(e, U8(VB.battery));
        CHECK(hb.active && hb.error.empty());
        CHECK(rd(hookshot_save() / "slot_00" / "progress.sav") == "VAULT-NEWER");   // newer vault copy wins
        const int bk = count_backups_named(VB.conflicts, "progress.sav.local.", "HS-OLDER");
        CHECK(bk == 1);                                                        // Hookshot's overwritten copy kept
        std::printf("  edgeB begin: %s\n", hb.summary.c_str());
        // reverse: Hookshot's copy is now the newer one; it must win on the way back
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        wr(hookshot_save() / "slot_00" / "progress.sav", "HS-NEWER");
        set_mtime(hookshot_save() / "slot_00" / "progress.sav", fs::file_time_type::clock::now());
        hubemu::end_session(hb);
        CHECK(hb.error.empty());
        CHECK(rd(VB.tid / "slot_00" / "progress.sav") == "HS-NEWER");
        CHECK(count_backups_named(VB.conflicts, "progress.sav.local.", "VAULT-NEWER") == 1);
    }

    // ---- Edge C1: read-only file in the Hookshot folder, the vault copy is newer (Hookshot's copy must be the destination)
    {
        const Vault VC = make_vault(root / "edgeC1" / "vault");
        wr(VC.tid / "slot_00" / "progress.sav", "VAULT-NEWER-RO");
        set_mtime(VC.tid / "slot_00" / "progress.sav", fs::file_time_type::clock::now());
        fs::remove_all(hookshot_user(), ec);
        wr(hookshot_save() / "slot_00" / "progress.sav", "HS-RO-OLD");
        set_mtime(hookshot_save() / "slot_00" / "progress.sav", fs::file_time_type::clock::now() - std::chrono::hours(1));
        set_readonly(hookshot_save() / "slot_00" / "progress.sav", true);
        hubemu::SaveSession c1 = hookshot::begin_save_session(e, U8(VC.battery));
        std::printf("  edgeC1 begin: active=%d error='%s' summary='%s' file='%s'\n", (int)c1.active, c1.error.c_str(), c1.summary.c_str(),
                    rd(hookshot_save() / "slot_00" / "progress.sav").c_str());
        // observed: the read-only flag does NOT stop the newer vault copy replacing it (rename over it succeeds); the old bytes are backed up first
        CHECK(rd(hookshot_save() / "slot_00" / "progress.sav") == "VAULT-NEWER-RO");
        CHECK(c1.error.empty());
        CHECK(count_backups_named(VC.conflicts, "progress.sav.local.", "HS-RO-OLD") == 1);   // the read-only copy was backed up before it was replaced
        set_readonly(hookshot_save() / "slot_00" / "progress.sav", false);
        hubemu::end_session(c1);
    }

    // ---- Edge C2: read-only file in the Hookshot folder is a valid source at end_session
    {
        const Vault VD = make_vault(root / "edgeC2" / "vault");
        fs::remove_all(hookshot_user(), ec);
        wr(hookshot_save() / "slot_01" / "progress.sav", "RO-SRC");
        set_readonly(hookshot_save() / "slot_01" / "progress.sav", true);
        hubemu::SaveSession d2 = hookshot::begin_save_session(e, U8(VD.battery));
        CHECK(d2.active && d2.error.empty());
        hubemu::end_session(d2);
        CHECK(d2.error.empty());
        CHECK(rd(VD.tid / "slot_01" / "progress.sav") == "RO-SRC");
        set_readonly(hookshot_save() / "slot_01" / "progress.sav", false);
    }

    // ---- Edge C3: read-only vault file, Hookshot's copy is newer (the vault is the destination at end_session)
    {
        const Vault VE = make_vault(root / "edgeC3" / "vault");
        wr(VE.tid / "slot_00" / "progress.sav", "VAULT-RO");
        fs::remove_all(hookshot_user(), ec);
        hubemu::SaveSession e3 = hookshot::begin_save_session(e, U8(VE.battery));
        CHECK(e3.active && e3.error.empty());
        set_readonly(VE.tid / "slot_00" / "progress.sav", true);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        wr(hookshot_save() / "slot_00" / "progress.sav", "HS-NEW-VS-RO-VAULT");
        set_mtime(hookshot_save() / "slot_00" / "progress.sav", fs::file_time_type::clock::now());
        hubemu::end_session(e3);
        std::printf("  edgeC3 end: error='%s' vault='%s'\n", e3.error.c_str(), rd(VE.tid / "slot_00" / "progress.sav").c_str());
        // observed: the read-only vault file is replaced (no error); its old bytes must be backed up first
        CHECK(e3.error.empty());
        CHECK(rd(VE.tid / "slot_00" / "progress.sav") == "HS-NEW-VS-RO-VAULT");
        CHECK(count_backups_named(VE.conflicts, "progress.sav.local.", "VAULT-RO") == 1);
        set_readonly(VE.tid / "slot_00" / "progress.sav", false);
    }

    // ---- Three-way: Lichii -> Hookshot -> Longshot -> Lichii through ONE vault
    {
        const Vault V3 = make_vault(root / "three" / "vault");
        const fs::path pc = root / "three" / "lichii";
        auto nap = [] { std::this_thread::sleep_for(std::chrono::milliseconds(60)); };
        fs::remove_all(hookshot_user(), ec);
        fs::remove_all(longshot_user(), ec);

        hubemu::SaveSession l1 = hubemu::begin_session(e, lichii_at(pc), U8(V3.battery), "", 0);
        CHECK(l1.active && l1.error.empty());
        copy_real(real, files, fs::u8path(l1.emu_root) / fs::u8path(l1.rels[0]));
        hubemu::end_session(l1);
        CHECK(l1.error.empty());
        for (const auto& rel : files) CHECK(rd(V3.tid / fs::u8path(rel)) == rd(real / fs::u8path(rel)));

        // Hookshot receives Lichii's save, plays slot_02
        hubemu::SaveSession h = hookshot::begin_save_session(e, U8(V3.battery), hookshot::Flavor::Hookshot);
        CHECK(h.active && h.error.empty());
        for (const auto& rel : files) CHECK(rd(hookshot_save() / fs::u8path(rel)) == rd(real / fs::u8path(rel)));
        nap();
        wr(hookshot_save() / "slot_02" / "progress.sav", "THREEWAY-HOOKSHOT");
        set_mtime(hookshot_save() / "slot_02" / "progress.sav", fs::file_time_type::clock::now());
        hubemu::end_session(h);
        CHECK(h.error.empty() && rd(V3.tid / "slot_02" / "progress.sav") == "THREEWAY-HOOKSHOT");

        // Longshot (fresh folder) receives what Hookshot played, plays slot_02 again
        hubemu::SaveSession ls = hookshot::begin_save_session(e, U8(V3.battery), hookshot::Flavor::Longshot);
        CHECK(ls.active && ls.error.empty());
        CHECK(fs::u8path(ls.emu_root) == longshot_user() && fs::u8path(ls.manifest_path) == V3.longshot_manifest());
        for (const auto& rel : files)
            CHECK(rd(longshot_save() / fs::u8path(rel)) == (is_progress_slot2(rel) ? std::string("THREEWAY-HOOKSHOT") : rd(real / fs::u8path(rel))));
        nap();
        wr(longshot_save() / "slot_02" / "progress.sav", "THREEWAY-LONGSHOT");
        set_mtime(longshot_save() / "slot_02" / "progress.sav", fs::file_time_type::clock::now());
        hubemu::end_session(ls);
        CHECK(ls.error.empty() && rd(V3.tid / "slot_02" / "progress.sav") == "THREEWAY-LONGSHOT");
        CHECK(rd(hookshot_save() / "slot_02" / "progress.sav") == "THREEWAY-HOOKSHOT");   // Hookshot's own folder keeps its copy until it next starts

        // Lichii comes back: its NAND holds the older real file, the vault the Longshot one
        hubemu::SaveSession l2 = hubemu::begin_session(e, lichii_at(pc), U8(V3.battery), "", 0);
        CHECK(l2.active && l2.error.empty());
        const fs::path nand = fs::u8path(l2.emu_root) / fs::u8path(l2.rels[0]);
        for (const auto& rel : files) CHECK(rd(nand / fs::u8path(rel)) == (is_progress_slot2(rel) ? std::string("THREEWAY-LONGSHOT") : rd(real / fs::u8path(rel))));
        hubemu::end_session(l2);
        CHECK(l2.error.empty());
        // and Hookshot picks the Longshot save up again too (round trip back)
        hubemu::SaveSession h2b = hookshot::begin_save_session(e, U8(V3.battery), hookshot::Flavor::Hookshot);
        CHECK(h2b.active && rd(hookshot_save() / "slot_02" / "progress.sav") == "THREEWAY-LONGSHOT");
        hubemu::end_session(h2b);

        // three manifests, three files; the older vault copies are backed up in _conflicts; nothing deleted
        CHECK(fs::exists(V3.lichii_manifest()) && fs::exists(V3.hookshot_manifest()) && fs::exists(V3.longshot_manifest()));
        CHECK(V3.longshot_manifest() != V3.hookshot_manifest() && V3.longshot_manifest() != V3.lichii_manifest());
        CHECK(has_text(rd(V3.longshot_manifest()), "save/0400486024DEC000/slot_02\\progress.sav"));
        CHECK(count_backups_named(V3.conflicts, "progress.sav.local.", "THREEWAY-HOOKSHOT") >= 1);
        CHECK(!fs::exists(V3.battery / "save" / "5a3c9e1f0b7d24680000000000000001"));
        check_no_tmp(root / "three", "three-way");
    }

    // ---- Two PCs: PC A plays Longshot, PC B plays Hookshot, same vault (synced folder)
    {
        const Vault V4 = make_vault(root / "twopc" / "vault");
        const fs::path pcA2 = root / "twopc" / "A-lichii";
        auto nap = [] { std::this_thread::sleep_for(std::chrono::milliseconds(60)); };
        // the vault starts with the real save (put there by Lichii on PC A, earlier)
        hubemu::SaveSession l0 = hubemu::begin_session(e, lichii_at(pcA2), U8(V4.battery), "", 0);
        copy_real(real, files, fs::u8path(l0.emu_root) / fs::u8path(l0.rels[0]));
        hubemu::end_session(l0);
        CHECK(l0.error.empty());

        // PC A: Longshot only
        fs::remove_all(hookshot_user(), ec);
        fs::remove_all(longshot_user(), ec);
        hubemu::SaveSession a = hookshot::begin_save_session(e, U8(V4.battery), hookshot::Flavor::Longshot);
        CHECK(a.active && a.error.empty());
        for (const auto& rel : files) CHECK(rd(longshot_save() / fs::u8path(rel)) == rd(real / fs::u8path(rel)));
        nap();
        wr(longshot_save() / "slot_01" / "progress.sav", "PC-A-LONGSHOT");
        set_mtime(longshot_save() / "slot_01" / "progress.sav", fs::file_time_type::clock::now());
        hubemu::end_session(a);
        CHECK(a.error.empty() && rd(V4.tid / "slot_01" / "progress.sav") == "PC-A-LONGSHOT");


        // PC B: a different PC, no Longshot folder, Hookshot only
        fs::remove_all(longshot_user(), ec);
        fs::remove_all(hookshot_user(), ec);
        hubemu::SaveSession b = hookshot::begin_save_session(e, U8(V4.battery), hookshot::Flavor::Hookshot);
        CHECK(b.active && b.error.empty());
        CHECK(!fs::exists(longshot_save()));                                         // PC B never made a Longshot folder
        CHECK(rd(hookshot_save() / "slot_01" / "progress.sav") == "PC-A-LONGSHOT");   // PC A's Longshot progress arrives in PC B's Hookshot
        nap();
        wr(hookshot_save() / "slot_00" / "progress.sav", "PC-B-HOOKSHOT");
        set_mtime(hookshot_save() / "slot_00" / "progress.sav", fs::file_time_type::clock::now());
        hubemu::end_session(b);
        CHECK(b.error.empty() && rd(V4.tid / "slot_00" / "progress.sav") == "PC-B-HOOKSHOT");

        // PC A again: its own Longshot folder (restore it) picks up PC B's Hookshot progress and keeps its own
        fs::remove_all(hookshot_user(), ec);
        wr(longshot_save() / "slot_01" / "progress.sav", "PC-A-LONGSHOT");   // PC A kept only what it played
        hubemu::SaveSession a2 = hookshot::begin_save_session(e, U8(V4.battery), hookshot::Flavor::Longshot);
        CHECK(a2.active && a2.error.empty());
        CHECK(rd(longshot_save() / "slot_00" / "progress.sav") == "PC-B-HOOKSHOT");
        CHECK(rd(longshot_save() / "slot_01" / "progress.sav") == "PC-A-LONGSHOT");
        hubemu::end_session(a2);
        CHECK(a2.error.empty());
        CHECK(rd(V4.tid / "slot_00" / "progress.sav") == "PC-B-HOOKSHOT" && rd(V4.tid / "slot_01" / "progress.sav") == "PC-A-LONGSHOT");
        CHECK(!fs::exists(hookshot_save()));   // PC A never made a Hookshot folder again
        check_no_tmp(root / "twopc", "two PCs");
    }

    // ---- clean up: only this run's temp folders and this exe's QA data folder
    clear_readonly_tree(root);
    clear_readonly_tree(hookshot_user());
    clear_readonly_tree(longshot_user());
    fs::remove_all(root, ec);
    fs::remove_all(fs::u8path(hookshot::game_dir()), ec);
    fs::remove_all(fs::u8path(hookshot::game_dir(hookshot::Flavor::Longshot)), ec);

    std::printf("hookshot saves QA: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
