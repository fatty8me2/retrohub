#include "winutil.h"
#include <cctype>
#include <cstdlib>
#include <ctime>
#include "emulators.h"
#include "emu_http.h"
#include "psp.h"
#include "lichii.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <tlhelp32.h>
#include <cwctype>
#include <xinput.h>
#include <setupapi.h>
#include <initguid.h>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "controls.h"
#include "emu_saves.h"
#include "wiiu_meta.h"
#include "miniz.h"
#include "games.h"
#include "logic.h"
#include "qm_logic.h"
#include "dolphin_speed_logic.h"

namespace hubemu {

namespace fs = std::filesystem;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::string S(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
static std::wstring W(const std::string& s) { return P(s).wstring(); }
static std::string U(const std::wstring& w) { return S(fs::path(w)); }

static bool file_exists(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(P(p), ec);
}
static bool dir_exists(const std::string& p) {
    std::error_code ec;
    return fs::is_directory(P(p), ec);
}
static void make_dirs(const std::string& p) {
    std::error_code ec;
    fs::create_directories(P(p), ec);
}
static std::string read_text(const std::string& p) {
    std::ifstream f(P(p), std::ios::binary);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static bool write_text(const std::string& p, const std::string& t) {
    make_dirs(S(P(p).parent_path()));
    std::ofstream f(P(p), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(t.data(), (std::streamsize)t.size());
    return (bool)f;
}
static void remove_tree(const std::string& p) {
    std::error_code ec;
    fs::remove_all(P(p), ec);
}
static std::string env(const wchar_t* n) {
    wchar_t b[2048];
    DWORD len = GetEnvironmentVariableW(n, b, 2048);
    return (len > 0 && len < 2048) ? U(std::wstring(b, len)) : std::string();
}
static std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char c = a.back();
    return (c == '\\' || c == '/') ? a + b : a + "\\" + b;
}

const char* name(Kind k) { return k == Kind::Azahar ? "Azahar" : k == Kind::Cemu ? "Cemu" : k == Kind::Pcsx2 ? "PCSX2" : k == Kind::Lrps2 ? "PS2 online core" : k == Kind::Ppsspp ? "PPSSPP" : k == Kind::Lichii ? "Lichii" : k == Kind::Fritchii ? "Fritchii" : "Dolphin"; }
Kind kind_for(hub::System s) { return s == hub::System::WiiU ? Kind::Cemu : s == hub::System::GC ? Kind::Dolphin : s == hub::System::PS2 ? Kind::Pcsx2 : s == hub::System::PSP ? Kind::Ppsspp : s == hub::System::Switch ? Kind::Lichii : Kind::Azahar; }

/* Pinned official releases. Azahar: github.com/azahar-emu/azahar (GPL-3.0), asset digest as published by GitHub.
 * Cemu: github.com/cemu-project/Cemu (MPL-2.0); GitHub lists no digest for the Windows zip, so the SHA-256 was
 * computed from the official v2.6 asset when this was written (trust on first use). */
static const InstallTestHooks* g_test_hooks = nullptr;
void set_install_test_hooks(const InstallTestHooks* h) { g_test_hooks = h; }

/* ======================================================================================================================================================
 * FRITCHII PIN (v5.7). The one place the Fritchii download is defined: kPath, kSize, kSha256 (kVersion is the text the marker, Settings and the dialogs show).
 * Provenance: built LOCALLY on the owner's PC on 2026-11-10 (Visual Studio 2026, MSVC 14.50, Qt 6.8.3 msvc2022_64, the same configure line as the release-binaries
 * workflow's Windows job), because GitHub paused Actions on the fork ("Workflows are not currently being run on this fork due to the scale of GitHub Actions usage");
 * the owner chose a local build, so no workflow run exists for it. Source: fatty8me2/Fritchii branch retrohub/msvc (the one long-lived build branch from now on),
 * commit f411b88 (the r2 build) = Frizti's fritchii-v5.7 (52317bf) + 75ab24c (the MSVC warning flags, Windows only) + 6b21f52 (the Windows configure passes no -DLICHII_BUILD_TAG, so the
 * binary is named and versioned like Frizti's own release; the generated scm_rev says name "Fritchii", version "v5.7", and FritchiiUi::Enabled() is true: Fritchii's own
 * UI, Preparing shaders screen included) + f411b88 (see r2 below). v5.6 was skipped (never pinned). The "Convert to NX" tool is NOT in this build: the prebuilt nxformat-1.0.2-windows-x64.zip no
 * longer matched the SHA-256 pinned in Fritchii's CMakeModules/NxFormat.cmake (upstream re-published the asset), so configure skipped it; loading .nx games is unaffected.
 * The zip (50 entries, at the ROOT) holds lichii.exe, eden-cli.exe, the Qt DLLs and plugin folders, d3dcompiler_47.dll, dxcompiler.dll, dxil.dll, opengl32sw.dll, qt.conf,
 * Frizti's nx2_switches.txt (his default tuning, part of the vetted build: it is kept as shipped, RetroHub never writes to it or adds its own), the shader-preparation
 * recipe recipes\OoT v131072 (cache v54).nx2recipe (1,821,351 bytes, sha256 85d944c8...c3a1, unchanged since v5; Frizti's v5.7 zip carries the same file; it is not in the
 * source tree), and NEW the Visual C++ 14.50 runtime DLLs app-local (concrt140, msvcp140, msvcp140_1, msvcp140_2, msvcp140_atomic_wait, msvcp140_codecvt_ids, vccorlib140,
 * vcruntime140, vcruntime140_1, vcruntime140_threads), because MSVC 14.50 is newer than the runtime many PCs have installed. The install keeps every one of them (and the
 * recipes folder); the zip no longer has lichii.pdb, eden-cli.pdb or vc_redist.x64.exe, and the install's delete of those is a no-op when they are absent.
 * Release asset 66,270,333 bytes, SHA-256 1afa2247a5f4a5528db1ab97bc41b796a8773dfd9fc740ff59fc2296e7b61264. Defender clean. Vetted 2026-11-10
 * (source audits fritchii-v5.5..v5.6 and v5.6..v5.7: clean with notes; Fritchii-UI audit: kFritchiiWhatsNewSeen "v5.7" and the EDS2 pins in fritchii_ini_pin).
 * v5.7 facts: shader cache still v54, recipe format unchanged, "Preparing shaders" runs once after the update (the prepared stamp holds the build version); the tuned-settings
 * revision is 7 (RetroHub's 999 pin still holds it off); new built-in switches EDEN_NX2_JIT_WARM=1, EDEN_NX2_TC_CEILING_MIB=7680, EDEN_NX2_READ_THROTTLE=0 and experiment 49
 * (async GPU read-back) on do not conflict with what RetroHub sets.
 * r2 (2026-11-10, RetroHub 1.40.11; f411b88, the last commit above): the first v5.7 build (commit 6b21f52, 66,270,008 bytes, SHA-256 43ad6f48...139a) crashed with an access violation in
 * Shader::Optimization::BlitConstantsPass while preparing shaders (three dumps on the owner's PC, same instruction). Cause: an MSVC 14.50 code generation fault, not
 * Fritchii's code: the inlined IR::Value::U32() became one 64-bit load of the union, and the switch jump table was indexed with the whole register, whose upper half a
 * u32 immediate never wrote. f411b88 stores the narrow immediates (bool, u8, u16, u32, f32) through the 64-bit member, so that half is always zero; only value.cpp
 * changed, lichii.exe and eden-cli.exe are the only zip entries that differ, translated shaders are identical. kVersion says "v5.7 r2" so the dialogs show a change;
 * the binary still reports "v5.7" (kFritchiiWhatsNewSeen stays "v5.7"), so Preparing shaders resumes instead of starting over.
 * (Earlier pins: v5.5 = GitHub Actions run 42635153252 on retrohub/v5.5-msvc, Frizti's fritchii-v5.5 4012e52 + 5d3389f + 64f21c0, 120,118,919 bytes,
 * SHA-256 960ff5c4e1dd5119ff1cf526100f2e0fe263f22e5e12d0de0a07d6caed4ea831 (its zip also had lichii.pdb, eden-cli.pdb and vc_redist.x64.exe);
 * v5 = run 42557579406 on retrohub/v5-msvc, 62194f3 + 843ab8e, 119,936,811 bytes, SHA-256 30ad7db5ee8141647d388f741c7d64afefbb75009aaa647a3409b88797810d24;
 * v4.1 = run 42450099783 on retrohub/v4.1-msvc: Frizti's fritchii-v4.1 f6d42b8 plus build-flags-only commit 2a1f611, 119612370 bytes,
 * SHA-256 068fb4c66b2e32447ac7690393039043854de4b189aa7e95b34c4bf0d2a4080e.)
 * ====================================================================================================================================================== */
namespace fritchii_pin {
static const char* const kVersion = "v5.7 r2";
static const wchar_t* const kPath = L"/fatty8me2/Fritchii/releases/download/retrohub-fritchii-v5.7-r2/Fritchii-v5.7-RetroHub-r2-windows-x64.zip";
static const uint64_t kSize = 66270333ull;
static const char* const kSha256 = "1afa2247a5f4a5528db1ab97bc41b796a8773dfd9fc740ff59fc2296e7b61264";
}   // namespace fritchii_pin

const Release& release(Kind k) {
    if (g_test_hooks && g_test_hooks->release && g_test_hooks->kind == k) return *g_test_hooks->release;
    static const Release az = {
        "2126.2", L"github.com", L"/azahar-emu/azahar/releases/download/2126.2/azahar-windows-msvc-2126.2.zip",
        43941048ull, "4412eac8d13123c8840f11890fc26e5b0e4c76d16aa28a608941377b948a9db0", L"azahar.exe", "GPL-3.0"};
    static const Release ce = {
        "2.6", L"github.com", L"/cemu-project/Cemu/releases/download/v2.6/cemu-2.6-windows-x64.zip",
        26374093ull, "a6bcc2bc42a362d10213819948f3152fae7d47f70067f25939b51d3ddcfb0896", L"Cemu.exe", "MPL-2.0"};
    /* Dolphin 2509 (GPL-2.0+), the official x64 7z from dl.dolphin-emu.org; the SHA-256 was computed from that file. */
    static const Release dol = {
        "2509", L"dl.dolphin-emu.org", L"/releases/2509/dolphin-2509-x64.7z",
        18402050ull, "3a910cd09887bdc17df0d344de18249ac9c060d006649cd8660b791a5e8ba355", L"Dolphin.exe", "GPL-2.0+"};
    /* PCSX2 v2.8.2 stable (GPL-3.0+), github.com/PCSX2/pcsx2; the SHA-256 is GitHub's published asset digest. Portable via portable.ini. */
    static const Release ps = {
        "2.8.2", L"github.com", L"/PCSX2/pcsx2/releases/download/v2.8.2/pcsx2-v2.8.2-windows-x64-Qt.7z",
        25670075ull, "7dfc829ca1994cc1045ac49f05e39b6cf968b72e6a374c40e05c2a2b4ac200b4", L"pcsx2-qt.exe", "GPL-3.0+"};
    /* LRPS2 / "ParaLLEl PS2" v2.0.0-8f1d15f (GPL-3.0), pcsx2_libretro.dll from the libretro buildbot nightly of 2026-11-02, mirrored UNCHANGED as a release asset of
     * fatty8me2/retrohub (tag core-lrps2-8f1d15f) so the bytes never move: lockstep needs every peer on the byte-identical core, and the buildbot's "latest" URL
     * changes under us (that broke the old 38d3a83 pin). This build adds the pcsx2_multitap option (PS2 online with 3-4 players). Source (GPL-3.0):
     * https://github.com/libretro/ps2 at commit 8f1d15f. To move to a newer core: re-run the determinism + lockstep QA (docs/PS2_ONLINE.md), upload the new zip under
     * a new core-lrps2-<id> tag, and change this pin; there is deliberately no fallback URL (a different build would be refused anyway). Never shipped in the package. */
    static const Release lr = {
        "2.0.0-8f1d15f", L"github.com", L"/fatty8me2/retrohub/releases/download/core-lrps2-8f1d15f/pcsx2_libretro.dll.zip",
        3432192ull, "ff97bd2690e7a53433e1deb3570a72f9a06a78253f1db3796c8df6742aca7fe7", L"pcsx2_libretro.dll", "GPL-3.0"};
    /* PPSSPP v1.20.4 (GPL-2.0+), the official Windows zip from ppsspp.org (the "latest" link of ppsspp.org/download on 2026-11-01). ppsspp.org publishes no
     * digest, so the SHA-256 was computed from that exact file when this was written (trust on first use, like Cemu). The zip holds PPSSPPWindows64.exe at
     * its root; with no installed.txt beside the exe PPSSPP is portable: all data lives in <dir>\memstick. */
    static const Release pp = {
        "1.20.4", L"www.ppsspp.org", L"/files/1_20_4/ppsspp_win.zip",
        27658759ull, "a60f04ebdb0b5f1655422bd7f88349a46999b17ad5115d6ddb290c3934bd5163", L"PPSSPPWindows64.exe", "GPL-2.0+"};
    /* Lichii build 130 (GPL-3.0), github.com/zortos293/Lichii, tag build-130 (nx2 @ 3dae119, a full release published 2026-11-09 17:55 UTC; the zip's 42 files are byte-identical
     * to the CI artifact of Lichii's own run 42660458563, and the source diff cbb512e..3dae119 (build 125 to 130) was audited). An Eden fork that adds Switch 2 games. Over
     * build 125 it brings: asynchronous GPU read-back and back-thread macro reads (Ocarina of Time is faster), HostTiming threads that sleep instead of spinning, all-zero
     * guest pages released (about 6 GB less RAM), .nx archive support, and a new shipped nxformat.dll (prebuilt from github.com/fab918/nxformat V1.0.0, hash-pinned in Lichii's
     * CMake; it must stay). The shader cache is now "v2" (user\cache\shader\<title>\lichii-cache.v2.bin + .driver.bin): an existing vulkan.fork-v54 / v53 / v52 .bin is converted
     * once at the first start and left in place, never deleted. Its cache writer drains for ~3 s at exit, a queued log compaction of a big cache file can run on, and a
     * driver-blob store may drain for up to 60 s: see lichii_hang_watch (launch.cpp), which also requires no disk I/O before it ends a Lichii that lost its window.
     * Build 125 added mesh-shader output topologies 0 and 1 (Lichii PR #515) over build 95. lichii.exe is at the zip root and the zip ships no .pdb files (~89 MB).
     * Portable via a `user` folder beside the exe (lichii.h), which an upgrade from an older pin never touches (install_core). The zip also holds lichii-updater.exe,
     * Lichii's own self-updater: install_core deletes it, because RetroHub is the only updater (lichii_ini_launch also forces check_for_updates off). No games, keys or
     * firmware are included: the player supplies their own. The version string is what the marker, Settings and the dialogs show. */
    static const Release li = {
        "build 130", L"github.com", L"/zortos293/Lichii/releases/download/build-130/lichii-windows-x86_64.zip",
        89253414ull, "f09893b3041596662d762959a103ae4af40d479631309fff9990bcc288078eed", L"lichii.exe", "GPL-3.0"};
    /* Fritchii (GPL-3.0): Frizti's Ocarina-of-Time fork of Lichii, built by us (v5.7 r2 locally, earlier pins on GitHub Actions; github.com/fatty8me2/Fritchii). Same exe name (lichii.exe at the ROOT
     * of the zip, plus eden-cli.exe), same portable `user` folder, same qt-config.ini and save layout as Lichii. It lives in its OWN folder (userdata\emulators\fritchii)
     * with its OWN user\, never linked to Lichii's. The values come from the one block fritchii_pin below. */
    static const Release fr = {fritchii_pin::kVersion, L"github.com", fritchii_pin::kPath, fritchii_pin::kSize, fritchii_pin::kSha256, L"lichii.exe", "GPL-3.0"};
    return k == Kind::Azahar ? az : k == Kind::Cemu ? ce : k == Kind::Pcsx2 ? ps : k == Kind::Lrps2 ? lr : k == Kind::Ppsspp ? pp : k == Kind::Lichii ? li : k == Kind::Fritchii ? fr : dol;
}

bool release_is_placeholder(Kind k) {
    const Release& r = release(k);
    if (!r.sha256) return true;
    for (const char* c = r.sha256; *c; ++c) if (*c != '0') return false;
    return true;   // all zeros: nothing real is pinned
}

std::string managed_root() { return join(hub::data_dir(), "emulators"); }
std::string managed_dir(Kind k, const std::string& root) { return join(root, k == Kind::Azahar ? "azahar" : k == Kind::Cemu ? "cemu" : k == Kind::Pcsx2 ? "pcsx2" : k == Kind::Lrps2 ? "lrps2" : k == Kind::Ppsspp ? "ppsspp" : k == Kind::Lichii ? "lichii" : k == Kind::Fritchii ? "fritchii" : "dolphin"); }

/* ------------------------------------------------------------------ */
/* locating                                                            */
/* ------------------------------------------------------------------ */

static std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return s;
}

/* Folders named like "Cemu", "Cemu_2.6" below `base` (two levels) that hold Cemu.exe. */
static std::string cemu_in(const std::string& base, int depth) {
    std::error_code ec;
    if (base.empty() || !fs::is_directory(P(base), ec)) return "";
    if (file_exists(join(base, "Cemu.exe"))) return join(base, "Cemu.exe");
    if (depth <= 0) return "";
    for (fs::directory_iterator it(P(base), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        std::string n = lower(S(it->path().filename()));
        if (depth == 2 && n.find("cemu") == std::string::npos && n != "games" && n != "emulators" && n != "emulator" && n != "everything") continue;
        std::string r = cemu_in(S(it->path()), depth - 1);
        if (!r.empty()) return r;
    }
    return "";
}

/* ---- upgrade support ---- */

/* Staging / journal names inside an install folder. They are RetroHub's own scratch space, never part of an install. */
static const char* const kStageDir = "_extract";   // the unpacked download
static const char* const kOldDir = "_old";         // the previous build's program files while an upgrade swaps
static const char* const kJournal = "retrohub-upgrade.txt";
static const char* const kJournalTmp = "retrohub-upgrade.txt.tmp";

/* Top-level names inside an install folder that hold the PLAYER'S data and that RetroHub's own files live in. An install/upgrade never deletes, replaces or
 * overwrites them, even when the archive ships a folder of the same name. Non-empty = this kind can be upgraded in place. Lichii: everything it keeps
 * (keys, firmware, saves, config, shader caches, RetroHub's retrohub-defaults.txt) is under user\ (portable mode, lichii.h); RetroHub writes nothing else
 * beside the exe except the install marker. */
static std::vector<std::string> preserved_names(Kind k) {
    if (is_nx(k)) return {"user"};   // Lichii and Fritchii: each keeps its own user folder
    return {};
}
static bool upgradable(Kind k) { return !preserved_names(k).empty(); }
static bool name_in(const std::vector<std::string>& v, const std::string& n) {
    for (const auto& x : v) if (_stricmp(x.c_str(), n.c_str()) == 0) return true;
    return false;
}
/* sha256 line of retrohub-install.txt ("" when absent) */
static std::string installed_sha(const std::string& dir) {
    std::istringstream ls(read_text(join(dir, "retrohub-install.txt")));
    std::string line;
    while (std::getline(ls, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.compare(0, 7, "sha256 ") == 0) return line.substr(7);
    }
    return "";
}

/* Fritchii's own in-app updater parks an unfinished update as `update-staging`, `update-old` and update journal / lock files beside the exe ("update-*", ".update-*").
 * Starting the game with one of them there shows an "Interrupted Update" box, so such an install counts as outdated (RetroHub's reinstall puts the pinned build in) and Play
 * refuses to start it. RetroHub's own install and upgrade never create these. Lichii is never checked (its path stays as it was). */
std::string update_leftovers(Kind k, const std::string& dir) {
    if (k != Kind::Fritchii || dir.empty()) return "";
    std::error_code ec;
    for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = lower(S(it->path().filename()));
        if (n.compare(0, 7, "update-") == 0 || n.compare(0, 8, ".update-") == 0) return S(it->path().filename());
    }
    return "";
}

Install find(Kind k, const std::string& configured, const std::string& game_path, const std::string& root) {
    Install in;
    if (k == Kind::Cemu) {
        if (!configured.empty() && file_exists(configured)) {
            in.exe = configured;
            in.how = "chosen in Settings";
        }
        bool only_managed = !env(L"RETROHUB_NO_EXISTING_EMU").empty();   // test/diagnostic: ignore any Cemu outside RetroHub's data dir
        if (only_managed) in.exe.clear();
        if (in.exe.empty() && !game_path.empty() && !only_managed) {   // an install next to / above the game file (Cemu's own "game" folder)
            fs::path d = P(game_path).parent_path();
            for (int i = 0; i < 4 && !d.empty(); ++i, d = d.parent_path()) {
                if (file_exists(S(d / L"Cemu.exe"))) { in.exe = S(d / L"Cemu.exe"); in.how = "existing install near the game"; break; }
                if (d == d.root_path()) break;
            }
        }
        if (in.exe.empty() && !only_managed) {
            std::string up = env(L"USERPROFILE");
            const std::string roots[] = {join(up, "Desktop"), join(up, "Documents"), join(up, "Downloads"), "C:\\Cemu", "C:\\Games",
                                         "C:\\Emulators", env(L"ProgramFiles")};
            for (const auto& b : roots) {
                std::string r = cemu_in(b, 2);
                if (!r.empty()) { in.exe = r; in.how = "existing install (found automatically)"; break; }
            }
        }
        if (!in.exe.empty()) {
            in.dir = S(P(in.exe).parent_path());
            in.managed = false;
            return in;
        }
    }
    std::string md = managed_dir(k, root);
    std::string exe = join(md, U(release(k).exe));
    /* complete installs only; an upgrade journal means a swap is running or was cut short (old files may still be parked in _old): not playable until
     * the install job or recover_interrupted() has finished or rolled it back, so Play goes through the install path instead of starting a half build */
    if (file_exists(exe) && file_exists(join(md, "retrohub-install.txt")) && !(upgradable(k) && file_exists(join(md, kJournal)))) {
        in.exe = exe;
        in.dir = md;
        in.managed = true;
        in.how = "RetroHub download";
        in.outdated = upgradable(k) && (installed_sha(md) != release(k).sha256 || !update_leftovers(k, md).empty());
        if (in.outdated) in.how += " (update available)";
    }
    return in;
}

/* ------------------------------------------------------------------ */
/* download + install                                                  */
/* ------------------------------------------------------------------ */

/* Hashes a file with a BCrypt algorithm (SHA-256 or MD5). Returns lowercase hex, "" on error. */
static std::string bcrypt_file(const std::string& path, LPCWSTR alg_id, int digest_len) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, alg_id, nullptr, 0) != 0) return "";
    std::string out;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(1 << 20);
        while (f) {
            f.read(buf.data(), (std::streamsize)buf.size());
            std::streamsize n = f.gcount();
            if (n > 0) BCryptHashData(h, (PUCHAR)buf.data(), (ULONG)n, 0);
        }
        std::vector<UCHAR> dig(digest_len);
        if (BCryptFinishHash(h, dig.data(), (ULONG)digest_len, 0) == 0) {
            std::string hex;
            char tmp[3];
            for (int i = 0; i < digest_len; ++i) { std::snprintf(tmp, sizeof tmp, "%02x", dig[i]); hex += tmp; }
            out = hex;
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

std::string sha256_file(const std::string& path) { return bcrypt_file(path, BCRYPT_SHA256_ALGORITHM, 32); }
std::string md5_file(const std::string& path) { return bcrypt_file(path, BCRYPT_MD5_ALGORITHM, 16); }

struct Progress {
    std::atomic<uint64_t>* got;
    std::atomic<uint64_t>* total;
    const std::atomic<bool>* cancel = nullptr;   // emu_http.h callers only: stop (download fails with "cancelled")
    const std::atomic<bool>* pause = nullptr;    // emu_http.h callers only: wait while true
};

/* ---- install log + plain-English messages ----
 * The player sees a short sentence; the Windows error code goes to <managed root>\install-log.txt (and the debugger), never into the sentence. */
/* (heap-allocated and never freed: a straggler thread at process exit must not touch a destroyed object) */
static std::mutex& g_log_mu = *new std::mutex;
static std::string& g_log_path = *new std::string;
static void set_log_path(const std::string& p) { std::lock_guard<std::mutex> lk(g_log_mu); g_log_path = p; }
static void emu_log(const std::string& line) {
    OutputDebugStringA(("RetroHub emulator install: " + line + "\n").c_str());
    std::lock_guard<std::mutex> lk(g_log_mu);
    if (g_log_path.empty()) return;
    std::error_code ec;
    if (fs::exists(P(g_log_path), ec) && fs::file_size(P(g_log_path), ec) > 256 * 1024) fs::remove(P(g_log_path), ec);   // never grows without bound
    std::ofstream f(P(g_log_path), std::ios::binary | std::ios::app);
    if (!f) return;
    char stamp[32] = "";
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    if (localtime_s(&tmv, &t) == 0) std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S ", &tmv);
    f << stamp << line << "\n";
}

/* What a failed disk operation means to a player (empty = nothing specific to say). */
static std::string disk_problem_plain(DWORD e) {
    switch (e) {
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return "the disk is full. Free some space on that drive, then try again";
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: return "another program (often antivirus or a file indexer) is using the file. Wait a moment, then try again";
    case ERROR_ACCESS_DENIED: return "Windows would not let RetroHub write there. Check that antivirus or a folder protection setting is not blocking it, then try again";
    default: return "";
    }
}
static std::string disk_fail_text(DWORD e, const char* doing) {
    emu_log(std::string(doing) + " failed with Windows error " + std::to_string((unsigned long)e));
    const std::string plain = disk_problem_plain(e);
    return plain.empty() ? std::string("could not save the download (the disk reported a problem)") : "could not save the download: " + plain;
}
static std::string reason_text(const std::error_code& ec) {
    std::string m = ec.message();
    while (!m.empty() && (m.back() == '\r' || m.back() == '\n' || m.back() == ' ' || m.back() == '.')) m.pop_back();
    for (char& c : m) if ((unsigned char)c < 32) c = ' ';
    return m;
}

/* Only one install job (or leftover pass) touches the managed folders at a time. */
static std::mutex& g_job_mu = *new std::mutex;

/* ---- download: one GET at a time over a small transport, so the resume logic runs the same against WinHTTP and against the test hook ---- */
struct NetReply {
    int status = 0;
    uint64_t content_length = 0;
    bool has_range = false;                  // a Content-Range header came with a 206
    uint64_t range_start = 0, range_total = 0;
};
struct Transport {
    virtual ~Transport() {}
    virtual bool request(uint64_t offset, NetReply* r, std::string* err) = 0;   // GET (Range: bytes=offset- when offset > 0); false = server not reached
    virtual bool read(void* buf, DWORD cap, DWORD* n) = 0;                       // false = the connection broke
};

struct WinHttpTransport : Transport {
    std::wstring host, path;
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    std::string open_err;
    WinHttpTransport(const std::wstring& h, const std::wstring& p) : host(h), path(p) {
        ses = WinHttpOpen(L"RetroHub", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
        if (!ses) { open_err = "network unavailable"; return; }
        WinHttpSetTimeouts(ses, 15000, 20000, 30000, 60000);   // resolve, connect, send, receive (ms): a stalled network ends in a clear message, not a hang
        con = WinHttpConnect(ses, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
    ~WinHttpTransport() {
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    }
    bool request(uint64_t offset, NetReply* r, std::string* err) override {
        if (req) { WinHttpCloseHandle(req); req = nullptr; }
        if (!ses) { *err = open_err; return false; }
        if (!con) { *err = "could not reach " + U(host); return false; }
        req = WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        const std::wstring hdr = offset ? L"Range: bytes=" + std::to_wstring(offset) + L"-" : std::wstring();
        if (!req || !WinHttpSendRequest(req, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), hdr.empty() ? 0 : (DWORD)-1L, nullptr, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, nullptr)) {
            const DWORD e = GetLastError();
            emu_log("request to " + U(host) + " failed with Windows/WinHTTP error " + std::to_string((unsigned long)e));
            *err = e == ERROR_WINHTTP_TIMEOUT ? "the connection to " + U(host) + " timed out" : "could not reach " + U(host);
            return false;
        }
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
        r->status = (int)status;
        wchar_t lenb[64] = {0};
        sz = sizeof lenb;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, lenb, &sz, nullptr)) r->content_length = (uint64_t)_wtoi64(lenb);
        wchar_t cr[128] = {0};
        sz = sizeof cr;
        if (status == 206 && WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"Content-Range", cr, &sz, WINHTTP_NO_HEADER_INDEX)) {
            unsigned long long a = 0, b = 0, t = 0;
            const int got = swscanf_s(cr, L"bytes %llu-%llu/%llu", &a, &b, &t);
            if (got >= 2) { r->has_range = true; r->range_start = a; r->range_total = got == 3 ? t : 0; }
        }
        return true;
    }
    bool read(void* buf, DWORD cap, DWORD* n) override {
        if (WinHttpReadData(req, buf, cap, n)) return true;
        emu_log("download read failed with Windows/WinHTTP error " + std::to_string((unsigned long)GetLastError()));
        return false;
    }
};

