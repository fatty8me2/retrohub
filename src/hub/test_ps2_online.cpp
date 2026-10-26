#include "ps2_online.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "emulators.h"
#include "logic.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubps2;

int main() {
    /* ---- readiness reasons: core download, BIOS, unpack, in that order ---- */
    OnlineState s;
    s.bios_found = true;
    CHECK(online_block_reason(s).find("Getting the PlayStation 2 online core") == 0);
    s.core_downloading = true; s.percent = 42;
    CHECK(online_block_reason(s) == "Downloading the PlayStation 2 online core (42%)...");
    s.core_downloading = false; s.download_error = "checksum mismatch: the download was rejected";
    CHECK(online_block_reason(s).find("could not be downloaded: checksum mismatch") != std::string::npos);
    s.core_present = true; s.bios_found = false;
    CHECK(online_block_reason(s).find("no PlayStation 2 BIOS") != std::string::npos);
    s.bios_found = true; s.needs_unpack = true; s.title = "Tekken 5";
    CHECK(online_block_reason(s) == "Unpacking Tekken 5 (needed for PlayStation 2 online)...");
    s.needs_unpack = false;
    CHECK(online_block_reason(s).empty());

    /* ---- retrorun arguments ---- */
    CHECK(retrorun_setup_args("scph70012.bin", "SLUS-21059", true) == " --system ps2 --opt pcsx2_bios=scph70012.bin --setup-tag SLUS-21059 --share-host-save");
    CHECK(retrorun_setup_args("a.bin", "", false) == " --system ps2 --opt pcsx2_bios=a.bin");
    CHECK(vault_card_rel("ps2_slus-21059") == "memcards\\RetroHub\\ps2_slus-21059\\Mcd001.ps2");

    /* ---- exit codes -> player text ---- */
    CHECK(std::string(setup_mismatch_text(9)).find("BIOS") != std::string::npos);
    CHECK(std::string(setup_mismatch_text(10)).find("disc") != std::string::npos);
    CHECK(std::string(setup_mismatch_text(11)).find("core") != std::string::npos);
    CHECK(std::string(setup_mismatch_text(7)).empty());
    for (unsigned long c : {9ul, 10ul, 11ul}) CHECK(hubui::exit_code_text(c) == setup_mismatch_text(c));

    /* ---- the pinned LRPS2 download (docs/PS2_ONLINE.md) ---- */
    const hubemu::Release& r = hubemu::release(hubemu::Kind::Lrps2);
    CHECK(r.size == 3760634ull);
    CHECK(std::string(r.sha256) == "730112622345f0ca12e7650c6d4052f443603aa630e9b6ab8a68874a4d66bae6");
    CHECK(std::wstring(r.host) == L"buildbot.libretro.com" && std::wstring(r.exe) == L"pcsx2_libretro.dll");
    CHECK(std::string(hubemu::name(hubemu::Kind::Lrps2)) == "PS2 online core");
    CHECK(hubemu::managed_dir(hubemu::Kind::Lrps2, "R").find("lrps2") != std::string::npos);
    CHECK(hubemu::kind_for(hub::System::PS2) == hubemu::Kind::Pcsx2);   // offline PS2 keeps using PCSX2

    /* Live download from the buildbot (only with RETROHUB_QA_DOWNLOAD=1; ctest -L online sets nothing, so offline runs never touch the network). */
    if (const char* d = std::getenv("RETROHUB_QA_DOWNLOAD"); d && d[0] == '1') {
        std::string root = (std::filesystem::temp_directory_path() / "rh_lrps2_dl").string();
        std::filesystem::remove_all(root);
        std::string err;
        bool ok = hubemu::install_blocking(hubemu::Kind::Lrps2, root, &err, nullptr);
        if (!ok) std::printf("download failed: %s\n", err.c_str());
        CHECK(ok);
        hubemu::Install in = hubemu::find(hubemu::Kind::Lrps2, "", "", root);
        CHECK(!in.exe.empty() && in.managed);
        std::error_code ec;
        CHECK(!in.exe.empty() && std::filesystem::file_size(in.exe, ec) == 11074048);
        std::filesystem::remove_all(root);
    }
    std::printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
