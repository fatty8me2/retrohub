#include "hack_logic.h"

/* hack.json <-> HackDef. PLACEHOLDER from the Wave 1 seams commit: the hacks lane (L7) implements these with tests in test_hack.cpp. */
namespace hubui {
namespace hacks {

std::string encode(const HackDef& d) { (void)d; return std::string(); }
bool decode(const std::string& text, HackDef* out) { (void)text; (void)out; return false; }

}  // namespace hacks
}  // namespace hubui
