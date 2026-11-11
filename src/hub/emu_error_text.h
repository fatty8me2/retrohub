#pragma once
/* Plain-English wording for an emulator install failure. The installer (emulators.cpp) only exposes text, so this matches the exact
 * strings it produces by a stable prefix or key phrase. Pure, no UI: unit tested in test_emu_error_text.cpp. The raw text is still shown
 * under "Details" in the dialog so support can see it. */
#include <string>

namespace hubemu {

namespace detail {
inline bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
inline bool has(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }
}  // namespace detail

/* One or two short sentences: what happened and what the player can do. `emu` is the emulator's name (e.g. "Azahar"). */
inline std::string plain_install_failure(const std::string& raw, const std::string& emu) {
    using detail::has;
    using detail::starts;
    const std::string retry = " Check your internet connection, then try again.";
    if (raw == "cancelled") return "The download was stopped.";
    if (raw == "network unavailable" || starts(raw, "could not reach ") || starts(raw, "server answered "))
        return "RetroHub could not reach the download server." + retry;
    if (raw == "download interrupted") return "The download stopped part way." + retry;
    if (starts(raw, "download has the wrong size") || starts(raw, "checksum mismatch"))
        return "The download did not arrive complete, so RetroHub did not use it. Nothing was installed." + retry;
    if (raw == "disk write failed" || starts(raw, "cannot write ") || starts(raw, "cannot extract ") || raw == "cannot move the download into place" ||
        raw == "cannot write the upgrade journal")
        return "RetroHub could not save files to its data folder. Check that the disk has free space and that no other program is using the folder, then try again.";
    if (has(raw, "is running: close it") || starts(raw, "an earlier ") || has(raw, "(is something using it?)"))
        return emu + " is still open or in use, so it could not be updated. Close " + emu + ", then try again.";
    if (starts(raw, "could not install ") || has(raw, " missing after "))
        return "The files could not be put in place. Close " + emu + " if it is open, then try again. Your old version was kept.";
    if (raw == "cannot open the download" || raw == "the download is not a valid zip" || raw == "damaged zip entry" || raw == "the download is empty" ||
        raw == "the download listing is inconsistent" || starts(raw, "unpacking failed") || has(raw, " not found in the download"))
        return "The download could not be unpacked. It may have been damaged. Try again.";
    if (starts(raw, "unsafe path in the ") || starts(raw, "the download contains a link entry"))
        return "The download was not safe to unpack, so RetroHub did not use it. Nothing was installed.";
    if (starts(raw, "tar.exe not found")) return "This version of Windows is missing a tool RetroHub needs to unpack the download. Update Windows, then try again.";
    if (has(raw, "is not published yet")) return emu + " is not available to download yet. Please try again later.";
    return "Something went wrong during the install. Try again. If it keeps happening, tell support what the details below say.";
}

}  // namespace hubemu