struct TestTransport : Transport {
    const Release& rel;
    std::string body;
    size_t pos = 0;
    bool drop = false;
    explicit TestTransport(const Release& r) : rel(r) {}
    bool request(uint64_t offset, NetReply* r, std::string* err) override {
        InstallTestHooks::HttpReply tr;
        if (!g_test_hooks->serve(rel, offset, &tr)) { *err = "could not reach the test server"; return false; }
        body = std::move(tr.body);
        pos = 0;
        drop = tr.drop_after_body;
        r->status = tr.status;
        r->content_length = body.size();
        r->has_range = tr.status == 206;
        r->range_start = tr.range_start;
        r->range_total = tr.range_total;
        return true;
    }
    bool read(void* buf, DWORD cap, DWORD* n) override {
        if (pos < body.size()) {
            *n = (DWORD)std::min<size_t>(cap, body.size() - pos);
            std::memcpy(buf, body.data() + pos, *n);
            pos += *n;
            return true;
        }
        *n = 0;
        return !drop;
    }
};

/* Writes the download to out_file. With `expected` (the pinned size) a partial file already there is RESUMED with a Range request when the server honours it
 * (206 with the right Content-Range); a server that ignores Range (200), answers 416 or sends the wrong range makes the download restart cleanly from zero. A
 * dropped connection is resumed up to three times. The partial file is kept when the download ends early, so the next try continues it; the caller's size +
 * SHA-256 check still decides whether the result is good (and deletes a file that fails it). expected == 0 (other callers): always a fresh file, no resume. */
static bool download_core(Transport& tp, const std::string& out_file, uint64_t expected, Progress pr, bool pace, std::string* err) {
    static const int kMaxResumes = 3;
    auto cancelled = [&] { return pr.cancel && pr.cancel->load(); };
    auto file_len = [&]() -> uint64_t {
        std::error_code ec;
        if (!fs::is_regular_file(P(out_file), ec)) return 0;
        const uint64_t n = (uint64_t)fs::file_size(P(out_file), ec);
        return ec ? 0 : n;
    };
    auto wipe = [&] { std::error_code ec; fs::remove(P(out_file), ec); };
    uint64_t have = 0;
    if (expected) {
        have = file_len();
        if (have > expected) { wipe(); have = 0; }   // not a prefix of the pinned file
    }
    int resumes = 0, restarts = 0;
    for (;;) {
        if (cancelled()) { *err = "cancelled"; return false; }
        if (expected && have == expected) {   // the whole file is already here: verification decides
            if (pr.got) *pr.got = have;
            if (pr.total) *pr.total = expected;
            return true;
        }
        NetReply r;
        if (!tp.request(have, &r, err)) {
            if ((have > 0 || resumes > 0) && resumes < kMaxResumes) { ++resumes; if (pace) Sleep(1000u * resumes); continue; }   // a network blip while resuming
            return false;
        }
        bool append = false;
        if (have > 0) {
            if (r.status == 206) {
                if (r.has_range && r.range_start == have && (!expected || r.range_total == 0 || r.range_total == expected)) append = true;
                else { emu_log("the server sent a range that does not match what was asked: starting over"); wipe(); have = 0; if (++restarts > 2) { *err = "the server's answer to a resume request made no sense"; return false; } continue; }
            } else if (r.status == 416) {
                emu_log("the server refused the range request (416): starting over");
                wipe(); have = 0;
                if (++restarts > 2) { *err = "the server refused to resume the download"; return false; }
                continue;
            } else if (r.status == 200) {
                emu_log("the server ignored the range request: starting over");   // it sends the whole file again: the file is rewritten from the start below
                have = 0;
            } else { *err = "server answered " + std::to_string(r.status); return false; }
        } else if (r.status != 200) { *err = "server answered " + std::to_string(r.status); return false; }

        if (pr.total) *pr.total = expected ? expected : r.content_length;
        HANDLE f = append ? CreateFileW(W(out_file).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)
                          : CreateFileW(W(out_file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) { *err = disk_fail_text(GetLastError(), "opening the download file"); return false; }
        if (append) {
            LARGE_INTEGER z{};
            if (!SetFilePointerEx(f, z, nullptr, FILE_END)) { const DWORD e = GetLastError(); CloseHandle(f); *err = disk_fail_text(e, "seeking in the download file"); return false; }
        }
        uint64_t pos = append ? have : 0;
        if (pr.got) *pr.got = pos;
        std::vector<char> buf(64 * 1024);
        bool dropped = false, stopped = false;
        for (;;) {
            while (pr.pause && pr.pause->load() && !cancelled()) Sleep(250);
            if (cancelled()) { stopped = true; break; }
            DWORD n = 0;
            if (!tp.read(buf.data(), (DWORD)buf.size(), &n)) { dropped = true; break; }
            if (n == 0) break;
            DWORD e = 0, w = 0;
            if (g_test_hooks && g_test_hooks->write_fault) e = (DWORD)g_test_hooks->write_fault(pos);
            if (!e) {
                if (!WriteFile(f, buf.data(), n, &w, nullptr)) e = GetLastError();
                else if (w != n) e = ERROR_DISK_FULL;
            }
            if (e) { CloseHandle(f); *err = disk_fail_text(e, "writing the download"); return false; }
            pos += n;
            if (pr.got) *pr.got = pos;
        }
        CloseHandle(f);
        if (stopped) { *err = "cancelled"; return false; }
        have = pos;
        if (!dropped && (!expected || have >= expected)) return true;
        if (!expected || resumes >= kMaxResumes) { *err = "download interrupted: the connection dropped. Try again and it will continue where it stopped"; return false; }
        ++resumes;
        emu_log("connection dropped after " + std::to_string((unsigned long long)have) + " bytes: resuming");
        if (pace) Sleep(1000u * resumes);
    }
}

static bool http_download(const std::wstring& host, const std::wstring& path, const std::string& out_file, uint64_t expected, Progress pr, std::string* err) {
    WinHttpTransport tp(host, path);
    return download_core(tp, out_file, expected, pr, true, err);
}

bool http_get_file(const std::string& host, const std::string& path, const std::string& out_file, std::atomic<uint64_t>* got,
                   std::atomic<uint64_t>* total, const std::atomic<bool>* cancel, const std::atomic<bool>* pause, std::string* err) {
    return http_download(W(host), W(path), out_file, 0, Progress{got, total, cancel, pause}, err);   // (no pinned size: always a fresh file)
}

/* Directory (searched to `depth` levels) that contains a file called `name`. */
static bool find_file_below(const std::string& dir, const std::wstring& name, int depth, std::string* found_dir) {
    std::error_code ec;
    std::vector<fs::path> subs;
    for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) subs.push_back(it->path());
        else if (_wcsicmp(it->path().filename().c_str(), name.c_str()) == 0) { *found_dir = dir; return true; }
    }
    if (depth <= 0) return false;
    for (auto& s : subs)
        if (find_file_below(S(s), name, depth - 1, found_dir)) return true;
    return false;
}

/* Entry names from the zip: UTF-8, relative, no "..", no drive or root. */
static bool safe_zip_name(const std::string& n) {
    if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find(':') != std::string::npos) return false;
    size_t p = 0;
    while (p <= n.size()) {
        size_t q = n.find_first_of("/\\", p);
        std::string part = n.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (part == "..") return false;
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return true;
}

/* Own unzip (miniz) instead of bsdtar: every entry name is checked, and any entry that cannot be written fails the install
 * (bsdtar skipped Cemu's resources/ar/<RTL mark>cemu.mo and exited 1; the wide-char API writes that name fine). */
