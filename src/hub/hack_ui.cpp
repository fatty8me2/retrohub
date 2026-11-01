#include "app.h"
#include "hack_logic.h"

/* ROM hacks and translations in the library (Plan 4 H2-H4). STUB from the Wave 1 seams commit: the hacks lane (L7) owns this file
 * and replaces it. Contract: hack_logic.h (identity), common/patcher.h (patch engine), GameEntry::hack_patch / hack_base. */

void hack_scan_append(std::vector<hub::GameEntry>* out, const std::vector<std::string>& folders) { (void)out; (void)folders; }
bool hack_ensure_rom(const hub::GameEntry& e, std::string* patched_path) {
    (void)e; (void)patched_path;
    set_notice("ROM hacks are not available in this build yet.");
    return false;
}
void hack_draw() {}
void hack_shot_init() {}
