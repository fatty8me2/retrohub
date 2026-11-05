#pragma once
/* The GBA battery save in the exact shape retrorun's mGBA core keeps in <battery>\<stem>.srm.
 *
 * Checked against the bundled mGBA 0.10 libretro core (src/platform/libretro/libretro.c): retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) is the
 * bare save, retro_get_memory_size() is GBASavedataSize(): 512 / 8192 (EEPROM), 32768 (SRAM), 65536 / 131072 (Flash), 131072 while the type is
 * still undetected. The clock is NOT part of it (libretro exposes RETRO_MEMORY_RTC for Game Boy only; a GBA cartridge clock is simply not saved).
 * Desktop mGBA, Dolphin's integrated GBA and FlashGBX write the same bytes plus a 16 byte clock block (GBASavedataRTCBuffer: time[7], control,
 * lastLatch) right after the save, so a Ruby / Sapphire / Emerald / Boktai .sav is canonical+16 bytes. retrorun would load such a file with a
 * "size mismatch" warning and keep an extra .bak, so every writer into the vault cuts the block off. The block is only cut when what is left is
 * exactly a size the core uses, so nothing else is ever trimmed. Nothing the core reads is lost: it never reads the block. */
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>

namespace gbasave {

constexpr size_t kRtcFooter = 16;
inline bool is_core_size(size_t n) {
    static const size_t canon[] = {512, 8192, 32768, 65536, 131072};
    return std::find(std::begin(canon), std::end(canon), n) != std::end(canon);
}
/* True (and `b` cut) when `b` is a core-size save plus the 16 byte clock block. */
inline bool strip_rtc_footer(std::string* b) {
    if (b->size() > kRtcFooter && !is_core_size(b->size()) && is_core_size(b->size() - kRtcFooter)) { b->resize(b->size() - kRtcFooter); return true; }
    return false;
}

}  // namespace gbasave