static bool unzip_to(const std::string& archive, const std::string& dest, std::string* err) {
    mz_zip_archive za;
    std::memset(&za, 0, sizeof za);
    FILE* zf = _wfopen(W(archive).c_str(), L"rb");
    if (!zf) { *err = "cannot open the download"; return false; }
    if (!mz_zip_reader_init_cfile(&za, zf, 0, 0)) { std::fclose(zf); *err = "the download is not a valid zip"; return false; }
    bool ok = true;
    mz_uint n = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < n && ok; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st)) { ok = false; *err = "damaged zip entry"; break; }
        std::string name = st.m_filename;
        if (!safe_zip_name(name)) { ok = false; *err = "unsafe path in the zip: " + name; break; }
        fs::path target = P(dest) / fs::u8path(name);
        std::error_code ec;
        if (st.m_is_directory || name.back() == '/' || name.back() == '\\') { fs::create_directories(target, ec); continue; }
        fs::create_directories(target.parent_path(), ec);
        FILE* out = _wfopen(target.wstring().c_str(), L"wb");
        if (!out) { ok = false; *err = "cannot write " + name; break; }
        bool r = mz_zip_reader_extract_to_cfile(&za, i, out, 0) != 0;
        bool flushed = std::fflush(out) == 0;
        std::fclose(out);
        if (!r || !flushed) { ok = false; *err = "cannot extract " + name; }
    }
    mz_zip_reader_end(&za);
    std::fclose(zf);
    return ok;
}

/* Dolphin ships as a .7z only. Windows' own tar.exe (bsdtar) reads it; the entry list is checked first (same path rules as
 * the zip reader) so nothing can land outside the staging folder, and any tar failure fails the install. tar runs by its absolute System32 path (never the search
 * path), with no window, and its error output goes to a temp file so the failure message can say what tar complained about. */
static std::string last_line_of(const std::string& text) {
    std::string line, best;
    std::istringstream ls(text);
    while (std::getline(ls, line)) {
        std::string clean;
        for (unsigned char c : line) if (c >= 32 && c < 127) clean += (char)c;
        while (!clean.empty() && clean.back() == ' ') clean.pop_back();
        size_t b = clean.find_first_not_of(' ');
        if (b != std::string::npos) best = clean.substr(b);
    }
    if (best.size() > 100) best = best.substr(0, 100);
    return best;
}

static bool run_tar(const std::wstring& args, const std::string& stdout_file, const std::string& stderr_file, std::string* err) {
    wchar_t sysdir[MAX_PATH];
    UINT sl = GetSystemDirectoryW(sysdir, MAX_PATH);
    std::wstring tar = (sl > 0 && sl < MAX_PATH ? std::wstring(sysdir) : std::wstring(L"C:\\Windows\\System32")) + L"\\tar.exe";
    std::wstring cmd = L"\"" + tar + L"\" " + args;
    std::vector<wchar_t> cb(cmd.begin(), cmd.end());
    cb.push_back(0);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = INVALID_HANDLE_VALUE, errh = INVALID_HANDLE_VALUE, in = INVALID_HANDLE_VALUE;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    out = CreateFileW(stdout_file.empty() ? L"NUL" : W(stdout_file).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, stdout_file.empty() ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    errh = CreateFileW(stderr_file.empty() ? L"NUL" : W(stderr_file).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, stderr_file.empty() ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = errh;
    PROCESS_INFORMATION pi{};
    BOOL ok = (in != INVALID_HANDLE_VALUE && out != INVALID_HANDLE_VALUE && errh != INVALID_HANDLE_VALUE)
                  ? CreateProcessW(tar.c_str(), cb.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) : FALSE;
    for (HANDLE h : {in, out, errh}) if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) { *err = "tar.exe not found (needs Windows 10 or later)"; return false; }
    DWORD w = WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
    DWORD code = 1;
    if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code); else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0) {
        const std::string why = stderr_file.empty() ? std::string() : last_line_of(read_text(stderr_file));
        emu_log("tar exit " + std::to_string((unsigned long)code) + (why.empty() ? "" : ": " + why));
        *err = "unpacking failed (tar exit " + std::to_string(code) + (why.empty() ? "" : ": " + why) + ")";
        return false;
    }
    return true;
}

static bool untar_to(const std::string& archive, const std::string& dest, const std::string& list_tmp, std::string* err) {
    const std::string err_tmp = list_tmp + ".err";
    struct Cleanup { std::string f, g; ~Cleanup() { std::error_code ec; fs::remove(P(f), ec); fs::remove(P(g), ec); } } cleanup{list_tmp, err_tmp};
    /* names (-tf): every path checked. types (-tvf): the first column is the entry type; only plain files and folders pass,
     * so no symlink or hardlink can point out of the staging folder. */
    if (!run_tar(L"-tf \"" + W(archive) + L"\"", list_tmp, err_tmp, err)) return false;
    std::istringstream ls(read_text(list_tmp));
    std::string line;
    int n = 0;
    while (std::getline(ls, line)) {
        while (!line.empty() && (line.back() == 13 || line.back() == 10)) line.pop_back();
        if (line.empty()) continue;
        if (!safe_zip_name(line)) { *err = "unsafe path in the download: " + line; return false; }
        ++n;
    }
    if (n == 0) { *err = "the download is empty"; return false; }
    if (!run_tar(L"-tvf \"" + W(archive) + L"\"", list_tmp, err_tmp, err)) return false;
    std::istringstream vs(read_text(list_tmp));
    int m = 0;
    while (std::getline(vs, line)) {
        if (line.empty() || line[0] == 13) continue;
        if ((line[0] != '-' && line[0] != 'd') || line.find(" link to ") != std::string::npos) { *err = "the download contains a link entry: " + line.substr(0, 80); return false; }
        ++m;
    }
    if (m != n) { *err = "the download listing is inconsistent"; return false; }
    return run_tar(L"-xf \"" + W(archive) + L"\" -C \"" + W(dest) + L"\"", "", err_tmp, err);
}

static bool running_from(const std::string& exe);   // (below)
static bool exe_is_running(const std::string& exe) {
    if (g_test_hooks && g_test_hooks->running) return g_test_hooks->running(exe);
    return running_from(exe);
}


static bool is_scratch(const std::string& n) {
    return _stricmp(n.c_str(), kStageDir) == 0 || _stricmp(n.c_str(), kOldDir) == 0 || _stricmp(n.c_str(), kJournal) == 0 || _stricmp(n.c_str(), kJournalTmp) == 0;
}

/* The journal is never seen half-written (a power cut while writing it): the text goes to a temp file first, which then replaces the journal in one step. */
static bool write_journal(const std::string& dir, const std::string& text) {
    const std::string tmp = join(dir, kJournalTmp);
    {
        std::ofstream f(P(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), (std::streamsize)text.size());
        f.flush();
        if (!f) return false;
    }
    return MoveFileExW(P(tmp).c_str(), P(join(dir, kJournal)).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

static std::vector<fs::path> children(const std::string& dir) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec)) v.push_back(it->path());
    return v;
}

/* Empties `dir` of everything except the kept names (the player's data); no keep list = the whole folder goes (the old behaviour). */
static void clear_dir_keeping(const std::string& dir, const std::vector<std::string>& keep) {
    if (keep.empty()) { remove_tree(dir); return; }
    for (const fs::path& c : children(dir)) {
        const std::string n = S(c.filename());
        if (name_in(keep, n)) continue;
        remove_tree(S(c));
    }
}

/* Journal phases: 1 = parking the old files in _old (dir still holds old files that never moved: nothing is removed); 2 = the new build is coming in (every
 * old file is parked, so whatever is in dir that is not kept data or scratch is new and goes first); 3 = putting the old build back (the new files are
 * already gone, so everything in dir is old: nothing is removed, the rest of _old moves home). Phase 3 is written as soon as the new files are gone, so a
 * restore that a locked file stops half way is finished by the next run instead of mistaking the files it already moved back for new ones. */
static bool set_journal_phase(const std::string& dir, int phase) {
    std::istringstream ls(read_text(join(dir, kJournal)));
    std::string line, text;
    while (std::getline(ls, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line.compare(0, 6, "phase ") == 0) continue;
        text += line + "\n";
    }
    return write_journal(dir, text + "phase " + std::to_string(phase) + "\n");
}

/* Puts the parked old build back: in phase 2 whatever new files made it into `dir` are removed first (never the kept data), then _old\* moves home.
 * Returns false when something could not be removed or moved back; the journal then stays (at the phase that is still true) so the next run retries. */
static bool restore_old(const std::string& dir, const std::vector<std::string>& keep, bool drop_new) {
    const std::string old = join(dir, kOldDir);
    if (!dir_exists(old)) return true;
    if (drop_new) {   // (phase 2, see above)
        bool left = false;
        for (const fs::path& c : children(dir)) {
            const std::string n = S(c.filename());
            if (name_in(keep, n) || is_scratch(n)) continue;
            remove_tree(S(c));
            std::error_code ec;
            if (fs::exists(fs::symlink_status(c, ec))) left = true;
        }
        if (left) return false;   // a new file is still in use: the journal still says phase 2, so the next run removes it and restores
        if (!set_journal_phase(dir, 3)) return false;   // nothing moved back yet, so phase 2 is still true: the next run simply drops nothing and restores
    }
    bool ok = true;
    for (const fs::path& c : children(old)) {
        const std::string n = S(c.filename());
        const fs::path to = P(dir) / c.filename();
        std::error_code ec;
        /* A name is in exactly one of dir and _old while parking (phase 1), so a clash means the new build already put its own copy there (phase 2):
         * that copy goes and the old one comes back. Never the kept data or the scratch. */
        if (!name_in(keep, n) && !is_scratch(n) && fs::exists(fs::symlink_status(to, ec))) remove_tree(S(to));
        fs::rename(c, to, ec);
        if (ec) ok = false;
    }
    if (!ok) return false;
    remove_tree(old);
    remove_tree(join(dir, kStageDir));
    std::error_code ec;
    fs::remove(P(join(dir, kJournal)), ec);
    fs::remove(P(join(dir, kJournalTmp)), ec);
    return true;
}

/* A crash can cut an upgrade anywhere between "journal written" and "journal removed". The journal is written before anything moves and removed last, and
 * the marker is the last thing the swap writes, so: a valid marker for the journal's sha256 = the swap finished (only scratch left to delete); otherwise the
 * old build is parked in _old and goes back. Without a journal, _old and _extract are junk from before any file moved. The data folder is never touched. */
void recover_interrupted(Kind k, const std::string& root) {
    const std::string dir = managed_dir(k, root);
    if (!upgradable(k) || !dir_exists(dir)) return;
    const std::vector<std::string> keep = preserved_names(k);
    const std::string journal = join(dir, kJournal);
    if (file_exists(journal)) {
        std::string want;
        bool drop_new = false;   // (no phase line: assume parking, which removes nothing; restore_old still replaces any new file that clashes with a parked one)
        std::istringstream ls(read_text(journal));
        std::string line;
        while (std::getline(ls, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.compare(0, 7, "sha256 ") == 0) want = line.substr(7);
            if (line == "phase 2") drop_new = true;
            else if (line == "phase 1" || line == "phase 3") drop_new = false;
        }
        if (!want.empty() && installed_sha(dir) == want && file_exists(join(dir, U(release(k).exe)))) {   // finished: only the scratch is left
            remove_tree(join(dir, kOldDir));
            remove_tree(join(dir, kStageDir));
            std::error_code ec;
            fs::remove(P(journal), ec);
            return;
        }
        if (dir_exists(join(dir, kOldDir))) { restore_old(dir, keep, drop_new); return; }
        std::error_code ec;   // journal but no _old: nothing had moved yet
        fs::remove(P(journal), ec);
    }
    remove_tree(join(dir, kStageDir));
    remove_tree(join(dir, kOldDir));
    std::error_code ec;
    fs::remove(P(join(dir, kJournalTmp)), ec);
}

/* A rename that an antivirus scanner or a file indexer is holding up usually works a moment later: three tries, about 300 ms apart (like copy_retry in
 * update_apply.cpp). `why` gets the operating system's reason when it still fails. */
static bool rename_retry(const fs::path& from, const fs::path& to, std::string* why) {
    std::error_code ec;
    for (int t = 1;; ++t) {
        fs::rename(from, to, ec);
        if (!ec) return true;
        if (t >= 3 || ec == std::errc::no_such_file_or_directory) break;
        Sleep(300);
    }
    emu_log("could not rename " + S(from) + " (" + std::to_string(ec.value()) + "): " + ec.message());
    if (why) *why = reason_text(ec);
    return false;
}

/* The swap of an upgrade: `inner` (the unpacked new build, a folder under dir\_extract) replaces the program files in `dir`. See recover_interrupted for the
 * crash story. On any failure the old build is moved back and false is returned. */
static bool swap_in_new_build(Kind k, const std::string& dir, const std::string& inner, const Release& rel, std::string* err) {
    const std::vector<std::string> keep = preserved_names(k);
    const std::string old = join(dir, kOldDir);
    remove_tree(old);
    make_dirs(old);
    const std::string jtext = std::string("to ") + rel.version + "\nsha256 " + rel.sha256 + "\n";
    if (!write_journal(dir, jtext + "phase 1\n")) { remove_tree(old); *err = "cannot write the upgrade journal"; return false; }
    bool phase2 = false;
    auto fail = [&](const std::string& why) {
        *err = why;
        if (!restore_old(dir, keep, phase2)) *err += " (and the old files could not all be put back; RetroHub will retry at the next start)";
        return false;
    };
    for (const fs::path& c : children(dir)) {   // 1. the old program files (and the old marker) go aside
        const std::string n = S(c.filename());
        if (name_in(keep, n) || is_scratch(n)) continue;
        std::string why;
        if (!rename_retry(c, P(old) / c.filename(), &why)) return fail("could not move " + n + " aside (is something using it?): " + why);
    }
    if (!write_journal(dir, jtext + "phase 2\n")) return fail("cannot write the upgrade journal");
    phase2 = true;
    for (const fs::path& c : children(inner)) {   // 2. the new ones come in; a `user` folder in the archive is ignored
        const std::string n = S(c.filename());
        if (name_in(keep, n) || is_scratch(n)) continue;
        std::string why;
        if (!rename_retry(c, P(dir) / c.filename(), &why)) return fail("could not install " + n + ": " + why);
    }
    if (!file_exists(join(dir, U(rel.exe)))) return fail(U(rel.exe) + " missing after the update");
    return true;
}

