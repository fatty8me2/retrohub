#include "runahead.h"

namespace runahead {

int auto_frames(const std::string& s) {
    return (s == "nes" || s == "snes" || s == "gb" || s == "gbc" || s == "gba") ? 1 : 0;
}
int effective(int setting, const std::string& system) {
    if (setting == kAuto) return auto_frames(system);
    return setting < 0 ? 0 : setting > 2 ? 2 : setting;
}
bool parse(const std::string& s, int* setting) {
    if (s == "off" || s == "0") *setting = 0;
    else if (s == "1") *setting = 1;
    else if (s == "2") *setting = 2;
    else if (s == "auto") *setting = kAuto;
    else return false;
    return true;
}

void speculate(int n, const std::function<void()>& run, const std::function<void(bool)>& speculative,
               const std::function<bool(const void*, size_t)>& load, const std::vector<uint8_t>& saved) {
    if (n <= 0) return;
    if (speculative) speculative(true);
    for (int i = 0; i < n; i++) run();
    if (speculative) speculative(false);
    load(saved.data(), saved.size());   // the core is back on the real frame; the framebuffer keeps the speculative picture
}

}  // namespace runahead
