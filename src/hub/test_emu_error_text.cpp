#include "emu_error_text.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static bool contains(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

int main() {
    using hubemu::plain_install_failure;
    const std::string az = "Azahar";
    /* every exact string install_core can produce maps to something plain and never echoes the raw text */
    const char* raws[] = {
        "network unavailable", "server answered 404", "cannot write C:\\x\\a.part", "cancelled", "download interrupted", "disk write failed",
        "could not reach github.com", "cannot open the download", "the download is not a valid zip", "damaged zip entry",
        "unsafe path in the zip: ..\\evil.dll", "cannot write bin/a.dll", "cannot extract bin/a.dll", "tar.exe not found (needs Windows 10 or later)",
        "unpacking failed (tar exit 1)", "unsafe path in the download: ../x", "the download is empty", "the download contains a link entry: l foo",
        "the download listing is inconsistent", "cannot write the upgrade journal", "could not move a.dll aside (is something using it?)",
        "could not install a.dll", "azahar.exe missing after the update", "an earlier Azahar update could not be finished yet. Close Azahar if it is open, then try again",
        "Azahar is not published yet (its download has not been set up)", "download has the wrong size (12 bytes)",
        "checksum mismatch: the download was rejected", "cannot move the download into place", "azahar.exe not found in the download",
        "Azahar is running: close it, then update", "azahar.exe missing after install",
    };
    for (const char* r : raws) {
        std::string t = plain_install_failure(r, az);
        CHECK(!t.empty());
        CHECK(!contains(t, "tar exit") && !contains(t, "checksum mismatch") && !contains(t, ".dll") && !contains(t, ".exe") && !contains(t, "journal"));
        CHECK(t.find("Something went wrong") == std::string::npos);   // each known text has its own sentence
    }
    CHECK(contains(plain_install_failure("checksum mismatch: the download was rejected", az), "safety check"));
    CHECK(contains(plain_install_failure("download has the wrong size (5 bytes)", az), "Nothing was installed"));
    CHECK(contains(plain_install_failure("download interrupted", az), "try again"));
    CHECK(contains(plain_install_failure("network unavailable", az), "internet connection"));
    CHECK(contains(plain_install_failure("disk write failed", az), "free space"));
    CHECK(contains(plain_install_failure("Azahar is running: close it, then update", az), "Close Azahar"));
    CHECK(contains(plain_install_failure("cannot extract a/b", az), "free space"));
    /* appended detail (an OS reason, tar's last line) must not change the wording */
    for (const char* r : raws) {
        const std::string base = plain_install_failure(r, az);
        if (std::string(r).find("could not all be put back") != std::string::npos) continue;
        CHECK(plain_install_failure(std::string(r) + ": Access is denied. (5)", az) == base);
    }
    CHECK(contains(plain_install_failure("server answered 404", az), "not available right now"));
    CHECK(contains(plain_install_failure("server answered 503", az), "not available right now"));
    CHECK(contains(plain_install_failure("cannot open the download: Access is denied. (5)", az), "antivirus"));
    CHECK(!contains(plain_install_failure("cannot open the download", az), "damaged"));
    /* old-version claims: only for an update, never when the restore failed */
    CHECK(!contains(plain_install_failure("could not install a.dll", az, false), "old version"));
    CHECK(contains(plain_install_failure("could not install a.dll", az, true), "old version was kept"));
    CHECK(!contains(plain_install_failure("azahar.exe missing after install", az, false), "old version"));
    {
        std::string t = plain_install_failure("could not install a.dll (and the old files could not all be put back; RetroHub will retry at the next start)", az, true);
        CHECK(!contains(t, "was kept") && contains(t, "install it again"));
        t = plain_install_failure("could not move a.dll aside (is something using it?) (and the old files could not all be put back; RetroHub will retry at the next start)", az, true);
        CHECK(!contains(t, "was kept") && contains(t, "install it again"));
    }
    CHECK(contains(plain_install_failure("something new", az), "Something went wrong"));
    CHECK(contains(plain_install_failure("", az), "Something went wrong"));
    CHECK(plain_install_failure("cancelled", az) == "The download was stopped.");
    for (const char* r : raws) { std::string t = plain_install_failure(r, az); CHECK(t.find("\xE2\x80\x94") == std::string::npos && t.find("\xE2\x80\x93") == std::string::npos); }
    if (!fails) std::printf("emu_error_text_test: all passed\n");
    return fails ? 1 : 0;
}