static bool install_core(Kind k, const std::string& root, std::string* err, std::atomic<int>* phase, Progress pr) {
    const Release& rel = release(k);
    std::lock_guard<std::mutex> job_lock(g_job_mu);   // one job at a time; the leftover pass stays out while this runs
    set_log_path(join(root, "install-log.txt"));
    std::string dir = managed_dir(k, root);
    std::string exe = join(dir, U(rel.exe));
    const std::vector<std::string> keep = preserved_names(k);
    recover_interrupted(k, root);   // an upgrade that a crash cut short comes back to a whole install first
    if (upgradable(k) && file_exists(join(dir, kJournal))) {   // ...but could not be undone yet (a file still in use): the parked old build in _old must survive for the next try
        *err = std::string("an earlier ") + name(k) + " update could not be finished yet. Close " + name(k) + " if it is open, then try again";
        return false;
    }
    if (release_is_placeholder(k)) { *err = std::string(name(k)) + " is not published yet (its download has not been set up)"; return false; }
    const bool complete = file_exists(exe) && file_exists(join(dir, "retrohub-install.txt"));   // (marker is written last)
    if (complete && (!upgradable(k) || (installed_sha(dir) == rel.sha256 && update_leftovers(k, dir).empty()))) return true;
    const bool upgrading = complete;   // an older pin: the old build stays installed and working until the new one is whole
    std::string stage = join(dir, kStageDir);
    std::string archive = join(join(root, "_downloads"), S(fs::path(rel.path).filename()));
    if (!upgrading) clear_dir_keeping(dir, keep);   // a leftover from a failed attempt (the player's data folder stays)
    make_dirs(dir);
    make_dirs(S(P(archive).parent_path()));
    remove_tree(stage);
    make_dirs(stage);
    bool ok = true;
    auto setp = [&](int p) { if (phase) *phase = p; };

    setp(0);
    /* A partial file is only resumed when it belongs to THIS pin: a sidecar beside it holds the pinned SHA-256 and size. A part from an older pin that happens to have
     * the same file name (or one with no sidecar) is wiped up front instead of being resumed into a hash failure. */
    const std::string pin_file = archive + ".part.pin";
    if (!(g_test_hooks && g_test_hooks->fetch)) {
        const std::string pin_text = std::string(rel.sha256) + " " + std::to_string((unsigned long long)rel.size) + "\n";
        std::error_code ec;
        if (read_text(pin_file) != pin_text) fs::remove(P(archive + ".part"), ec);
        write_text(pin_file, pin_text);
    }
    /* Room for the zip, its unpacked copy and some slack (about 2.5 x the pinned size + 100 MB), less what a resumable partial file already holds. Checked before
     * anything is downloaded so a nearly full drive says so at once instead of failing at the end. */
    {
        std::error_code ec;
        const uint64_t have_part = fs::is_regular_file(P(archive + ".part"), ec) ? (uint64_t)fs::file_size(P(archive + ".part"), ec) : 0;
        const uint64_t need = rel.size / 2 * 5 + (100ull << 20) - std::min<uint64_t>(have_part, rel.size);
        uint64_t avail = 0;
        bool known = false;
        const std::string dl_dir = S(P(archive).parent_path());
        if (g_test_hooks && g_test_hooks->free_space) known = g_test_hooks->free_space(dl_dir, &avail);
        else {
            ULARGE_INTEGER fb{};
            if (GetDiskFreeSpaceExW(W(dl_dir).c_str(), &fb, nullptr, nullptr)) { avail = fb.QuadPart; known = true; }
        }
        if (known && avail < need) {
            ok = false;
            *err = "not enough free disk space: " + std::string(name(k)) + " needs about " + std::to_string((need + (1u << 20) - 1) >> 20) + " MB free on the drive that holds RetroHub's data, and only " +
                   std::to_string(avail >> 20) + " MB is free. Free some space, then try again";
            emu_log(*err);
        }
    }
    bool download_cut_short = false;   // a real download that ended early keeps its partial file, so the next try resumes it
    if (!ok) { /* no room: nothing is downloaded */ }
    else if (g_test_hooks && g_test_hooks->fetch) ok = g_test_hooks->fetch(rel, archive + ".part", err);
    else if (g_test_hooks && g_test_hooks->serve) { TestTransport tt(rel); ok = download_core(tt, archive + ".part", rel.size, pr, false, err); download_cut_short = !ok; }
    else { ok = http_download(rel.host, rel.path, archive + ".part", rel.size, pr, err); download_cut_short = !ok; }
    if (ok) {
        std::error_code ec;
        uint64_t sz = fs::file_size(P(archive + ".part"), ec);
        if (ec || sz != rel.size) { ok = false; *err = "download has the wrong size (" + std::to_string(sz) + " bytes)"; }
    }
    if (ok) {
        setp(1);
        std::string h = sha256_file(archive + ".part");
        if (h != rel.sha256) { ok = false; *err = "checksum mismatch: the download was rejected"; }
    }
    if (ok) {
        std::error_code ec;
        fs::remove(P(archive), ec);
        std::string why;
        if (!rename_retry(P(archive + ".part"), P(archive), &why)) { ok = false; *err = "cannot move the download into place: " + why; }
    }
    if (ok) {
        setp(2);
        ok = (archive.size() > 3 && archive.compare(archive.size() - 3, 3, ".7z") == 0) ? untar_to(archive, stage, join(root, "_downloads\\list.txt"), err)
                                                                                         : unzip_to(archive, stage, err);
    }
    if (ok) {
        setp(3);
        std::string inner;
        if (!find_file_below(stage, rel.exe, 3, &inner)) { ok = false; *err = U(rel.exe) + " not found in the download"; }
        else if (upgrading) {
            if (exe_is_running(exe)) { ok = false; *err = std::string(name(k)) + " is running: close it, then update"; }   // never replace files under a running program
            else ok = swap_in_new_build(k, dir, inner, rel, err);
        } else {
            std::error_code ec;
            for (fs::directory_iterator it(P(inner), ec), end; !ec && it != end; it.increment(ec)) {
                if (name_in(keep, S(it->path().filename()))) continue;   // never let the archive replace the player's data folder
                std::error_code e2;
                fs::path to = P(dir) / it->path().filename();
                fs::remove_all(to, e2);
                std::string why;
                if (!rename_retry(it->path(), to, &why)) { ok = false; *err = "could not install " + S(it->path().filename()) + ": " + why; break; }
            }
            if (ok && !file_exists(exe)) { ok = false; *err = U(rel.exe) + " missing after install"; }
        }
    }
    const bool swapped = ok && upgrading;   // from here a failure must put the old build back
    if (ok && k == Kind::Pcsx2) ok = write_text(join(dir, "portable.ini"), "");   // portable mode: all data beside the exe, a user's own PCSX2 is never touched
    if (ok && k == Kind::Ppsspp) { std::error_code ec2; fs::remove(P(join(dir, "installed.txt")), ec2); make_dirs(join(dir, "memstick")); }   // no installed.txt = portable: PPSSPP keeps everything in <dir>\memstick
    if (ok && is_nx(k)) {   // (Lichii and Fritchii) a `user` folder beside lichii.exe = portable (a player's own %APPDATA%\eden is never touched); the bundled VC++ installer is not needed to play. The delete below is kept for older zips (~116 MB of .pdb files, vc_redist); current zips contain none of them, so it is a no-op. nxformat.dll (build 130+) is kept
        make_dirs(join(dir, "user"));
        std::error_code ec2;
        for (const char* f : {"lichii.pdb", "eden-cli.pdb", "vc_redist.x64.exe", "lichii-updater.exe"}) fs::remove(P(join(dir, f)), ec2);   // lichii-updater.exe (build 125+) is Lichii's own self-updater: RetroHub is the only updater
    }
    if (ok && k == Kind::Dolphin) ok = write_text(join(dir, "portable.txt"), "");   // Dolphin keeps its User folder beside the exe
    if (ok) ok = write_text(join(dir, "retrohub-install.txt"), std::string(name(k)) + " " + rel.version + "\nsha256 " + rel.sha256 + "\n");   // the marker is last
    if (!ok) {
        if (swapped) { if (!restore_old(dir, keep, true)) *err += " (the old files could not all be put back; RetroHub will retry at the next start)"; }
        else if (!upgrading) clear_dir_keeping(dir, keep);   // never leave a half install behind (the player's data folder stays)
    } else if (upgrading) {   // done: the scratch goes, the journal last
        remove_tree(join(dir, kOldDir));
        if (dir_exists(join(dir, kOldDir))) emu_log("the parked old build in _old could not be removed yet (in use?): the next start's leftover pass retries");
        remove_tree(stage);
        std::error_code ec3;
        fs::remove(P(join(dir, kJournal)), ec3);
    }
    remove_tree(stage);
    std::error_code ec;
    fs::remove(P(archive), ec);
    if (!download_cut_short) fs::remove(P(archive + ".part"), ec);   // a failed check deletes the file for good: it is never resumed
    else if (fs::is_regular_file(P(archive + ".part"), ec) && fs::file_size(P(archive + ".part"), ec) == 0) fs::remove(P(archive + ".part"), ec);
    if (!fs::exists(P(archive + ".part"), ec)) fs::remove(P(pin_file), ec);
    return ok;
}

/* ---- one conservative leftover pass (see emulators.h) ---- */
static bool is_reparse_point(const fs::path& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;   // (unreadable counts as "do not touch")
}
/* true when `d` and everything below it is plain files and folders: no reparse point anywhere and no folder or file called `user`. Never descends into a link. */
static bool plain_tree(const fs::path& d, int depth = 0) {
    if (depth > 40) return false;
    std::error_code ec;
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path c = it->path();
        if (is_reparse_point(c)) return false;
        if (_stricmp(S(c.filename()).c_str(), "user") == 0) return false;
        std::error_code e2;
        if (fs::is_directory(c, e2) && !plain_tree(c, depth + 1)) return false;
    }
    return !ec;
}

static std::string g_cleanup_test_root;   // console tests only (set_cleanup_test_root)
void set_cleanup_test_root(const std::string& root) { g_cleanup_test_root = root; }

/* The leftover pass deletes things, so it only accepts the hub's own emulators folder (<data dir>\emulators) or the test root: an absolute path, never a drive root,
 * a sane length, and the leaf name exactly `emulators` for the real one. */
static bool cleanup_root_ok(const std::string& root) {
    if (root.size() < 12) return false;
    const fs::path p = P(root).lexically_normal();
    if (!p.is_absolute() || p == p.root_path() || p.filename().empty()) return false;
    auto same = [](const fs::path& a, const fs::path& b) { return _wcsicmp(a.lexically_normal().wstring().c_str(), b.lexically_normal().wstring().c_str()) == 0; };
    if (!g_cleanup_test_root.empty() && same(p, P(g_cleanup_test_root))) return true;
    return _wcsicmp(p.filename().wstring().c_str(), L"emulators") == 0 && same(p, P(managed_root()));
}

static std::atomic<bool> g_cleanup_cancel{false};

static int cleanup_impl(const std::string& root, const std::atomic<bool>* cancel) {
    auto stop = [&] { return cancel && cancel->load(); };
    if (!cleanup_root_ok(root)) { emu_log("leftover pass refused the folder " + root); return 0; }
    std::unique_lock<std::mutex> job(g_job_mu, std::try_to_lock);
    if (!job.owns_lock()) return 0;   // an install job is running: it owns these folders
    set_log_path(join(root, "install-log.txt"));
    int removed = 0;
    std::error_code ec;
    const fs::path rootp = P(root);
    if (root.empty() || !fs::is_directory(rootp, ec) || is_reparse_point(rootp)) return 0;
    const auto now = fs::file_time_type::clock::now();
    // 1. downloads (and their listing / partial files) older than a day: plain files directly inside _downloads only
    const fs::path dl = rootp / "_downloads";
    if (fs::is_directory(dl, ec) && !is_reparse_point(dl)) {
        std::vector<fs::path> old_files;
        for (fs::directory_iterator it(dl, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2) || is_reparse_point(it->path())) continue;
            const auto t = fs::last_write_time(it->path(), e2);
            if (!e2 && now - t > std::chrono::hours(24)) old_files.push_back(it->path());
        }
        for (const fs::path& f : old_files) {
            if (stop()) return removed;
            std::error_code e2;
            if (fs::remove(f, e2) && !e2) { ++removed; emu_log("removed a stale download: " + S(f.filename())); }
        }
    }
    // 2. an `_old` / `_extract` folder with no upgrade journal beside it, in each emulator's own folder
    for (Kind k : {Kind::Azahar, Kind::Cemu, Kind::Dolphin, Kind::Pcsx2, Kind::Lrps2, Kind::Ppsspp, Kind::Lichii, Kind::Fritchii}) {
        if (stop()) return removed;
        const fs::path dir = P(managed_dir(k, root));
        if (!fs::is_directory(dir, ec) || is_reparse_point(dir)) continue;
        if (fs::exists(dir / kJournal, ec) || fs::exists(dir / kJournalTmp, ec)) continue;   // recover_interrupted owns those
        for (const char* n : {kOldDir, kStageDir}) {
            const fs::path s = dir / n;
            std::error_code e2;
            if (!fs::is_directory(s, e2) || is_reparse_point(s)) continue;
            const auto t = fs::last_write_time(s, e2);
            if (e2 || now - t < std::chrono::minutes(10)) continue;
            if (n == kOldDir && !(fs::is_regular_file(dir / release(k).exe, e2) && fs::is_regular_file(dir / "retrohub-install.txt", e2))) {
                emu_log("left alone (the live install beside it is incomplete, so this may be the only working copy): " + S(s));
                continue;
            }   // too fresh to be sure it is abandoned
            if (!plain_tree(s)) { emu_log(std::string("left alone (a link or a user folder inside): ") + S(s)); continue; }
            remove_tree(S(s));
            if (!fs::exists(s, e2)) { ++removed; emu_log("removed a leftover folder: " + S(s)); }
            else emu_log("could not remove a leftover folder (in use?): " + S(s));
        }
    }
    return removed;
}

int cleanup_leftovers(const std::string& root) { return cleanup_impl(root, nullptr); }

/* The background pass is a joinable thread that is cancelled and joined when the installer object goes away (~Installer) or when the next pass starts. */
static std::mutex& g_cleanup_mu = *new std::mutex;
static std::thread*& g_cleanup_th = *new std::thread*(nullptr);
void cleanup_leftovers_stop() {
    std::lock_guard<std::mutex> lk(g_cleanup_mu);
    g_cleanup_cancel = true;
    if (g_cleanup_th) { if (g_cleanup_th->joinable()) g_cleanup_th->join(); delete g_cleanup_th; g_cleanup_th = nullptr; }
}
void cleanup_leftovers_async(const std::string& root) {
    cleanup_leftovers_stop();
    std::lock_guard<std::mutex> lk(g_cleanup_mu);
    g_cleanup_cancel = false;
    g_cleanup_th = new std::thread([root] { cleanup_impl(root, &g_cleanup_cancel); });
}

bool install_blocking(Kind k, const std::string& root, std::string* err,
                      const std::function<void(int, uint64_t, uint64_t)>& progress) {
    std::atomic<int> phase{0};
    std::atomic<uint64_t> got{0}, total{0};
    std::atomic<bool> stop{false};
    bool ok = false;
    std::string e;
    std::thread t([&] { ok = install_core(k, root, &e, &phase, {&got, &total}); stop = true; });
    while (!stop) {
        if (progress) progress(phase, got, total);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    t.join();
    if (progress) progress(phase, got, total);
    if (err) *err = e;
    return ok;
}

Installer::~Installer() { cleanup_leftovers_stop(); if (th.joinable()) th.join(); }

void Installer::start(Kind k, const std::string& managed_root_dir) {
    if (th.joinable()) th.join();
    kind = k;
    root = managed_root_dir;
    updating = upgradable(k) && file_exists(join(managed_dir(k, root), U(release(k).exe))) && file_exists(join(managed_dir(k, root), "retrohub-install.txt"));
    done = false;
    ok = false;
    got = 0;
    total = 0;
    phase = 0;
    error.clear();
    state = Working;
    th = std::thread([this] {
        std::string e;
        bool r = install_core(kind, root, &e, &phase, {&got, &total});
        if (!r) { std::lock_guard<std::mutex> lk(mu); error = e; }
        ok = r;
        done = true;
    });
}

bool Installer::poll(bool* success) {
    if (state != Working || !done) return false;
    if (th.joinable()) th.join();
    if (success) *success = ok;
    state = ok ? Idle : Failed;
    return true;
}

/* ------------------------------------------------------------------ */
/* config pre-writing                                                  */
/* ------------------------------------------------------------------ */

/* Profile-driven 3DS pad: control -> qt-config key. The default profile maps by position (3DS A = east = the Xbox B
 * button). maptype "all" with api "controller" means any connected game controller (so the first XInput pad). */
static const char* const kAzButtons[] = {"a", "b", "x", "y", "up", "down", "left", "right", "l", "r", "start", "select", "zl", "zr"};

static std::string fmt_g(double v) { char b[40]; std::snprintf(b, sizeof b, "%g", v); return b; }

/* Which pad Azahar's SDL input binds: a guid + the n-th pad with that guid ("port"). Without a choice every binding keeps
 * "maptype:all,port:0" (any pad). */
static std::string az_target(const AzPad& pad) {
    if (pad.guid.empty()) return "maptype:all,port:0";
    return "guid:" + pad.guid + ",port:" + std::to_string(pad.port);
}

static std::string az_binding(const hubctl::Profile& p, const std::string& control, const AzPad& pad) {
    auto it = p.buttons.find(control);
    std::vector<hubctl::Term> t;
    if (it == p.buttons.end() || !hubctl::parse_expr(it->second, p.axis_threshold, &t) || t.empty()) return "";
    const hubctl::Term& m = t[0];   // Azahar takes one source per button
    if (!m.axis) return "engine:sdl,api:controller,button:" + std::to_string(m.idx) + "," + az_target(pad);
    return "engine:sdl,api:controller,axis:" + std::to_string(m.idx) + ",direction:" + (m.sign < 0 ? "-" : "+") +
           ",threshold:" + fmt_g(m.threshold / 32768.0) + "," + az_target(pad);
}

static std::string az_analog(const hubctl::Profile& p, const std::string& control, const AzPad& pad) {
    auto it = p.analogs.find(control);
    if (it == p.analogs.end()) return "";
    int ax = hubctl::axis_index(it->second.x), ay = hubctl::axis_index(it->second.y);
    if (ax < 0 || ay < 0) return "";
    return "engine:sdl,api:controller,axis_x:" + std::to_string(ax) + ",axis_y:" + std::to_string(ay) + ",deadzone:" +
           fmt_g(it->second.deadzone) + "," + az_target(pad);
}

std::string azahar_config_patch(const std::string& in, const hubctl::Profile& prof, const AzPad& pad) {
    std::string t = in;
    auto set = [&](const char* sec, const std::string& key, const std::string& val) {
        t = hubui::ini_set(t, sec, key + "\\default", "false");
        t = hubui::ini_set(t, sec, key, val);
    };
    set("UI", "confirmClose", "false");
    set("UI", "firstStart", "false");
    set("UI", "enable_discord_presence", "false");
    set("Miscellaneous", "check_for_update_on_start", "false");
    for (const char* c : kAzButtons) { std::string v = az_binding(prof, c, pad); if (!v.empty()) set("Controls", std::string("button_") + c, "\"" + v + "\""); }
    for (const char* c : {"circle_pad", "c_stick"}) { std::string v = az_analog(prof, c, pad); if (!v.empty()) set("Controls", c, "\"" + v + "\""); }
    return t;
}
/* The Qt UI language, English (Azahar config.cpp: UISettings::values.language lives in the [Paths] group), so the 3DS link automation finds its menus by name on any Windows language. Only RetroHub's own Azahar. */
std::string azahar_pin_language(const std::string& in) {
    std::string t = hubui::ini_set(in, "Paths", "language\\default", "false");
    return hubui::ini_set(t, "Paths", "language", "en");
}
std::string azahar_config_patch(const std::string& in, const hubctl::Profile& prof) { return azahar_config_patch(in, prof, AzPad()); }
std::string azahar_config_patch(const std::string& in) { return azahar_config_patch(in, hubctl::default_profile("3ds"), AzPad()); }

/* Player 1's pad = first in port order. Its "port" is its rank among the pads with the same guid in SDL's own order. */
bool azahar_pad_select(const std::vector<PadDesc>& by_port, const std::vector<PadDesc>& by_sdl, AzPad* out) {
    *out = AzPad();
    if (by_port.empty()) return false;
    const PadDesc& p = by_port[0];
    int rank = 0;
    for (const PadDesc& d : by_sdl) { if (d.key == p.key) break; if (d.guid == p.guid) ++rank; }
    out->guid = p.guid;
    out->port = rank;
    return true;
}

/* Marker suffix for the chosen pad: nothing when none (default profiles keep their old marker hash). */
static std::string pad_suffix(const std::string& sel) {
    if (sel.empty()) return "";
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned char c : sel) h = (h ^ c) * 0x100000001B3ull;
    return "-" + hubctl::hash_hex(h);
}

/* The generated pad configs are written when missing, and again only when the RetroHub profile they come from changed:
 * the marker file holds the hash of the profile that was written ("profile-hash: ..."). A marker without a hash is from
 * an older RetroHub that wrote the built-in default. Edits made in the emulator's own dialogs survive until then. */
static std::string marker_hash(const std::string& marker_text, const std::string& system) {
    size_t p = marker_text.find("profile-hash: ");
    if (p == std::string::npos) return hubctl::hash_hex(hubctl::mapping_hash(hubctl::default_profile(system)));
    size_t e = marker_text.find_first_of("\r\n", p);
    return marker_text.substr(p + 14, e == std::string::npos ? std::string::npos : e - p - 14);
}

static hubctl::Profile stored_profile(const std::string& system) {
    hubctl::Store st;
    hubctl::load_store(hub::data_dir(), "", &st);
    return hubctl::resolve(st, system);
}

void prepare_azahar(const std::string& install_dir, const hubctl::Profile& prof, const AzPad& pad) {
    std::string user = join(install_dir, "user");   // a "user" folder beside azahar.exe makes it portable
    std::string cfgdir = join(user, "config");
    make_dirs(cfgdir);
    make_dirs(join(user, "sdmc"));
    std::string marker = join(cfgdir, "retrohub-pad.txt");
    {   // every launch (the pad mapping below is written once per profile): the UI stays English
        const std::string ini0 = join(cfgdir, "qt-config.ini"), cur = read_text(ini0), pinned = azahar_pin_language(cur);
        if (pinned != cur) write_text(ini0, pinned);
    }
    std::string want = hubctl::hash_hex(hubctl::mapping_hash(prof)) + pad_suffix(pad.guid.empty() ? "" : pad.guid + "/" + std::to_string(pad.port));
    if (file_exists(marker) && marker_hash(read_text(marker), "3ds") == want) return;   // already written for this profile; later edits are kept
    std::string ini = join(cfgdir, "qt-config.ini");
    write_text(ini, azahar_config_patch(read_text(ini), prof, pad));
    write_text(marker, "RetroHub wrote the controller mapping from its Controllers profile.\n"
                       "It is rewritten only when that profile changes. Delete this file to have it written again.\n"
                       "profile-hash: " + want + "\n");
}
void prepare_azahar(const std::string& install_dir, const hubctl::Profile& prof) { prepare_azahar(install_dir, prof, AzPad()); }
void prepare_azahar(const std::string& install_dir) { prepare_azahar(install_dir, stored_profile("3ds"), AzPad()); }

