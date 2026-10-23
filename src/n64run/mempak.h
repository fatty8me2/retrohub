#pragma once
/* Controller Pak (mempak) policy for n64run. Pure logic, unit tested (test_mempak.cpp).
 *
 * Offline: every present port gets a Controller Pak. mupen64plus writes it as "<ROM name>-<CRC>.mpk"
 * into SaveSRAMPath, which is the --save-dir the hub passes (the vault battery folder), so packs sync
 * across PCs with the other battery saves.
 *
 * Online: packs are only enabled when host-save sharing is active (--share-host-save). Then the host's
 * .mpk is sent to every guest with the other battery saves and every PC boots from the same pak bytes.
 * Without sharing the guests have no pak, so packs stay off (PLUGIN_NONE) to avoid desync. */

#include <string>

namespace n64run {

// True when the input plugin should attach a Controller Pak (PLUGIN_MEMPAK) to each present port.
bool mempak_enabled(bool online, bool share_host_save);

// True for a file name the save sync may carry as a Controller Pak: a plain battery name ending in .mpk.
bool is_mempak_file(const std::string& name);

}  // namespace n64run
