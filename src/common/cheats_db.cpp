/* STUB (agent/cheat-ui): see cheats_db.h. Replaced on merge. */
#include "cheats_db.h"

namespace cheatsdb {

std::vector<Entry> cheats_for(const hub::GameEntry& e) {
    std::vector<Entry> v;
    auto add = [&](const char* id, const char* desc, cheats::Format f, const char* code, bool feat) {
        Entry en;
        en.cheat.id = id;
        en.cheat.desc = desc;
        en.cheat.format = f;
        en.cheat.codes = {code};
        en.featured = feat;
        v.push_back(en);
    };
    if (e.system == hub::System::N64) {
        add("unlock-all", "Unlock all characters", cheats::Format::GameShark, "8033B21E 00FF", true);
        add("infinite-lives", "Infinite stocks", cheats::Format::GameShark, "81000000 0003", true);
        add("all-stages", "Unlock all stages", cheats::Format::GameShark, "8033B220 00FF", false);
        add("hitbox", "Show hitboxes", cheats::Format::GameShark, "81000010 0001", false);
    } else if (e.system == hub::System::GBA) {
        add("unlock-all", "Unlock everything", cheats::Format::ActionReplay, "82003000 0000FFFF", true);
        add("max-money", "Max money", cheats::Format::ActionReplay, "82003004 0000FFFF", false);
    } else if (e.system == hub::System::GB || e.system == hub::System::GBC) {
        add("inf-health", "Infinite health", cheats::Format::GameShark, "01FF5CD1", true);
        add("max-money", "Max money", cheats::Format::GameGenie, "0AF-4E1-3C1", false);
    }
    return v;
}

}  // namespace cheatsdb