std::string cemu_default_settings_xml() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<content>\n"
           "\t<logflag>0</logflag>\n"
           "\t<mlc_path/>\n"
           "\t<disable_screensaver>true</disable_screensaver>\n"
           "\t<check_update>false</check_update>\n"
           "\t<receive_untested_updates>false</receive_untested_updates>\n"
           "\t<use_discord_presence>false</use_discord_presence>\n"
           "\t<vk_warning>true</vk_warning>\n"
           "\t<gp_download>true</gp_download>\n"
           "\t<fullscreen>false</fullscreen>\n"
           "</content>\n";
}

/* SDL source -> Cemu's XInput mapping ids (from Cemu's own set_default_mapping, VPADController.cpp). */
static int cemu_xinput_id(const hubctl::Term& t) {
    static const std::map<std::string, int> m = {
        {"a", 12}, {"b", 13}, {"x", 14}, {"y", 15}, {"leftshoulder", 8}, {"rightshoulder", 9}, {"start", 4}, {"back", 5},
        {"dpup", 0}, {"dpdown", 1}, {"dpleft", 2}, {"dpright", 3}, {"leftstick", 6}, {"rightstick", 7},
        {"lefttrigger", 42}, {"righttrigger", 43}, {"leftx+", 38}, {"lefty-", 39}, {"rightx+", 40}, {"righty-", 41},
        {"leftx-", 44}, {"lefty+", 45}, {"rightx-", 46}, {"righty+", 47}};
    auto it = m.find(hubctl::term_name(t));
    return it == m.end() ? -1 : it->second;
}

std::vector<int> xinput_slots(const std::vector<PadDesc>& pads) {
    std::vector<int> slot(pads.size(), -1);
    bool used[4] = {false, false, false, false};
    for (size_t i = 0; i < pads.size(); ++i)   // first pass: pads that know their slot
        if (pads[i].xinput && pads[i].player >= 0 && pads[i].player < 4 && !used[pads[i].player]) { slot[i] = pads[i].player; used[pads[i].player] = true; }
    for (size_t i = 0; i < pads.size(); ++i) {   // second pass: the rest take the lowest free slot
        if (!pads[i].xinput || slot[i] >= 0) continue;
        for (int s = 0; s < 4; ++s) if (!used[s]) { slot[i] = s; used[s] = true; break; }
    }
    return slot;
}

std::vector<PadDesc> default_player_order(const std::vector<PadDesc>& sdl) {
    const std::vector<int> slot = xinput_slots(sdl);
    std::vector<size_t> xs;
    for (size_t i = 0; i < sdl.size(); ++i) if (slot[i] >= 0) xs.push_back(i);
    std::stable_sort(xs.begin(), xs.end(), [&](size_t a, size_t b) { return slot[a] < slot[b]; });
    std::vector<PadDesc> out;
    for (size_t i : xs) out.push_back(sdl[i]);
    for (size_t i = 0; i < sdl.size(); ++i) if (slot[i] < 0) out.push_back(sdl[i]);
    return out;
}

/* Wii U player N = the N-th XInput pad of the RetroHub port order, addressed by its real XInput slot (Cemu's XInput API numbers pads by
 * slot, so with the pads in another order than the slots, or a gap in the slots, "the N-th pad" is not slot N-1). Pads that are not XInput
 * are skipped here: use cemu_pads_with_sdl to get them as SDL players. */
CemuPads cemu_pads_with(const std::vector<PadDesc>& by_port) {
    CemuPads c;
    const std::vector<int> slot = xinput_slots(by_port);
    for (size_t i = 0; i < by_port.size(); ++i)
        if (slot[i] >= 0 && c.xinput.size() < 4) c.xinput.push_back(slot[i]);
    return c;
}

static std::string cemu_sel(const CemuPads& c) {
    std::string s;
    for (size_t i = 0; i < c.xinput.size(); ++i)
        s += std::to_string(i) + "=" + (i < c.dsu.size() && c.dsu[i] >= 0 ? "dsu" + std::to_string(c.dsu[i]) : i < c.sdl.size() && !c.sdl[i].empty() ? c.sdl[i] : std::to_string(c.xinput[i])) + ";";
    return s == "0=0;" ? "" : s;   // the old single-pad setup keeps its marker hash
}

/* Cemu's SDL pad ids (SDLController.cpp: buttons are SDL's own gamepad button numbers; axes/triggers share the XInput ids). */
static int cemu_sdl_id(const hubctl::Term& t) {
    static const std::map<std::string, int> m = {
        {"a", 0}, {"b", 1}, {"x", 2}, {"y", 3}, {"back", 4}, {"start", 6}, {"leftstick", 7}, {"rightstick", 8},
        {"leftshoulder", 9}, {"rightshoulder", 10}, {"dpup", 11}, {"dpdown", 12}, {"dpleft", 13}, {"dpright", 14},
        {"lefttrigger", 42}, {"righttrigger", 43}, {"leftx+", 38}, {"lefty-", 39}, {"rightx+", 40}, {"righty-", 41},
        {"leftx-", 44}, {"lefty+", 45}, {"rightx-", 46}, {"righty+", 47}};
    auto it = m.find(hubctl::term_name(t));
    return it == m.end() ? -1 : it->second;
}

/* Cemu's SDL controller uuid: "<index>_<SDL GUID string>", the index counting pads with the same GUID (SDLController.cpp). */
std::string cemu_sdl_uuid(const std::string& guid, int same_guid_index) { return std::to_string(same_guid_index) + "_" + guid; }

CemuPads cemu_pads_with_sdl(const std::vector<PadDesc>& by_port, const std::vector<PadDesc>& all_pads) {
    CemuPads c;
    const std::vector<int> slot = xinput_slots(by_port);
    for (size_t i = 0; i < by_port.size(); ++i) {
        const PadDesc& p = by_port[i];
        int idx = 0;   // position among the pads that share this GUID, in SDL order
        for (const PadDesc& q : all_pads) { if (q.key == p.key) break; if (q.guid == p.guid) ++idx; }
        if (c.xinput.size() < 4) {
            c.xinput.push_back(p.xinput ? slot[i] : -1);
            c.sdl.push_back(p.xinput ? std::string() : cemu_sdl_uuid(p.guid, idx));
        }
    }
    return c;
}

/* Cemu's DSUController ids (DSUController.cpp: bits of the DSU button bytes in order; sticks/triggers share the XInput ids).
 * Wii U A/B/X/Y sit on the DSU A/B/X/Y bits; the remote pad's XInput layout is what dsu_server.cpp maps from. */
static int cemu_dsu_id(const hubctl::Term& t) {
    static const std::map<std::string, int> m = {
        {"back", 0}, {"leftstick", 1}, {"rightstick", 2}, {"start", 3}, {"dpup", 4}, {"dpright", 5}, {"dpdown", 6}, {"dpleft", 7},
        {"leftshoulder", 10}, {"rightshoulder", 11}, {"x", 12}, {"a", 13}, {"b", 14}, {"y", 15},
        {"lefttrigger", 42}, {"righttrigger", 43}, {"leftx+", 38}, {"lefty-", 39}, {"rightx+", 40}, {"righty-", 41},
        {"leftx-", 44}, {"lefty+", 45}, {"rightx-", 46}, {"righty+", 47}};
    auto it = m.find(hubctl::term_name(t));
    return it == m.end() ? -1 : it->second;
}

static std::string cemu_pad_xml(const hubctl::Profile& prof, int xinput_idx, int player, bool pro, const std::string& sdl_uuid = "", int dsu_slot = -1) {
    const bool dsu = dsu_slot >= 0;
    const bool sdl = !dsu && !sdl_uuid.empty();
    /* VPAD ButtonId (1..24) -> profile control. The default profile reproduces the old fixed table exactly. */
    static const char* const vpad[] = {"a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus", "dpup", "dpdown", "dpleft", "dpright",
                                       "l3", "r3", "ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down", "rs_left", "rs_right"};
    auto dz = [&](const char* name) { auto it = prof.analogs.find(name); return fmt_g(it == prof.analogs.end() ? 0.25 : it->second.deadzone); };
    std::string x = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<emulated_controller>\n\t<type>") + (pro ? "Wii U Pro Controller" : "Wii U GamePad") + "</type>\n"
                    "\t<controller>\n\t\t<api>" + std::string(dsu ? "DSUController" : sdl ? "SDLController" : "XInput") + "</api>\n\t\t<uuid>" + (dsu ? std::to_string(dsu_slot) : sdl ? sdl_uuid : std::to_string(xinput_idx)) + "</uuid>\n\t\t<display_name>Controller " + std::to_string(player + 1) + "</display_name>\n"
                    "\t\t<rumble>0</rumble>\n"
                    "\t\t<axis>\n\t\t\t<deadzone>" + dz("left_stick") + "</deadzone>\n\t\t\t<range>1</range>\n\t\t</axis>\n"
                    "\t\t<rotation>\n\t\t\t<deadzone>" + dz("right_stick") + "</deadzone>\n\t\t\t<range>1</range>\n\t\t</rotation>\n"
                    "\t\t<trigger>\n\t\t\t<deadzone>0.25</deadzone>\n\t\t\t<range>1</range>\n\t\t</trigger>\n"
                    "\t\t<mappings>\n";
    for (int i = 0; i < 24; i++) {
        auto it = prof.buttons.find(vpad[i]);
        std::vector<hubctl::Term> t;
        if (it == prof.buttons.end() || !hubctl::parse_expr(it->second, prof.axis_threshold, &t) || t.empty()) continue;
        int id = dsu ? cemu_dsu_id(t[0]) : sdl ? cemu_sdl_id(t[0]) : cemu_xinput_id(t[0]);   // one source per Wii U button
        if (id < 0) continue;
        /* Cemu's ButtonId enums differ after Minus: the Pro Controller has Home at 11 (ProController.h), the GamePad has it last
         * (VPADController.h), so VPAD ids 11.. (d-pad, stick clicks, stick directions) sit one higher on a Pro Controller. */
        int mapping = (pro && i >= 10) ? i + 2 : i + 1;
        x += "\t\t\t<entry>\n\t\t\t\t<mapping>" + std::to_string(mapping) + "</mapping>\n\t\t\t\t<button>" + std::to_string(id) +
             "</button>\n\t\t\t</entry>\n";
    }
    x += "\t\t</mappings>\n\t</controller>\n</emulated_controller>\n";
    return x;
}
std::string cemu_gamepad_profile_xml(const hubctl::Profile& prof) { return cemu_pad_xml(prof, 0, 0, false); }
std::string cemu_gamepad_profile_xml(const hubctl::Profile& prof, int xinput_idx) { return cemu_pad_xml(prof, xinput_idx, 0, false); }
std::string cemu_pro_profile_xml(const hubctl::Profile& prof, int xinput_idx, int player) { return cemu_pad_xml(prof, xinput_idx, player, true); }
std::string cemu_gamepad_profile_xml() { return cemu_gamepad_profile_xml(hubctl::default_profile("wiiu")); }
std::string cemu_gamepad_profile_xml_sdl(const hubctl::Profile& prof, const std::string& sdl_uuid) { return cemu_pad_xml(prof, 0, 0, false, sdl_uuid); }
std::string cemu_pro_profile_xml_dsu(const hubctl::Profile& prof, int dsu_slot, int player) { return cemu_pad_xml(prof, 0, player, true, "", dsu_slot); }
CemuPads cemu_pads_with_dsu(const CemuPads& local, int remote_players) {
    CemuPads c;
    c.xinput.push_back(!local.xinput.empty() ? local.xinput[0] : 0);
    c.sdl.push_back(!local.sdl.empty() ? local.sdl[0] : std::string());
    c.dsu.push_back(-1);
    for (int i = 0; i < remote_players && i < 3; ++i) { c.xinput.push_back(-1); c.sdl.push_back(std::string()); c.dsu.push_back(i); }
    return c;
}
std::string cemu_pro_profile_xml_sdl(const hubctl::Profile& prof, const std::string& sdl_uuid, int player) { return cemu_pad_xml(prof, 0, player, true, sdl_uuid); }

/* Writes controller0-3.xml into `cdir` (the controllerProfiles folder) when missing or when the profile/pad order changed.
 * `need_marker`: a user's own folder is only trusted as ours when the marker exists. */
static void write_cemu_profiles(const std::string& cdir, const hubctl::Profile& prof, const CemuPads& pads, bool need_marker) {
    make_dirs(cdir);
    std::string pad = join(cdir, "controller0.xml");
    std::string marker = join(cdir, "retrohub-pad.txt");
    std::string want = hubctl::hash_hex(hubctl::mapping_hash(prof)) + pad_suffix(cemu_sel(pads));
    /* No marker = written by an older RetroHub (the built-in default) or never written. */
    auto xml = [&](int i) {
        bool sdl = i < (int)pads.sdl.size() && !pads.sdl[i].empty();
        int xi = i < (int)pads.xinput.size() && pads.xinput[i] >= 0 ? pads.xinput[i] : 0;
        int ds = i < (int)pads.dsu.size() ? pads.dsu[i] : -1;
        return cemu_pad_xml(prof, xi, i, i > 0, sdl ? pads.sdl[i] : std::string(), ds);
    };
    /* Up to date only when the marker matches AND every player file still holds what we would write: a player edited by hand in
     * Cemu (say player 2 set to the host's own pad) would otherwise survive and steal a remote guest's DSU slot. */
    bool same = file_exists(pad) && (!need_marker || file_exists(marker)) && marker_hash(file_exists(marker) ? read_text(marker) : "", "wiiu") == want;
    for (int i = 0; same && i < 4 && i < (int)pads.xinput.size(); ++i) {
        std::string f = join(cdir, "controller" + std::to_string(i) + ".xml");
        same = file_exists(f) && read_text(f) == xml(i);
    }
    if (same) return;
    write_text(pad, xml(0));   // player 1 = the Wii U GamePad
    for (int i = 1; i < 4; ++i) {   // players 2-4 = Pro Controllers on the next pads; stale ones from an earlier setup go
        std::string f = join(cdir, "controller" + std::to_string(i) + ".xml");
        if (i < (int)pads.xinput.size()) write_text(f, xml(i));
        else if (file_exists(f) && read_text(f).find("Wii U Pro Controller") != std::string::npos) { std::error_code ec; fs::remove(P(f), ec); }
    }
    write_text(marker, "RetroHub wrote controller0.xml (and controller1-3.xml for more pads) from its Controllers profile; it is rewritten only when that profile or the pad order changes.\n"
                       "profile-hash: " + want + "\n");
}

void prepare_cemu_managed(const std::string& install_dir, const hubctl::Profile& prof, const CemuPads& pads) {
    std::string portable = join(install_dir, "portable");   // Cemu's portable marker + data folder
    make_dirs(join(portable, "controllerProfiles"));
    std::string settings = join(portable, "settings.xml");
    if (!file_exists(settings)) write_text(settings, cemu_default_settings_xml());   // its absence triggers the first-run dialog
    write_cemu_profiles(join(portable, "controllerProfiles"), prof, pads, false);
}

std::string cemu_user_data_dir(const std::string& exe_dir, const std::string& appdata) {
    std::string ed = exe_dir;
    while (!ed.empty() && (ed.back() == '\\' || ed.back() == '/')) ed.pop_back();
    if (fs::is_directory(P(join(ed, "portable")))) return join(ed, "portable");
    if (file_exists(join(ed, "settings.xml"))) return ed;
    return join(appdata, "Cemu");
}

bool cemu_backup_controls(const std::string& user_data) {
    std::string cdir = join(user_data, "controllerProfiles"), bak = join(user_data, "controllerProfiles.retrohub-backup");
    std::error_code ec;
    if (fs::exists(P(bak), ec)) return true;   // once: the first backup is the user's original
    if (fs::is_directory(P(cdir), ec)) fs::copy(P(cdir), P(bak), fs::copy_options::recursive, ec);
    else fs::create_directories(P(bak), ec);   // nothing to keep: an empty backup restores to "no profiles"
    return !ec;
}

void prepare_cemu_own(const std::string& user_data, const hubctl::Profile& prof, const CemuPads& pads, bool manage) {
    if (!manage) return;   // the user's own controls stay untouched
    if (!cemu_backup_controls(user_data)) return;   // never write without a backup
    write_cemu_profiles(join(user_data, "controllerProfiles"), prof, pads, true);
}

bool cemu_restore_controls(const std::string& user_data) {
    std::string cdir = join(user_data, "controllerProfiles"), bak = join(user_data, "controllerProfiles.retrohub-backup");
    std::error_code ec;
    if (!fs::is_directory(P(bak), ec)) return false;
    fs::remove_all(P(cdir), ec);
    fs::create_directories(P(cdir), ec);
    fs::copy(P(bak), P(cdir), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    return !ec;
}
void prepare_cemu_managed(const std::string& install_dir, const hubctl::Profile& prof) { prepare_cemu_managed(install_dir, prof, CemuPads()); }
void prepare_cemu_managed(const std::string& install_dir) { prepare_cemu_managed(install_dir, stored_profile("wiiu")); }

/* ------------------------------------------------------------------ */
/* Dolphin (GameCube)                                                  */
/* ------------------------------------------------------------------ */

DEFINE_GUID(kGuidUsbDevice, 0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);   // GUID_DEVINTERFACE_USB_DEVICE

/* The Nintendo GameCube adapter (WUP-028, USB 057E:0337) counts only when its driver is WinUSB/libusbK: Dolphin cannot
 * open it through Nintendo's own driver, and a port set to "adapter" without that would have no controller at all. */
static bool gc_adapter_usable() {
    HDEVINFO set = SetupDiGetClassDevsW(&kGuidUsbDevice, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVINFO_DATA d{};
    d.cbSize = sizeof d;
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        wchar_t id[512] = {0}, svc[128] = {0};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, nullptr, (PBYTE)id, sizeof id - 2, nullptr)) continue;
        std::wstring hw = id;
        for (auto& ch : hw) ch = (wchar_t)towupper(ch);
        if (hw.find(L"VID_057E&PID_0337") == std::wstring::npos) continue;
        if (SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_SERVICE, nullptr, (PBYTE)svc, sizeof svc - 2, nullptr)) {
            std::wstring s = svc;
            for (auto& ch : s) ch = (wchar_t)towupper(ch);
            found = s == L"WINUSB" || s == L"LIBUSBK" || s == L"LIBUSB0";
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

DolphinPads detect_dolphin_pads() {
    DolphinPads p;
    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE st{};
        p.xinput[i] = XInputGetState(i, &st) == ERROR_SUCCESS;
    }
    p.adapter = gc_adapter_usable();
    return p;
}

