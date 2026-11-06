/* Console test for slot2.h: refusal rules, subsystem pick, copy-back rule, backup pruning. */
#include <cstdio>
#include <string>

#include "slot2.h"

using namespace retrorun::slot2;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
    RunMode m;
    CHECK(refuse_reason(m, false, false).empty());           // no flags: nothing to say
    CHECK(refuse_reason(m, true, false).empty());
    CHECK(refuse_reason(m, true, true).empty());
    CHECK(!refuse_reason(m, false, true).empty());           // save without rom
    { RunMode o = m; o.net = true;  CHECK(!refuse_reason(o, true, true).empty()); CHECK(refuse_reason(o, true, true).find("offline") != std::string::npos); }
    { RunMode o = m; o.ds_link = true; CHECK(refuse_reason(o, true, true).find("wireless") != std::string::npos); }
    { RunMode o = m; o.rollback = true; CHECK(!refuse_reason(o, true, false).empty()); }
    { RunMode o = m; o.share_host_save = true; CHECK(!refuse_reason(o, true, false).empty()); }
    { RunMode o = m; o.synctest = true; CHECK(!refuse_reason(o, true, false).empty()); }
    { RunMode o = m; o.net = true; CHECK(refuse_reason(o, false, false).empty()); }   // no slot2 flags: online play is not our business
    { RunMode o = m; o.is_ds = false; CHECK(!refuse_reason(o, true, true).empty()); }

    std::vector<Sub> subs = {{"melondsds_slot_1_2_boot", "Slot 1 & 2 Boot", 0x101, 3}, {"melondsds_slot_1_2_boot_no_sram", "Slot 1 & 2 Boot (No GBA Save Data)", 0x102, 2}, {"x", "Other", 0x103, 3}};
    unsigned id = 0;
    CHECK(pick_game_type(subs, true, &id) && id == 0x101);
    CHECK(pick_game_type(subs, false, &id) && id == 0x102);
    CHECK(!pick_game_type({{"x", "Other", 1, 3}}, true, &id));
    CHECK(!pick_game_type({}, false, &id));

    std::vector<uint8_t> a(32768, 1), b(32768, 2), c(100, 3), e;
    CHECK(!should_copy_back(a, a));      // unchanged
    CHECK(should_copy_back(a, b));       // changed, same size
    CHECK(!should_copy_back(a, c));      // wrong size: keep the vault file
    CHECK(!should_copy_back(a, e));      // empty
    CHECK(should_copy_back(e, b));       // there was no save yet

    CHECK(backup_name("pokeruby.srm", "20261105-101500") == "pokeruby.srm.20261105-101500.bak");
    CHECK(is_backup_of("pokeruby.srm.20261105-101500.bak", "pokeruby.srm"));
    CHECK(!is_backup_of("pokeruby.srm", "pokeruby.srm"));
    CHECK(!is_backup_of("other.srm.20261105-101500.bak", "pokeruby.srm"));
    {   // two backups in the same second never take each other's place, and still sort oldest first
        std::vector<std::string> have;
        const std::string b1 = unique_backup_name("s.srm", "20261105-101500", have);
        CHECK(b1 == "s.srm.20261105-101500-01.bak" && is_backup_of(b1, "s.srm"));
        have.push_back(b1);
        const std::string b2 = unique_backup_name("s.srm", "20261105-101500", have);
        CHECK(b2 == "s.srm.20261105-101500-02.bak" && b2 != b1 && b1 < b2);
        have.push_back(b2);
        for (int i = 3; i < 10; ++i) have.push_back(unique_backup_name("s.srm", "20261105-101500", have));
        const std::string b10 = unique_backup_name("s.srm", "20261105-101500", have);
        CHECK(b10 == "s.srm.20261105-101500-10.bak" && have.back() < b10);
        CHECK(unique_backup_name("s.srm", "20261105-101501", have) == "s.srm.20261105-101501-01.bak");   // another second starts again
        CHECK(backups_to_prune({b2, b1, "s.srm.20261105-101459-01.bak"}, 2) == std::vector<std::string>{"s.srm.20261105-101459-01.bak"});
        std::vector<std::string> full;
        for (int i = 1; i < 100; ++i) full.push_back(unique_backup_name("s.srm", "t", full));
        CHECK(full.size() == 99 && unique_backup_name("s.srm", "t", full).empty());
    }
    {   // two backups in the same second never take each other's place, and still sort oldest first
        std::vector<std::string> have;
        const std::string b1 = unique_backup_name("s.srm", "20261105-101500", have);
        CHECK(b1 == "s.srm.20261105-101500-01.bak" && is_backup_of(b1, "s.srm"));
        have.push_back(b1);
        const std::string b2 = unique_backup_name("s.srm", "20261105-101500", have);
        CHECK(b2 == "s.srm.20261105-101500-02.bak" && b2 != b1 && b1 < b2);
        have.push_back(b2);
        for (int i = 3; i < 10; ++i) have.push_back(unique_backup_name("s.srm", "20261105-101500", have));
        const std::string b10 = unique_backup_name("s.srm", "20261105-101500", have);
        CHECK(b10 == "s.srm.20261105-101500-10.bak" && have.back() < b10);
        CHECK(unique_backup_name("s.srm", "20261105-101501", have) == "s.srm.20261105-101501-01.bak");   // another second starts again
        CHECK(backups_to_prune({b2, b1, "s.srm.20261105-101459-01.bak"}, 2) == std::vector<std::string>{"s.srm.20261105-101459-01.bak"});
        std::vector<std::string> full;
        for (int i = 1; i < 100; ++i) full.push_back(unique_backup_name("s.srm", "t", full));
        CHECK(full.size() == 99 && unique_backup_name("s.srm", "t", full).empty());
    }
    {   // two backups in the same second never take each other's place, and still sort oldest first
        std::vector<std::string> have;
        const std::string b1 = unique_backup_name("s.srm", "20261105-101500", have);
        CHECK(b1 == "s.srm.20261105-101500-01.bak" && is_backup_of(b1, "s.srm"));
        have.push_back(b1);
        const std::string b2 = unique_backup_name("s.srm", "20261105-101500", have);
        CHECK(b2 == "s.srm.20261105-101500-02.bak" && b2 != b1 && b1 < b2);
        have.push_back(b2);
        for (int i = 3; i < 10; ++i) have.push_back(unique_backup_name("s.srm", "20261105-101500", have));
        const std::string b10 = unique_backup_name("s.srm", "20261105-101500", have);
        CHECK(b10 == "s.srm.20261105-101500-10.bak" && have.back() < b10);
        CHECK(unique_backup_name("s.srm", "20261105-101501", have) == "s.srm.20261105-101501-01.bak");   // another second starts again
        CHECK(backups_to_prune({b2, b1, "s.srm.20261105-101459-01.bak"}, 2) == std::vector<std::string>{"s.srm.20261105-101459-01.bak"});
        std::vector<std::string> full;
        for (int i = 1; i < 100; ++i) full.push_back(unique_backup_name("s.srm", "t", full));
        CHECK(full.size() == 99 && unique_backup_name("s.srm", "t", full).empty());
    }
    std::vector<std::string> n = {"s.srm.20261105-000004.bak", "s.srm.20261105-000001.bak", "s.srm.20261105-000003.bak", "s.srm.20261105-000002.bak"};
    auto p = backups_to_prune(n, 3);
    CHECK(p.size() == 1 && p[0] == "s.srm.20261105-000001.bak");
    CHECK(backups_to_prune({"a", "b"}, 3).empty());
    CHECK(backups_to_prune(n, 0).size() == 4);

    printf(g_fail ? "retrorun_slot2_test: %d FAILED\n" : "retrorun_slot2_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