std::string dolphin_ini_defaults(const std::string& in) {
    std::string t = in;
    t = hubui::ini_set(t, "Analytics", "Enabled", "False");
    t = hubui::ini_set(t, "Analytics", "PermissionAsked", "True");
    t = hubui::ini_set(t, "AutoUpdate", "UpdateTrack", "");
    t = hubui::ini_set(t, "Interface", "ConfirmStop", "False");
    t = hubui::ini_set(t, "Interface", "UsePanicHandlers", "False");
    t = hubui::ini_set(t, "Interface", "ShowActiveTitle", "False");
    t = hubui::ini_set(t, "Display", "RenderToMain", "False");
    t = hubui::ini_set(t, "Display", "KeepWindowOnTop", "False");
    t = hubui::ini_set(t, "Input", "BackgroundInput", "False");              // Dolphin must ignore the pad while the hub menu has focus
    t = hubui::ini_set(t, "General", "HotkeysRequireFocus", "False");        // the speed hotkeys come from the hub's DSU pad (dolphin_speed.cpp): no focus needed
    t = hubui::ini_set(t, "Core", "SkipIPL", "True");
    t = hubui::ini_set(t, "Interface", "SkipNKitWarning", "True");          // NKit images (.nkit.iso) otherwise stop at a dialog
    if (hubui::ini_get(t, "Core", "GFXBackend").empty()) t = hubui::ini_set(t, "Core", "GFXBackend", "D3D");   // Direct3D 11
    if (hubui::ini_get(t, "DSP", "Volume").empty()) t = hubui::ini_set(t, "DSP", "Volume", "100");
    return t;
}

/* Per launch: what must follow the current situation. SlotA = GCI folder (the vault copy), card B empty. A pad is plugged into
 * a port where an XInput pad is connected (port 1 always, keyboard fallback); with a usable GameCube adapter all four ports
 * are the adapter's. SIDevice: 0 = nothing, 6 = standard pad, 12 = adapter. */
std::string dolphin_ini_ports(const std::string& in, const DolphinPads& pads) {
    std::string t = in;
    t = hubui::ini_set(t, "Core", "SlotA", "8");
    t = hubui::ini_set(t, "Core", "SlotB", "255");
    t = hubui::ini_set(t, "Core", "CPUThread", "True");       // dual core (Dolphin's default); NetPlay used to leave it off in this shared ini, which made later play slow
    t = hubui::ini_set(t, "Core", "EmulationSpeed", "1.0");   // the Game menu's Speed row starts every session at 1x
    t = hubui::ini_set(t, "General", "HotkeysRequireFocus", "False");   // every launch (upgraded installs too): the speed pad needs no focus; other hotkeys are unbound (dolphin_hotkeys_ini)
    t = hubui::ini_set(t, "Interface", "ConfirmStop", "False");         // every launch: Back+Start closes Dolphin; a "stop?" dialog would pop up over the game
    t = hubui::ini_set(t, "Interface", "LanguageCode", "en");           // every launch: the NetPlay automation finds Dolphin's menus by their English names
    t = hubui::ini_set(t, "Input", "BackgroundInput", "False");         // every launch, not only first setup: Dolphin ignores the pad while the menu has focus
    t = hubui::ini_set(t, "Core", "SkipIPL", "True");   // every launch; the offline "real console startup" turns it off for that launch only (real_boot_ui.cpp)
    for (int i = 0; i < 4; ++i)
        t = hubui::ini_set(t, "Core", "SIDevice" + std::to_string(i), pads.adapter ? "12" : (i == 0 || pads.xinput[i] || !pads.dev[i].empty()) ? "6" : "0");
    return t;
}

/* Hotkeys.ini: the two emulation-speed hotkeys on Ctrl+Shift+F13 / F14 (keys nobody presses), which the Game menu's Speed row
 * sends after focusing Dolphin. Other lines (the user's own hotkeys) are kept. */
/* True when a Dolphin hotkey expression reads the keyboard / mouse: an explicit `...Keyboard Mouse:...` term, or a bare term (no backticks)
 * while the section's default Device is the keyboard. Dolphin's expression language has no focus function, so such a binding cannot be
 * guarded; pad-only bindings (`XInput/0/Gamepad:...`, `SDL/...`, `DSUClient/...`, or bare names with a pad Device) are safe and kept. */
static bool hotkey_reads_keyboard(const std::string& expr, bool device_is_keyboard) {
    std::string bare;
    bool in_tick = false;
    std::string term;
    for (char c : expr) {
        if (c == '`') {
            if (in_tick) { if (term.find("Keyboard") != std::string::npos || term.find("Mouse") != std::string::npos) return true; term.clear(); }
            in_tick = !in_tick;
            continue;
        }
        if (in_tick) term += c; else bare += c;
    }
    if (!device_is_keyboard) return false;
    // bare text: drop function names ("name(" / "@("), numbers and operators; any name left is a control of the default device
    for (size_t i = 0; i < bare.size(); ++i) {
        if (!std::isalpha((unsigned char)bare[i])) continue;
        size_t j = i;
        while (j < bare.size() && (std::isalnum((unsigned char)bare[j]) || bare[j] == '_')) ++j;
        size_t k = j;
        while (k < bare.size() && bare[k] == ' ') ++k;
        if (k < bare.size() && bare[k] == '(') { i = j; continue; }   // a function, not a control
        return true;
    }
    return false;
}

/* Dolphin.ini sets HotkeysRequireFocus = False (the speed pad must work without focus). In Dolphin 2509 that one switch opens the focus
 * gate for EVERY hotkey (DolphinQt/HotkeyScheduler.cpp: Core::UpdateInputGate(MAIN_FOCUSED_HOTKEYS) before all of them), and its
 * keyboard backend reads keys globally: a default binding such as Stop = Escape or Toggle Pause = F10 would fire while the user types in
 * another app. So every other hotkey in RetroHub's own Dolphin is unbound; the quick menu covers stop / pause / states. */
/* `val` without RetroHub's chord `expr` and the `|` that joined it: the user's own (pad) part of the binding. */
static std::string strip_chord(std::string val, const std::string& expr) {
    for (size_t p; (p = val.find(expr)) != std::string::npos;) val.erase(p, expr.size());
    const size_t a = val.find_first_not_of(" |\t\r"), b = val.find_last_not_of(" |\t\r");
    return a == std::string::npos ? std::string() : val.substr(a, b - a + 1);
}

static std::string blank_other_hotkeys(const std::string& in) {
    std::string out, line;
    bool in_hk = false;
    const std::string dev = hubui::ini_get(in, "Hotkeys", "Device");
    const bool dev_kb = dev.empty() || dev.find("Keyboard") != std::string::npos;
    size_t i = 0;
    while (i <= in.size()) {
        size_t nl = in.find('\n', i);
        line = in.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        std::string tl = line;
        while (!tl.empty() && (tl.back() == '\r' || tl.back() == ' ')) tl.pop_back();
        size_t a = tl.find_first_not_of(" \t");
        if (a != std::string::npos && tl[a] == '[') in_hk = tl.substr(a) == "[Hotkeys]";
        else if (in_hk && a != std::string::npos && tl[a] != ';' && tl[a] != '#') {
            size_t eq = tl.find('=');
            if (eq != std::string::npos) {
                std::string key = tl.substr(a, eq - a), val = tl.substr(eq + 1);
                while (!key.empty() && key.back() == ' ') key.pop_back();
                const bool ours = key == "Device" || key == hubqm::dolphin_hotkey_name(hubqm::DolphinKey::Increase) || key == hubqm::dolphin_hotkey_name(hubqm::DolphinKey::Decrease);
                const std::string cr = !line.empty() && line.back() == '\r' ? "\r" : "";
                bool act_key = false;
                for (hubqm::DolphinAct a : hubqm::dolphin_acts()) if (key == hubqm::dolphin_act_hotkey_name(a)) {   // Pause / Save / Load: our chord is dropped, the user's pad-only part stays
                    act_key = true;
                    const std::string rest = strip_chord(val, hubqm::dolphin_act_hotkey_expr(a));
                    line = key + " = " + (!rest.empty() && !hotkey_reads_keyboard(rest, dev_kb) ? rest : std::string()) + cr;
                }
                if (!ours && !act_key && val.find_first_not_of(" ") != std::string::npos && hotkey_reads_keyboard(val, dev_kb)) line = key + " = " + cr;
            }
        }
        out += line;
        if (nl == std::string::npos) break;
        out += '\n';
        i = nl + 1;
    }
    return out;
}

std::string dolphin_hotkeys_ini(const std::string& in) {
    std::string t = blank_other_hotkeys(in);
    if (hubui::ini_get(t, "Hotkeys", "Device").empty()) t = hubui::ini_set(t, "Hotkeys", "Device", "DInput/0/Keyboard Mouse");
    for (auto k : {hubqm::DolphinKey::Increase, hubqm::DolphinKey::Decrease})
        t = hubui::ini_set(t, "Hotkeys", hubqm::dolphin_hotkey_name(k), hubds::hotkey_expr(k));   // DSU pad button | Ctrl+Shift+F13/F14
    for (hubqm::DolphinAct a : hubqm::dolphin_acts()) {   // the Game menu's Pause / Save state / Load state (slot 1): chords nobody presses (qm_logic.h); a user's pad-only binding is kept beside it
        const std::string keep = hubui::ini_get(t, "Hotkeys", hubqm::dolphin_act_hotkey_name(a)), expr = hubqm::dolphin_act_hotkey_expr(a);
        t = hubui::ini_set(t, "Hotkeys", hubqm::dolphin_act_hotkey_name(a), keep.empty() ? expr : keep + " | " + expr);
    }
    return t;
}

/* DSUClient.ini (Dolphin's DSU client): one server, the hub's speed pad (dolphin_speed.cpp). Other entries are kept. */
std::string dolphin_dsu_ini(const std::string& in) {
    std::string t = hubui::ini_set(in, "Server", "Enabled", "True");
    const std::string e = hubui::ini_get(t, "Server", "Entries");
    const std::string want = hubds::merge_entries(e);   // the port the server got (dolphin_speed_start runs before prepare_dolphin)
    if (e != want) t = hubui::ini_set(t, "Server", "Entries", want);
    return t;
}

std::string dolphin_dolphin_ini_patch(const std::string& in, const DolphinPads& pads) {
    return dolphin_ini_ports(dolphin_ini_defaults(in), pads);
}

std::string dolphin_gfx_ini_patch(const std::string& in) {
    std::string t = in;
    t = hubui::ini_set(t, "Settings", "InternalResolution", "2");           // 2x native (1280x1056)
    t = hubui::ini_set(t, "Settings", "wideScreenHack", "False");
    t = hubui::ini_set(t, "Settings", "ShaderCompilationMode", "2");        // asynchronous ubershaders: no compile stutter
    t = hubui::ini_set(t, "Settings", "WaitForShadersBeforeStarting", "False");
    t = hubui::ini_set(t, "Settings", "BorderlessFullscreen", "True");      // fullscreen without a mode switch
    t = hubui::ini_set(t, "Settings", "ShowFPS", "False");
    return t;
}

/* SDL source -> Dolphin's XInput device control names. */
static std::string dolphin_xinput_name(const hubctl::Term& t) {
    static const std::map<std::string, const char*> m = {
        {"a", "`Button A`"}, {"b", "`Button B`"}, {"x", "`Button X`"}, {"y", "`Button Y`"}, {"leftshoulder", "`Shoulder L`"},
        {"rightshoulder", "`Shoulder R`"}, {"start", "Start"}, {"back", "Back"}, {"leftstick", "`Thumb L`"}, {"rightstick", "`Thumb R`"},
        {"dpup", "`Pad N`"}, {"dpdown", "`Pad S`"}, {"dpleft", "`Pad W`"}, {"dpright", "`Pad E`"},
        {"lefttrigger", "`Trigger L`"}, {"righttrigger", "`Trigger R`"},
        {"leftx+", "`Left X+`"}, {"leftx-", "`Left X-`"}, {"lefty-", "`Left Y+`"}, {"lefty+", "`Left Y-`"},
        {"rightx+", "`Right X+`"}, {"rightx-", "`Right X-`"}, {"righty-", "`Right Y+`"}, {"righty+", "`Right Y-`"}};
    auto it = m.find(hubctl::term_name(t));
    return it == m.end() ? "" : it->second;
}

/* SDL source -> Dolphin's SDL device control names ("Button S" = SDL south = a; Y- is up on this backend). */
static std::string dolphin_sdl_name(const hubctl::Term& t) {
    static const std::map<std::string, const char*> m = {
        {"a", "`Button S`"}, {"b", "`Button E`"}, {"x", "`Button W`"}, {"y", "`Button N`"}, {"leftshoulder", "`Shoulder L`"},
        {"rightshoulder", "`Shoulder R`"}, {"start", "Start"}, {"back", "Back"}, {"guide", "Guide"}, {"leftstick", "`Thumb L`"},
        {"rightstick", "`Thumb R`"}, {"dpup", "`Pad N`"}, {"dpdown", "`Pad S`"}, {"dpleft", "`Pad W`"}, {"dpright", "`Pad E`"},
        {"lefttrigger", "`Trigger L`"}, {"righttrigger", "`Trigger R`"},
        {"leftx+", "`Left X+`"}, {"leftx-", "`Left X-`"}, {"lefty-", "`Left Y-`"}, {"lefty+", "`Left Y+`"},
        {"rightx+", "`Right X+`"}, {"rightx-", "`Right X-`"}, {"righty-", "`Right Y-`"}, {"righty+", "`Right Y+`"}};
    auto it = m.find(hubctl::term_name(t));
    return it == m.end() ? "" : it->second;
}

/* Key name -> Dolphin's DInput keyboard names (upper-case letters / digits; its defaults use UP, RETURN ...). */
std::string dolphin_key_name(const std::string& k) {
    if (k.size() == 1) return std::string(1, (char)std::toupper((unsigned char)k[0]));
    static const std::map<std::string, const char*> m = {
        {"up", "UP"}, {"down", "DOWN"}, {"left", "LEFT"}, {"right", "RIGHT"}, {"return", "RETURN"}, {"kp_enter", "NUMPADENTER"},
        {"space", "SPACE"}, {"tab", "TAB"}, {"backspace", "BACK"}, {"lshift", "LSHIFT"}, {"rshift", "RSHIFT"}, {"lctrl", "LCONTROL"},
        {"rctrl", "RCONTROL"}, {"lalt", "LMENU"}, {"ralt", "RMENU"}, {"comma", "COMMA"}, {"period", "PERIOD"}, {"slash", "SLASH"},
        {"semicolon", "SEMICOLON"}, {"quote", "APOSTROPHE"}, {"lbracket", "LBRACKET"}, {"rbracket", "RBRACKET"}, {"minus", "MINUS"},
        {"equals", "EQUALS"}, {"backslash", "BACKSLASH"}, {"grave", "GRAVE"}, {"insert", "INSERT"}, {"delete", "DELETE"},
        {"home", "HOME"}, {"end", "END"}, {"pageup", "PRIOR"}, {"pagedown", "NEXT"}};
    auto it = m.find(k);
    if (it != m.end()) return it->second;
    if (k.size() >= 2 && k[0] == 'f' && std::isdigit((unsigned char)k[1])) return "F" + k.substr(1);
    if (k.size() == 3 && k[0] == 'k' && k[1] == 'p' && std::isdigit((unsigned char)k[2])) return std::string("NUMPAD") + k[2];
    return "";
}

DolphinPads dolphin_pads_with(DolphinPads base, const std::vector<PadDesc>& by_port, const std::vector<PadDesc>& by_sdl) {
    const std::vector<int> slot = xinput_slots(by_port);
    for (size_t i = 0; i < by_port.size() && i < 4; ++i) {
        const PadDesc& p = by_port[i];
        if (p.xinput) {
            /* Dolphin addresses an XInput pad by its user slot. Only a pad that already sits on slot == port keeps Dolphin's own default device. */
            if ((int)i != slot[i]) base.dev[i] = "XInput/" + std::to_string(slot[i]) + "/Gamepad";
        } else {
            int n = 0;   // Dolphin numbers same-named SDL devices in SDL's order
            for (const PadDesc& d : by_sdl) { if (d.key == p.key) break; if (d.name == p.name) ++n; }
            base.dev[i] = "SDL/" + std::to_string(n) + "/" + p.name;
        }
    }
    /* Which ports have a pad = the ports the RetroHub port order filled, not "XInput slot n is connected": with pads on slots 0 and 2 the
     * second player's pad is XInput/2, and port 3 must stay empty (it used to turn on too, so one pad drove two players). */
    if (!by_port.empty()) for (int i = 0; i < 4; ++i) base.xinput[i] = (size_t)i < by_port.size();
    return base;
}

std::string dolphin_pad_ini(const std::string& in, const DolphinPads& pads, const hubctl::Profile& prof) {
    std::string t = in;
    const std::string kb = "DInput/0/Keyboard Mouse";
    struct M { const char* control; const char* key; const char* key_kb; };
    static const M map[] = {   // profile control -> GC control; keyboard fallback on port 1 (Dolphin's own default keys)
        {"a", "Buttons/A", "X"},        {"b", "Buttons/B", "Z"},      {"x", "Buttons/X", "C"},
        {"y", "Buttons/Y", "S"},        {"z", "Buttons/Z", "D"},      {"start", "Buttons/Start", "RETURN"},
        {"main_up", "Main Stick/Up", "UP"},    {"main_down", "Main Stick/Down", "DOWN"},
        {"main_left", "Main Stick/Left", "LEFT"}, {"main_right", "Main Stick/Right", "RIGHT"},
        {"c_up", "C-Stick/Up", "I"},       {"c_down", "C-Stick/Down", "K"},
        {"c_left", "C-Stick/Left", "J"},     {"c_right", "C-Stick/Right", "L"},
        {"l", "Triggers/L", "Q"},      {"r", "Triggers/R", "W"},
        {"l_analog", "Triggers/L-Analog", ""}, {"r_analog", "Triggers/R-Analog", ""},
        {"dpad_up", "D-Pad/Up", "T"},            {"dpad_down", "D-Pad/Down", "G"},
        {"dpad_left", "D-Pad/Left", "F"},          {"dpad_right", "D-Pad/Right", "H"},
    };
    for (int i = 0; i < 4; ++i) {
        std::string sec = "GCPad" + std::to_string(i + 1);
        const std::string dev = pads.dev[i].empty() ? "XInput/" + std::to_string(i) + "/Gamepad" : pads.dev[i];
        const bool sdl = dev.compare(0, 4, "SDL/") == 0;
        t = hubui::ini_set(t, sec, "Device", dev);
        for (const auto& m : map) {
            std::string v;
            auto it = prof.buttons.find(m.control);
            std::vector<hubctl::Term> terms;
            if (it != prof.buttons.end() && hubctl::parse_expr(it->second, prof.axis_threshold, &terms))
                for (const auto& term : terms) { std::string n = sdl ? dolphin_sdl_name(term) : dolphin_xinput_name(term); if (!n.empty()) v += (v.empty() ? "" : " | ") + n; }
            if (i == 0) {   // the keyboard pad (profile keys; the defaults are Dolphin's own)
                auto kit = prof.keys.find(m.control);
                std::vector<int> codes;
                if (kit != prof.keys.end() && hubctl::parse_keys(kit->second, &codes))
                    for (int c : codes) { std::string kn = dolphin_key_name(hubctl::key_name(c)); if (!kn.empty()) v += (v.empty() ? "" : " | ") + std::string("`") + kb + ":" + kn + "`"; }
            }
            t = hubui::ini_set(t, sec, m.key, v);
        }
        /* A deadzone is only written when the player set one (0 = Dolphin's own default). */
        for (const auto& st : {std::make_pair("main_stick", "Main Stick"), std::make_pair("c_stick", "C-Stick")}) {
            auto it = prof.analogs.find(st.first);
            if (it != prof.analogs.end() && it->second.deadzone > 0) t = hubui::ini_set(t, sec, std::string(st.second) + "/Dead Zone", fmt_g(it->second.deadzone * 100));
        }
        t = hubui::ini_set(t, sec, "Rumble/Motor", "`Motor L`|`Motor R`");
    }
    return t;
}
std::string dolphin_pad_ini(const std::string& in, const DolphinPads& p) { return dolphin_pad_ini(in, p, hubctl::default_profile("gc")); }
static std::string dolphin_sel(const DolphinPads& p) {
    std::string s;
    for (int i = 0; i < 4; ++i) if (!p.dev[i].empty()) s += std::to_string(i) + "=" + p.dev[i] + ";";
    return s;
}

void prepare_dolphin(const std::string& install_dir, const DolphinPads& pads, const hubctl::Profile& prof) {
    std::string cfg = join(install_dir, "User\\Config");
    make_dirs(cfg);
    if (!file_exists(join(install_dir, "portable.txt"))) write_text(join(install_dir, "portable.txt"), "");
    /* Written once (marker), so settings changed later in Dolphin's own dialogs or by hand are kept: Dolphin.ini defaults,
     * GFX.ini defaults and the pad mappings. The pad mappings are rewritten when the RetroHub profile changed. */
    std::string ini = join(cfg, "Dolphin.ini"), gfx = join(cfg, "GFX.ini"), pad = join(cfg, "GCPadNew.ini");
    std::string marker = join(cfg, "retrohub-defaults.txt");
    std::string want = hubctl::hash_hex(hubctl::mapping_hash(prof)) + pad_suffix(dolphin_sel(pads));
    const char* kText = "RetroHub wrote its default Dolphin settings and pad mappings once. Delete this file to have them written again.\n";
    if (!file_exists(marker)) {
        write_text(ini, dolphin_ini_defaults(read_text(ini)));
        write_text(gfx, dolphin_gfx_ini_patch(read_text(gfx)));
        write_text(pad, dolphin_pad_ini(read_text(pad), pads, prof));
        write_text(marker, std::string(kText) + "profile-hash: " + want + "\n");
    } else if (marker_hash(read_text(marker), "gc") != want) {
        write_text(pad, dolphin_pad_ini(read_text(pad), pads, prof));
        write_text(marker, std::string(kText) + "profile-hash: " + want + "\n");
    }
    /* Every launch: only the memory card slots and which ports have a pad plugged in. */
    write_text(ini, dolphin_ini_ports(read_text(ini), pads));
    {   // keyboard hotkeys are unbound (focus-free hotkeys would fire in other apps); the user's originals are kept once in a backup
        const std::string hk_path = join(cfg, "Hotkeys.ini"), old = read_text(hk_path), now = dolphin_hotkeys_ini(old);
        const std::string bak = join(cfg, "Hotkeys.retrohub-backup.ini");
        std::error_code ec;
        if (!old.empty() && old != now && !std::filesystem::exists(std::filesystem::u8path(bak), ec)) write_text(bak, old);
        write_text(hk_path, now);
    }
    write_text(join(cfg, "DSUClient.ini"), dolphin_dsu_ini(read_text(join(cfg, "DSUClient.ini"))));
}
void prepare_dolphin(const std::string& install_dir, const DolphinPads& pads) { prepare_dolphin(install_dir, pads, stored_profile("gc")); }

/* True when a process whose image is exactly this file is running (is_running matches the bare file name, so an unrelated
 * Dolphin.exe elsewhere would wrongly keep an old download alive and make the cleanup depend on what else runs). */
static bool running_from(const std::string& exe) {
    std::wstring want = P(exe).wstring();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, P(exe).filename().wstring().c_str()) != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) { found = true; continue; }   // cannot tell: be careful and treat it as running
        wchar_t buf[MAX_PATH * 2];
        DWORD n = (DWORD)(sizeof buf / sizeof buf[0]);
        if (QueryFullProcessImageNameW(h, 0, buf, &n) && _wcsicmp(buf, want.c_str()) == 0) found = true;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}
/* Older RetroHub builds ran a Dolphin downloaded to <exe>\dolphin with its user folder at <data>\dolphin-user, and kept the
 * memory card straight in the vault (<vault>\gc\<game>\battery\GCI): that location is the one the new sync still reads, so
 * saves need no move. This only tidies up: save states move to the new User folder, the old user folder is renamed (never
 * deleted), and an untouched old download (exe + portable.txt, no User folder of its own) is removed. */
void dolphin_migrate_old(const std::string& data_dir, const std::string& exe_dir, const std::string& install_dir) {
    std::error_code ec;
    std::string old_user = join(data_dir, "dolphin-user");
    if (dir_exists(old_user) && !dir_exists(old_user + ".migrated")) {
        std::string st = join(old_user, "StateSaves"), nst = join(install_dir, "User\\StateSaves");
        if (dir_exists(st) && !dir_exists(nst)) {
            make_dirs(S(P(nst).parent_path()));
            fs::copy(P(st), P(nst), fs::copy_options::recursive, ec);
        }
        fs::rename(P(old_user), P(old_user + ".migrated"), ec);
    }
    std::string old_inst = join(exe_dir, "dolphin");
    if (file_exists(join(old_inst, "Dolphin.exe")) && file_exists(join(old_inst, "portable.txt")) && !dir_exists(join(old_inst, "User")) &&
        !file_exists(join(old_inst, "retrohub-install.txt")) && !running_from(join(old_inst, "Dolphin.exe")))
        remove_tree(old_inst);
}

std::string pcsx2_safe_key(const std::string& key) {
    std::string k = key;
    for (char& ch : k) if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_' && ch != '.') ch = '_';
    return k;
}
std::string pcsx2_card_dir_rel(const std::string& key) { return "memcards/RetroHub/" + pcsx2_safe_key(key); }

std::string dolphin_card_root(const std::string& install_dir, const std::string& key) {
    std::string k = key;
    for (char& ch : k) if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_' && ch != '.') ch = '_';
    return join(install_dir, "User\\GC\\RetroHub\\" + k);
}

/* ---- per-game settings (src/common/game_settings.h): applied just before each launch ---- */
std::string dolphin_gfx_apply(const std::string& in, const hub::gs::Values& v) {
    std::string t = in;
    auto g = [&](const char* k, const char* d) { auto i = v.find(k); return i == v.end() ? std::string(d) : i->second; };
    t = hubui::ini_set(t, "Settings", "InternalResolution", g("gc.res", "2"));
    t = hubui::ini_set(t, "Settings", "wideScreenHack", g("gc.widescreen", "false") == "true" ? "True" : "False");
    return t;
}

std::string azahar_config_apply(const std::string& in, const hub::gs::Values& v) {
    std::string t = in;
    auto g = [&](const char* k, const char* d) { auto i = v.find(k); return i == v.end() ? std::string(d) : i->second; };
    auto set = [&](const char* sec, const std::string& key, const std::string& val) {
        t = hubui::ini_set(t, sec, key + "\\default", "false");
        t = hubui::ini_set(t, sec, key, val);
    };
    set("Renderer", "resolution_factor", g("3ds.res", "1"));
    std::string l = g("3ds.layout", "default");
    set("Layout", "layout_option", l == "single" ? "1" : l == "large" ? "2" : l == "side" ? "3" : l == "hybrid" ? "5" : "0");
    return t;
}

std::string cemu_settings_apply(const std::string& in, const hub::gs::Values& v) {
    /* Cemu has no switch for graphic packs; the <GraphicPack> list (the user's enabled packs) is renamed away and back,
     * so turning them off never loses the selection. */
    auto i = v.find("wiiu.graphic_packs");
    bool on = i == v.end() || i->second != "false";
    std::string t = in;
    auto rename = [&](const std::string& from, const std::string& to) {
        for (const char* pre : {"<", "</"}) {
            std::string a = std::string(pre) + from + ">", b = std::string(pre) + to + ">";
            for (size_t p = 0; (p = t.find(a, p)) != std::string::npos; p += b.size()) t.replace(p, a.size(), b);
        }
    };
    if (on) rename("GraphicPack_off_by_retrohub", "GraphicPack");
    else rename("GraphicPack", "GraphicPack_off_by_retrohub");
    return t;
}

void apply_game_settings(Kind k, const std::string& install_dir, const hub::gs::Values& v) {
    if (k == Kind::Dolphin) {
        std::string f = join(install_dir, "User\\Config\\GFX.ini");
        make_dirs(join(install_dir, "User\\Config"));
        write_text(f, dolphin_gfx_apply(read_text(f), v));
    } else if (k == Kind::Azahar) {
        std::string d = join(install_dir, "user\\config");
        make_dirs(d);
        std::string f = join(d, "qt-config.ini");
        write_text(f, azahar_config_apply(read_text(f), v));
    } else if (k == Kind::Pcsx2 || k == Kind::Ppsspp || is_nx(k)) {
        return;   // written by prepare_pcsx2 / prepare_ppsspp (pcsx2.h, ppsspp.h)
    } else {   // RetroHub's own portable Cemu (a user's own install goes through prepare_cemu_own_settings)
        std::string f = join(install_dir, "portable\\settings.xml");
        if (file_exists(f)) write_text(f, cemu_fullscreen_apply(cemu_settings_apply(read_text(f), v), hub::gs::is_fullscreen(v)));
    }
}

/* ---- Wii U: the user's own Cemu ---- */

static std::string cemu_tag_value(const std::string& xml, const std::string& tag) {
    std::string a = "<" + tag + ">", b = "</" + tag + ">";
    size_t p = xml.find(a);
    if (p == std::string::npos) return "";
    size_t e = xml.find(b, p);
    if (e == std::string::npos) return "";
    return xml.substr(p + a.size(), e - p - a.size());
}

std::string cemu_fullscreen_value(const std::string& xml) {
    std::string v = cemu_tag_value(xml, "fullscreen");
    return v == "true" || v == "false" ? v : std::string();
}

std::string cemu_fullscreen_apply(const std::string& in, bool fs) {
    const std::string val = fs ? "true" : "false";
    std::string t = in;
    size_t p = t.find("<fullscreen>");
    if (p != std::string::npos) {
        size_t e = t.find("</fullscreen>", p);
        if (e != std::string::npos) { t.replace(p + 12, e - p - 12, val); return t; }
    }
    size_t sc = t.find("<fullscreen/>");
    if (sc != std::string::npos) { t.replace(sc, 13, "<fullscreen>" + val + "</fullscreen>"); return t; }
    size_t end = t.rfind("</content>");
    if (end == std::string::npos) return t;   // not a Cemu settings file: leave it alone
    t.insert(end, "\t<fullscreen>" + val + "</fullscreen>\n");
    return t;
}

/* The <GraphicPack>...</GraphicPack> element (empty string when absent or self-closing = nothing enabled). */
static bool cemu_gp_span(const std::string& xml, size_t* from, size_t* to) {
    size_t p = xml.find("<GraphicPack>");
    if (p == std::string::npos) return false;
    size_t e = xml.find("</GraphicPack>", p);
    if (e == std::string::npos) return false;
    *from = p;
    *to = e + 14;
    return true;
}

std::string cemu_own_settings_apply(const std::string& in, const hub::gs::Values& v, CemuOwnState* st) {
    std::string t = in;
    const bool fs = hub::gs::is_fullscreen(v);
    st->fs_orig = cemu_fullscreen_value(t);
    t = cemu_fullscreen_apply(t, fs);
    st->fs_written = fs ? "true" : "false";
    st->fs_touched = true;
    auto i = v.find("wiiu.graphic_packs");
    if (i != v.end() && i->second == "false") {
        size_t a, b;
        if (cemu_gp_span(t, &a, &b)) {   // the user's selection is parked in the state file, Cemu sees no packs
            st->gp_block = t.substr(a, b - a);
            st->gp_stripped = true;
            t.replace(a, b - a, "<GraphicPack/>");
        }
    }
    return t;
}

std::string cemu_own_settings_revert(const std::string& in, const CemuOwnState& st) {
    std::string t = in;
    if (st.fs_touched && cemu_fullscreen_value(t) == st.fs_written)   // unchanged since: the user did not toggle it in the session
        t = cemu_fullscreen_apply(t, st.fs_orig.empty() ? false : st.fs_orig == "true");
    if (st.gp_stripped && !st.gp_block.empty()) {
        size_t sc = t.find("<GraphicPack/>");
        size_t a, b;
        if (sc != std::string::npos) t.replace(sc, 14, st.gp_block);
        else if (cemu_gp_span(t, &a, &b) && t.substr(a, b - a).find("<Entry") == std::string::npos) t.replace(a, b - a, st.gp_block);   // Cemu wrote an empty list
        /* a non-empty list: the user enabled packs during the session; theirs wins */
    }
    return t;
}

static std::string state_esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '\\') o += "\\\\"; else if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r"; else o += c; }
    return o;
}
static std::string state_unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) { char n = s[++i]; o += n == 'n' ? '\n' : n == 'r' ? '\r' : n; }
        else o += s[i];
    }
    return o;
}

std::string cemu_own_state_text(const CemuOwnState& st) {
    return std::string("RetroHub changed these Cemu settings for a game session and puts them back afterwards. Do not edit.\n") +
           "fs_touched=" + (st.fs_touched ? "1" : "0") + "\nfs_orig=" + st.fs_orig + "\nfs_written=" + st.fs_written +
           "\ngp_stripped=" + (st.gp_stripped ? "1" : "0") + "\ngp_block=" + state_esc(st.gp_block) + "\n";
}

bool cemu_own_state_parse(const std::string& text, CemuOwnState* st) {
    CemuOwnState o;
    bool any = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "fs_touched") { o.fs_touched = v == "1"; any = true; }
        else if (k == "fs_orig") o.fs_orig = v;
        else if (k == "fs_written") o.fs_written = v;
        else if (k == "gp_stripped") o.gp_stripped = v == "1";
        else if (k == "gp_block") o.gp_block = state_unesc(v);
    }
    if (any) *st = o;
    return any;
}

bool revert_cemu_own_settings(const std::string& user_data) {
    std::string sf = join(user_data, "retrohub-cemu-state.txt"), settings = join(user_data, "settings.xml");
    if (!file_exists(sf)) return false;
    CemuOwnState st;
    bool ok = cemu_own_state_parse(read_text(sf), &st);
    if (ok && file_exists(settings)) write_text(settings, cemu_own_settings_revert(read_text(settings), st));
    std::error_code ec;
    fs::remove(P(sf), ec);
    return ok;
}

bool prepare_cemu_own_settings(const std::string& user_data, const hub::gs::Values& v) {
    std::string settings = join(user_data, "settings.xml");
    if (!file_exists(settings)) return false;   // Cemu has not run yet: nothing of the user's to protect, and the first-run dialog stays theirs
    revert_cemu_own_settings(user_data);        // leftovers of a session that never finished
    std::string bak = settings + ".retrohub-backup";
    std::string orig = read_text(settings);
    if (!file_exists(bak) && !write_text(bak, orig)) return false;   // never change the file without a backup
    CemuOwnState st;
    std::string out = cemu_own_settings_apply(orig, v, &st);
    if (!write_text(join(user_data, "retrohub-cemu-state.txt"), cemu_own_state_text(st))) return false;
    return write_text(settings, out);
}

std::string cemu_profile_apply(const std::string& in, const hub::gs::Values& v) {
    std::string t = in;
    auto i = v.find("wiiu.cpu_mode");
    const char* mode = i == v.end() ? "" : hub::wiiu_cpu_mode_ini(i->second);
    if (*mode) t = hub::ini_put(t, "CPU", "cpuMode", mode);
    return t;
}

/* Games whose Vulkan path makes the GPU driver reset (Windows nvlddmkm event 153, Cemu "vkWaitForFences ... -4")
 * often enough to freeze at boot: Super Mario 3D World (US, EU, JP) on the intro. They run on OpenGL instead. */
bool cemu_needs_opengl(uint64_t tid) {
    switch (tid) {
    case 0x0005000010145c00ull: case 0x0005000010145d00ull: case 0x0005000010106100ull: return true;
    default: return false;
    }
}

bool apply_cemu_game_profile(const std::string& user_data, uint64_t tid, const hub::gs::Values& v) {
    if (!tid) return false;
    std::string dir = join(user_data, "gameProfiles"), f = join(dir, hub::wiiu_profile_file_name(tid));
    revert_cemu_game_profile(user_data, tid);
    std::string cur = file_exists(f) ? read_text(f) : std::string();
    std::string out = cemu_profile_apply(cur, v);
    if (cemu_needs_opengl(tid) && hub::ini_get(cur, "Graphics", "graphics_api").empty())   // a user's own choice wins
        out = hub::ini_put(out, "Graphics", "graphics_api", "0");   // Cemu GraphicAPI: 0 OpenGL, 1 Vulkan
    if (out == cur) return false;   // nothing to set: leave the user's profile alone
    make_dirs(dir);
    if (file_exists(f)) { if (!write_text(f + ".retrohub-orig", cur)) return false; }
    else if (!write_text(f + ".retrohub-new", "RetroHub created " + hub::wiiu_profile_file_name(tid) + " for a game session and removes it afterwards.\n")) return false;
    return write_text(f, out);
}

bool revert_cemu_game_profile(const std::string& user_data, uint64_t tid) {
    if (!tid) return false;
    std::string f = join(join(user_data, "gameProfiles"), hub::wiiu_profile_file_name(tid));
    std::error_code ec;
    if (file_exists(f + ".retrohub-orig")) {
        bool ok = write_text(f, read_text(f + ".retrohub-orig"));
        if (ok) fs::remove(P(f + ".retrohub-orig"), ec);
        return ok;
    }
    if (file_exists(f + ".retrohub-new")) { fs::remove(P(f), ec); fs::remove(P(f + ".retrohub-new"), ec); return true; }
    return false;
}

std::string cemu_profile_summary(const std::string& exe_dir, const std::string& user_data, uint64_t tid) {
    if (!tid) return "";
    std::string name = hub::wiiu_profile_file_name(tid);
    auto find_default = [&]() {   // Cemu ships names in mixed case; match case-insensitively
        std::error_code ec;
        std::string want = name;
        for (char& c : want) c = (char)std::tolower((unsigned char)c);
        for (fs::directory_iterator it(P(join(join(exe_dir, "gameProfiles"), "default")), ec), end; !ec && it != end; it.increment(ec)) {
            std::string n = S(it->path().filename());
            for (char& c : n) c = (char)std::tolower((unsigned char)c);
            if (n == want) return S(it->path());
        }
        return std::string();
    };
    std::string dp = find_default(), up = join(join(user_data, "gameProfiles"), name);
    std::string d = dp.empty() ? "" : read_text(dp), u = file_exists(up) ? read_text(up) : std::string();
    if (d.empty() && u.empty()) return "";
    std::string cpu = hub::wiiu_profile_value(d, u, "CPU", "cpuMode"), mul = hub::wiiu_profile_value(d, u, "Graphics", "accurateShaderMul"),
                so = hub::wiiu_profile_value(d, u, "Graphics", "streamoutBufferCacheSize");
    std::string o;
    if (!cpu.empty()) o += "CPU mode " + cpu;
    if (!mul.empty()) o += std::string(o.empty() ? "" : ", ") + "accurate shader multiplication " + mul;
    if (!so.empty()) o += std::string(o.empty() ? "" : ", ") + "stream-out cache " + so;
    return o.empty() ? "" : o + (u.empty() ? " (Cemu's default game profile)" : " (your game profile)");
}

std::vector<int> cemu_toggle_gamepad_keys() { return {VK_CONTROL, VK_TAB}; }
bool cemu_toggle_combo(unsigned b) { return (b & XINPUT_GAMEPAD_BACK) && (b & XINPUT_GAMEPAD_RIGHT_SHOULDER); }

/* ------------------------------------------------------------------ */
/* launch arguments                                                    */
/* ------------------------------------------------------------------ */

static std::string quote(const std::string& s) { return winutil::quote_arg(s); }

std::string dolphin_args(const std::string& game, bool fullscreen, const std::string& gci, int volume) {
    /* -b: no main window, Esc / closing the render window ends the process. The GCI override pins the memory card to the
     * folder RetroHub syncs with the vault. The game path must be the last argument. */
    return std::string("-b -C Dolphin.Display.Fullscreen=") + (fullscreen ? "True" : "False") + " -C Dolphin.Core.SlotA=8" +
           (volume >= 0 ? " -C Dolphin.DSP.Volume=" + std::to_string(volume > 100 ? 100 : volume) : std::string()) + " -C " +
           quote("Dolphin.Core.GCIFolderAPathOverride=" + gci) + " -e " + quote(game);
}

std::string azahar_args(const std::string& game, bool fullscreen, const std::string& extra) {
    return std::string(fullscreen ? "-f " : "-w ") + (extra.empty() ? "" : extra + " ") + quote(game);   // the game path must be the last argument
}
std::string cemu_args(const std::string& game, bool fullscreen) {
    return "-g " + quote(game) + (fullscreen ? " -f" : "");
}

/* ------------------------------------------------------------------ */
/* saves                                                               */
/* ------------------------------------------------------------------ */

bool is_running(const std::string& exe) {
    std::wstring want = P(exe).filename().wstring();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, want.c_str()) == 0) found = true;
    CloseHandle(snap);
    return found;
}

uint64_t title_id_from_key(const std::string& key) {
    size_t c = key.find(':');
    if (c == std::string::npos || key.size() < c + 17) return 0;
    uint64_t v = 0;
    for (size_t i = c + 1; i < c + 17; ++i) {
        char h = key[i];
        int d = (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = (v << 4) | uint64_t(d);
    }
    return (key.size() == c + 17) ? v : 0;
}

static std::string stamp_tag() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[48];
    std::strftime(b, sizeof b, "local.%Y%m%d-%H%M%S", &tm);
    return b;
}

static hub::FsProbe probe() {
    hub::FsProbe p;
    p.dir_exists = dir_exists;
    p.file_exists = file_exists;
    p.read_text = read_text;
    return p;
}

uint64_t wiiu_title_id_of_file(const std::string& path) {
    std::string low = path;
    for (char& c : low) c = (char)std::tolower((unsigned char)c);
    if (low.size() < 4 || low.compare(low.size() - 4, 4, ".wua") != 0) return 0;
    std::error_code ec;
    uint64_t size = fs::file_size(P(path), ec);
    if (ec) return 0;
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return 0;
    auto rd = [&](uint64_t off, void* buf, size_t n) -> bool {
        f.clear();
        f.seekg((std::streamoff)off, std::ios::beg);
        f.read(static_cast<char*>(buf), (std::streamsize)n);
        return f.gcount() == (std::streamsize)n;
    };
    hub::WiiuInfo w;
    if (hub::parse_wua(rd, size, &w) && w.title_id) return w.title_id;
    hub::WiiuMeta m;
    return hub::wua_read_meta(rd, size, &m) ? m.title_id : 0;   // repacks with a 0000000000000000_v0 folder: the id is in meta.xml
}

hub::FsProbe wiiu_probe() { return probe(); }

void sync_vault_to_emulator(SaveSession& s) {
    std::string tag = stamp_tag();
    hub::SyncManifest man;
    hub::manifest_load(s.manifest_path, &man);
    int in = 0, kept = 0;
    /* The user's own install: the newest save wins like everywhere else, but a file of theirs is only replaced after a copy of it sits in a local
     * snapshot that RetroHub never prunes (%LOCALAPPDATA%\RetroHub\save-backups\<game>\<stamp>-before-<emulator>-launch). No snapshot, no overwrite. */
    std::string snap_dir;
    if (s.own_install) {
        const std::string la_s = winutil::getenv_utf8("LOCALAPPDATA"); const char* la = la_s.empty() ? nullptr : la_s.c_str();   // the same place nx_runner's default_save_backup_root() uses (not linked into every test that builds this file)
        const std::string root = la && *la ? join(join(la, "RetroHub"), "save-backups") : std::string();
        std::string label = S(P(s.battery).parent_path().filename());
        for (char& c : label) if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) c = '_';   // same folder naming as nx_runner's safe_name
        if (!root.empty() && !label.empty()) {
            std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &t);
            char b[48];
            std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
            const std::string base = join(join(root, label), std::string(b) + "-before-" + (s.snapshot_emu.empty() ? "emulator" : s.snapshot_emu) + "-launch");
            snap_dir = base;
            std::error_code ec;
            for (int n = 2; fs::exists(P(snap_dir), ec) && n < 1000; ++n) snap_dir = base + "-" + std::to_string(n);   // never touch an earlier snapshot
        }
    }
    for (size_t ri = 0; ri < s.rels.size(); ++ri) {
        const std::string& rel = s.rels[ri];
        const std::string& vrel = ri < s.vault_rels.size() ? s.vault_rels[ri] : rel;
        std::string vsrc = join(s.battery, vrel), edst = join(s.emu_root, rel);
        std::string bk = join(join(s.backup_dir, "emulator"), vrel);
        int keep = 10;
        if (s.own_install) {
            /* No snapshot, no overwrite: with nowhere to put the snapshot, a tree that already holds their files is left exactly as it is (an empty one can still be filled). */
            if (snap_dir.empty()) {
                if (hub::tree_has_files(edst)) {
                    if (s.error.empty()) s.error = "Couldn't make a backup of your Cemu save first, so the save from your account was not applied. Your own save is untouched.";
                    continue;
                }
            } else { bk = join(snap_dir, vrel); keep = 0; }
        }
        hub::TreeSyncReport r = hub::sync_tree_newest(vsrc, edst, bk, tag, &man, vrel + "/", keep);   // a file is only replaced after its copy was written to `bk`
        in += r.copied;
        kept += r.kept;
        if (!r.ok && s.error.empty()) s.error = s.own_install ? "Couldn't back up your Cemu save first, so some files from your account were not applied. Your own save is untouched. (" + r.error + ")" : r.error;
    }
    hub::manifest_save(s.manifest_path, man);
    s.summary = std::to_string(in) + " save file(s) restored from the vault, " + std::to_string(kept) + " kept (emulator copy newer)";
}

std::string emu_manifest_path(const std::string& battery_in, const char* suffix) {
    std::string battery = battery_in;
    while (!battery.empty() && (battery.back() == '\\' || battery.back() == '/')) battery.pop_back();
    fs::path game = P(battery).parent_path(), sysdir = game.parent_path(), vault = sysdir.parent_path();
    return S(vault / L".sync" / (L"emu-" + sysdir.filename().wstring() + L"-" + game.filename().wstring() + W(suffix ? suffix : "") + L".json"));
}

NxSaveMap nx_save_map(const std::string& install_dir, uint64_t tid, const std::string& battery, bool create_profile, bool force_account) {
    NxSaveMap m;
    if (!tid || install_dir.empty()) return m;
    std::string uuid;
    if (create_profile) {
        uuid = lichii_ensure_profile(install_dir);
        if (uuid.empty()) { m.error = "could not create the Switch user profile (profiles.dat)"; return m; }
    } else {   // read-only look at another install: its profile when it has one, else the fixed RetroHub user (the same on every install)
        const std::string pf = join(lichii_user_dir(install_dir), "nand\\system\\save\\8000000000000010\\su\\avators\\profiles.dat");
        uuid = file_exists(pf) ? lichii_ensure_profile(install_dir) : std::string(kLichiiUserUuid);
        if (uuid.empty()) uuid = kLichiiUserUuid;
    }
    m.user_root = lichii_user_dir(install_dir);
    /* The vault side holds the title id only (the user uuid can differ between PCs): <vault>/switch/<key>/battery/save/<TID> */
    char tb[24];
    std::snprintf(tb, sizeof tb, "%016llX", (unsigned long long)tid);
    m.rels = {lichii_save_rel(uuid, tid)};
    m.vault_rels = {std::string("save/") + tb};
    const std::string acct = std::string("nand/user/save/account/") + uuid + "/" + tb;   // Lichii prefers this folder when it exists
    if (force_account || dir_exists(join(m.user_root, acct)) || dir_exists(join(battery, std::string("save-account/") + tb))) {   /* either side has it: keep both PCs in step */ m.rels.push_back(acct); m.vault_rels.push_back(std::string("save-account/") + tb); }
    return m;
}

SaveSession begin_session(const hub::GameEntry& e, const Install& inst, const std::string& battery_in, const std::string&, uint64_t tid_override, const char* manifest_suffix) {
    SaveSession s;
    s.started_unix = (int64_t)std::time(nullptr);
    std::string battery = battery_in;
    while (!battery.empty() && (battery.back() == '\\' || battery.back() == '/')) battery.pop_back();
    s.battery = battery;
    /* <vault>\<system>\<game>\battery -> <vault>\_conflicts\<system>\<game>\battery */
    fs::path game = P(battery).parent_path(), sysdir = game.parent_path(), vault = sysdir.parent_path();
    s.backup_dir = S(vault / L"_conflicts" / sysdir.filename() / game.filename() / L"battery");
    s.manifest_path = emu_manifest_path(battery, manifest_suffix);   // (vault\.sync\emu-<system>-<game>[suffix].json)

    uint64_t tid = tid_override ? tid_override : title_id_from_key(e.key);
    if (e.system == hub::System::N3DS) {
        if (!tid || inst.dir.empty()) return s;   // homebrew (.3dsx) and unknown titles: nothing to map
        s.emu_root = hub::n3ds_sd_root(join(inst.dir, "user"));
        s.rels = {hub::n3ds_save_rel(tid), hub::n3ds_extdata_rel(tid)};
    } else if (e.system == hub::System::WiiU) {
        if (inst.dir.empty()) return s;
        hub::CemuDirs d;
        if (inst.managed) {
            d.user_data = join(inst.dir, "portable");
            d.mlc = join(d.user_data, "mlc01");
        } else {
            d = hub::cemu_dirs_for(inst.dir, env(L"APPDATA"), probe());
        }
        s.emu_root = d.mlc;
        s.own_install = !inst.managed;   // the user's own install: newest save wins, but their copy is snapshotted before it is replaced
        s.snapshot_emu = "cemu";
        s.wiiu_save_parent = join(d.mlc, "usr\\save\\00050000");
        if (!tid) {   // .wud/.wux: the title id is not readable without decrypting; reuse what an earlier session stored
            for (const char* hi : {"00050000", "0005000e"}) {
                auto known = hub::subdirs_modified_since(join(battery, std::string("usr\\save\\") + hi), 0);
                for (const auto& k : known) s.rels.push_back(std::string("usr/save/") + hi + "/" + k);   // every folder an earlier session stored
            }
            s.wiiu_unknown_title = true;
        } else {
            s.rels = hub::wiiu_save_rels(tid);   // the game's save and its update title's; every account folder sits inside
        }
    } else if (e.system == hub::System::PS2) {
        if (inst.dir.empty()) return s;
        s.emu_root = inst.dir;   // <install>/memcards/RetroHub/<game> <-> <vault>/ps2/<game>/battery/memcards/RetroHub/<game>
        s.rels = {pcsx2_card_dir_rel(e.key)};
        make_dirs(join(s.emu_root, s.rels[0]));
    } else if (e.system == hub::System::PSP) {
        if (inst.dir.empty()) return s;
        s.psp_disc_id = hub::psp::disc_id_from_key(e.key);
        if (s.psp_disc_id.empty()) return s;   // homebrew without a DISC_ID: its save folders cannot be told apart
        s.emu_root = join(inst.dir, "memstick\\PSP");   // <install>/memstick/PSP/SAVEDATA/<ID>... <-> <vault>/psp/<key>/battery/SAVEDATA/<ID>...
        std::vector<std::string> seen;
        for (const std::string& base : {join(s.emu_root, "SAVEDATA"), join(battery, "SAVEDATA")})
            for (const auto& f : hub::subdirs_modified_since(base, 0))
                if (hub::psp::savedata_folder_matches(f, s.psp_disc_id) && std::find(seen.begin(), seen.end(), f) == seen.end()) seen.push_back(f);
        for (const auto& f : seen) s.rels.push_back("SAVEDATA/" + f);
        make_dirs(join(s.emu_root, "SAVEDATA"));
    } else if (e.system == hub::System::Switch) {
        if (!tid || inst.dir.empty()) return s;   // no title id (homebrew, unknown): nothing to map
        NxSaveMap nm = nx_save_map(inst.dir, tid, battery, true);   // Lichii and Fritchii alike (both map the same layout under their own user\)
        if (!nm.error.empty()) { s.error = nm.error + " - saves were not copied"; return s; }
        s.emu_root = nm.user_root;
        s.rels = nm.rels;
        s.vault_rels = nm.vault_rels;
    } else if (e.system == hub::System::GC) {
        if (inst.dir.empty()) return s;
        s.emu_root = dolphin_card_root(inst.dir, e.key);   // <install>\User\GC\RetroHub\<game>\GCI  <->  <vault>\gc\<game>\battery\GCI
        s.rels = {"GCI"};
        make_dirs(join(s.emu_root, "GCI"));
    } else {
        return s;
    }
    s.active = true;
    sync_vault_to_emulator(s);
    return s;
}

void end_session(SaveSession& s) {
    if (!s.active) return;
    if (s.wiiu_unknown_title) {   // .wud/.wux: every save folder that changed during the session belongs to this game
        for (const char* hi : {"00050000", "0005000e"}) {
            std::string parent = join(S(P(s.wiiu_save_parent).parent_path()), hi);
            for (const auto& c : hub::subdirs_modified_since(parent, s.started_unix - 2)) {
                std::string rel = std::string("usr/save/") + hi + "/" + c;
                if (std::find(s.rels.begin(), s.rels.end(), rel) == s.rels.end()) s.rels.push_back(rel);
            }
        }
    }
    if (!s.psp_disc_id.empty()) {   // PSP: a save folder the game created during the session
        for (const auto& c : hub::subdirs_modified_since(join(s.emu_root, "SAVEDATA"), 0)) {
            std::string rel = "SAVEDATA/" + c;
            if (hub::psp::savedata_folder_matches(c, s.psp_disc_id) && std::find(s.rels.begin(), s.rels.end(), rel) == s.rels.end()) s.rels.push_back(rel);
        }
    }
    std::string tag = stamp_tag();
    hub::SyncManifest man;
    hub::manifest_load(s.manifest_path, &man);
    int out = 0, bk = 0;
    for (size_t ri = 0; ri < s.rels.size(); ++ri) {
        const std::string& rel = s.rels[ri];
        const std::string& vrel = ri < s.vault_rels.size() ? s.vault_rels[ri] : rel;
        hub::TreeSyncReport r = hub::sync_tree_newest(join(s.emu_root, rel), join(s.battery, vrel), join(s.backup_dir, vrel), tag, &man, vrel + "/");
        out += r.copied;
        bk += r.backed_up;
        if (!r.ok && s.error.empty()) s.error = r.error;
    }
    hub::manifest_save(s.manifest_path, man);
    s.summary = std::to_string(out) + " save file(s) copied into the vault, " + std::to_string(bk) + " older vault file(s) kept in _conflicts" +
                (s.wiiu_unknown_title && s.rels.empty() ? "; no Wii U save folder changed during the session" : "");
    s.active = false;
}

} // namespace hubemu
