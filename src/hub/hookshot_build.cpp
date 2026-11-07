#include "hookshot_build.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "../common/games.h"
#include "../common/library.h"
#include "../common/winutil.h"
#include "emu_http.h"
#include "hookshot.h"
#include "miniz.h"

namespace fs = std::filesystem;

namespace hookshot {

/* ======================================================================================================================== */
/* small helpers                                                                                                            */
/* ======================================================================================================================== */

static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::string S(const fs::path& p) { return p.u8string(); }
static std::wstring W(const std::string& s) { return winutil::to_wide(s); }
static std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
static bool starts_with(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }
static bool pexists(const fs::path& p) { std::error_code ec; return fs::exists(p, ec); }
static bool is_dir(const fs::path& p) { std::error_code ec; return fs::is_directory(p, ec); }
static bool is_file(const fs::path& p) { std::error_code ec; return fs::is_regular_file(p, ec); }
static bool dir_empty(const fs::path& p) {   // false when the folder cannot be listed: "empty" is never assumed (saves live in these folders)
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return false;
    fs::directory_iterator it(p, ec);
    return !ec && it == fs::directory_iterator();
}
static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}
static std::string data_dir_clean() {
    std::string d = hub::data_dir();
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    return d;
}
static std::string emu_root() { return S(P(data_dir_clean()) / "emulators"); }
static std::string staging_dir() { return S(P(emu_root()) / "hookshot.staging"); }
static std::string old_dir() { return S(P(emu_root()) / "hookshot.old"); }
std::string kit_dir() { return S(P(emu_root()) / "hookshot-kit"); }
bool kit_present() { return is_dir(P(kit_dir())); }

/* ======================================================================================================================== */
/* hashes                                                                                                                   */
/* ======================================================================================================================== */

std::string to_hex(const std::vector<uint8_t>& d) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) { s += h[b >> 4]; s += h[b & 15]; }
    return s;
}

std::string to_base64(const std::vector<uint8_t>& d) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    size_t i = 0;
    for (; i + 2 < d.size(); i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8 | d[i + 2];
        s += t[v >> 18]; s += t[(v >> 12) & 63]; s += t[(v >> 6) & 63]; s += t[v & 63];
    }
    if (i + 1 == d.size()) {
        uint32_t v = (uint32_t)d[i] << 16;
        s += t[v >> 18]; s += t[(v >> 12) & 63]; s += "==";
    } else if (i + 2 == d.size()) {
        uint32_t v = (uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8;
        s += t[v >> 18]; s += t[(v >> 12) & 63]; s += t[(v >> 6) & 63]; s += '=';
    }
    return s;
}

namespace {
struct Hasher {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    ULONG len = 0;
    bool ok = false;
    explicit Hasher(HashAlg a) {
        if (BCryptOpenAlgorithmProvider(&alg, a == HashAlg::Sha256 ? BCRYPT_SHA256_ALGORITHM : BCRYPT_SHA512_ALGORITHM, nullptr, 0) != 0) return;
        len = a == HashAlg::Sha256 ? 32 : 64;
        ok = BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    }
    ~Hasher() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    bool add(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        while (n > 0) {
            ULONG k = (ULONG)std::min<size_t>(n, 1u << 24);
            if (BCryptHashData(h, (PUCHAR)b, k, 0) != 0) return false;
            b += k;
            n -= k;
        }
        return true;
    }
    bool finish(std::vector<uint8_t>* out) {
        out->assign(len, 0);
        return BCryptFinishHash(h, out->data(), len, 0) == 0;
    }
};
}   // namespace

bool hash_data(HashAlg a, const void* data, size_t n, std::vector<uint8_t>* out) {
    Hasher h(a);
    return h.ok && h.add(data, n) && h.finish(out);
}

bool hash_file(HashAlg a, const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return false;
    Hasher h(a);
    if (!h.ok) return false;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize n = f.gcount();
        if (n > 0 && !h.add(buf.data(), (size_t)n)) return false;
    }
    if (f.bad()) return false;
    return h.finish(out);
}

std::string hash_data_hex(HashAlg a, const std::string& bytes) {
    std::vector<uint8_t> d;
    return hash_data(a, bytes.data(), bytes.size(), &d) ? to_hex(d) : std::string();
}

std::string hash_file_hex(HashAlg a, const std::string& path) {
    std::vector<uint8_t> d;
    return hash_file(a, path, &d) ? to_hex(d) : std::string();
}

/* ======================================================================================================================== */
/* pure helpers                                                                                                             */
/* ======================================================================================================================== */

bool parse_protocol_line(const std::string& line_in, BuildStatus* st) {
    std::string line = line_in;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.compare(0, 2, "@@") != 0) return false;
    size_t sp = line.find(' ');
    const std::string kind = line.substr(2, sp == std::string::npos ? std::string::npos : sp - 2);
    const std::string rest = sp == std::string::npos ? std::string() : trim(line.substr(sp + 1));
    if (kind == "stage") {
        if (rest.empty()) return false;
        size_t s2 = rest.find(' ');
        if (st) {
            st->stage_id = rest.substr(0, s2);
            st->stage = s2 == std::string::npos ? st->stage_id : trim(rest.substr(s2 + 1));
            st->progress = 0.f;
            st->state = BuildState::Building;
        }
        return true;
    }
    if (kind == "progress") {
        if (rest.empty()) return false;
        char* end = nullptr;
        double v = std::strtod(rest.c_str(), &end);
        if (end == rest.c_str() || *end != '\0' || v != v) return false;
        if (st) st->progress = (float)std::min(1.0, std::max(0.0, v));
        return true;
    }
    if (kind == "check") {
        size_t s2 = rest.find(' ');
        const std::string verdict = rest.substr(0, s2);
        const std::string text = s2 == std::string::npos ? std::string() : trim(rest.substr(s2 + 1));
        if (verdict != "ok" && verdict != "bad") return false;
        if (st) {
            st->stage_id = "check";
            if (verdict == "ok") { st->stage = text; st->error.clear(); }
            else st->error = text.empty() ? std::string("This game file can't be used.") : text;
        }
        return true;
    }
    if (kind == "done") {
        if (st) { st->stage_id = "done"; st->stage = rest; st->progress = 1.f; }
        return true;
    }
    if (kind == "error") {
        if (st) { st->error = rest.empty() ? std::string("The build stopped.") : rest; st->state = BuildState::Failed; }
        return true;
    }
    return false;
}

int jobs_for(uint64_t ram_bytes, int cores) {
    if (cores < 1) cores = 1;
    if (ram_bytes == 0) return cores;
    const double gb = (double)ram_bytes / (1024.0 * 1024.0 * 1024.0);
    const int by_ram = (int)(gb * 0.8 / 3.5);
    return std::max(1, std::min(cores, by_ram));
}

const std::vector<Language>& languages() {
    static const std::vector<Language> v = {
        {"en-US", "English (US)"}, {"en-GB", "English (UK)"}, {"fr", "French (France)"}, {"fr-CA", "French (Canada)"}, {"de", "German"},
        {"it", "Italian"}, {"es", "Spanish (Spain)"}, {"es-419", "Spanish (Latin America)"}, {"pt-BR", "Portuguese (Brazil)"}, {"nl", "Dutch"},
        {"pl", "Polish"}, {"ru", "Russian"}, {"ja", "Japanese"}, {"ko", "Korean"}, {"zh-Hans", "Chinese (Simplified)"},
        {"zh-Hant", "Chinese (Traditional)"}};
    return v;
}

bool language_valid(const std::string& code) {
    for (const Language& l : languages()) if (code == l.code) return true;
    return false;
}

GameSource resolve_game_source(const std::string& entry_path) {
    GameSource g;
    if (entry_path.empty()) {
        g.problem = "RetroHub doesn't know where this game's file is.";
        g.fix = "Add the game's folder to RetroHub's library again.";
        return g;
    }
    const fs::path p = P(entry_path);
    const std::string ext = lower(S(p.extension()));
    auto dump = [&](const fs::path& exefs, const fs::path& romfs) {
        if (!is_file(exefs / "main")) {
            g.problem = "This game's folder has no exefs\\main file.";
            g.fix = "Use a full dump of the game: a folder with exefs and romfs in it.";
            return;
        }
        if (!is_dir(romfs)) {
            g.problem = "This game's folder has no romfs folder next to exefs.";
            g.fix = "Use a full dump of the game: a folder with exefs and romfs in it.";
            return;
        }
        g.kind = GameSource::Dump;
        g.exefs = S(exefs);
        g.romfs = S(romfs);
    };
    if (ext == ".nup") {
        if (!is_file(p)) {
            g.problem = "The game file can't be found. Is the drive it is on connected?";
            g.fix = "Connect the drive, then check again.";
            return g;
        }
        g.kind = GameSource::Nup;
        g.nup = S(p);
        return g;
    }
    if (p.filename() == "main" && lower(S(p.parent_path().filename())) == "exefs") {
        dump(p.parent_path(), p.parent_path().parent_path() / "romfs");
        return g;
    }
    if (is_dir(p) && is_dir(p / "exefs")) {
        dump(p / "exefs", p / "romfs");
        return g;
    }
    g.problem = ext.empty() ? std::string("RetroHub can only build the native version from a .nup file or a full game dump folder.")
                            : "RetroHub can only build the native version from a .nup file or a full game dump folder, not from a " + ext + " file.";
    g.fix = "Add the game as a .nup file, or as a folder with exefs and romfs in it.";
    return g;
}

std::vector<std::string> game_source_args(const GameSource& g) {
    if (g.kind == GameSource::Nup) return {"--nup", g.nup};
    if (g.kind == GameSource::Dump) return {"--romfs", g.romfs, "--exefs", g.exefs};
    return {};
}

std::vector<Problem> requirement_problems(const Requirements& r) {
    std::vector<Problem> v;
    if (!r.game_ok) {
        Problem p;
        p.id = "game";
        p.text = r.game_problem.empty() ? std::string("The game file can't be used.") : r.game_problem;
        p.hint = r.game_fix;
        v.push_back(p);
    }
    if (!r.vs_tools) {
        Problem p;
        p.id = "vs";
        p.text = "Visual Studio's C++ tools are not installed.";
        p.hint = "Install Visual Studio Build Tools (free, from Microsoft) with 'Desktop development with C++'";
        p.url = "https://visualstudio.microsoft.com/downloads/";
        p.button = "Get the build tools";
        v.push_back(p);
    } else if (!r.win_sdk) {
        Problem p;
        p.id = "sdk";
        p.text = "The Windows SDK is not installed.";
        p.hint = "Open the Visual Studio Installer, choose Modify, and tick a Windows SDK (it comes with 'Desktop development with C++')";
        v.push_back(p);
    }
    if (!r.vulkan_mesh) {
        Problem p;
        p.id = "vulkan";
        p.text = r.gpu_name.empty() ? std::string("No graphics card with Vulkan 1.3 and mesh shaders was found.")
                                    : r.gpu_name + " doesn't support Vulkan 1.3 with mesh shaders.";
        p.hint = "Update your graphics driver. Only NVIDIA graphics cards have been tested";
        v.push_back(p);
    }
    if (r.ram_gb < kNeedRamGb) {
        Problem p;
        p.id = "ram";
        p.text = "This PC has " + std::to_string(r.ram_gb) + " GB of memory. The build needs " + std::to_string(kNeedRamGb) + " GB.";
        p.hint = "Build it on a PC with more memory";
        v.push_back(p);
    }
    if (r.free_gb < kNeedFreeGb) {
        Problem p;
        p.id = "disk";
        p.text = "Only " + std::to_string(r.free_gb) + " GB is free on the drive RetroHub keeps its data on. The build needs " +
                 std::to_string(kNeedFreeGb) + " GB while it works.";
        p.hint = "Free some space on that drive, then check again (about 25 GB stays used afterwards)";
        v.push_back(p);
    }
    return v;
}

void fill_requirement_problem(Requirements* r) {
    r->vs_cpp = r->vs_tools && r->win_sdk;
    std::vector<Problem> p = requirement_problems(*r);
    r->problem = p.empty() ? std::string() : p[0].text;
    r->fix_hint = p.empty() ? std::string() : p[0].hint;
}

float overall_for(const std::string& id, float p) {
    struct Seg { const char* id; float a, b; };
    static const Seg segs[] = {{"kit-src", 0.f, 0.01f}, {"kit-python", 0.01f, 0.04f}, {"kit-llvm", 0.04f, 0.12f}, {"kit-cpm", 0.12f, 0.13f},
                               {"check", 0.13f, 0.14f},  {"read", 0.14f, 0.17f},      {"lift", 0.17f, 0.40f},     {"compile", 0.40f, 0.97f},
                               {"finish", 0.97f, 0.99f}, {"swap", 0.99f, 1.f}};
    p = std::min(1.f, std::max(0.f, p));
    for (const Seg& s : segs) if (id == s.id) return s.a + (s.b - s.a) * p;
    if (id == "done") return 1.f;
    return -1.f;
}

/* ======================================================================================================================== */
/* the pinned downloads                                                                                                     */
/* ======================================================================================================================== */

const std::vector<Pin>& pins() {
    static const std::vector<Pin> v = {
        {"python", "api.nuget.org", "/v3-flatcontainer/python/3.13.17/python.3.13.17.nupkg", HashAlg::Sha512,
         "DmZ5KLtPGVc8bNiPZRM7LgeyQ6XHFvM1tWqma7nsb4IyglbSLDop4joCdqxcam14YhH+q1JIkK0eVmKRlsaynA==", true, 14826465, "python.3.13.17.nupkg",
         "Python 3.13.17"},
        {"llvm", "github.com", "/llvm/llvm-project/releases/download/llvmorg-23.1.5/clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.zst",
         HashAlg::Sha256, "b2be33385ddc86e83511417a060de927ae8b09d1f3017cc3cdce5c73cd5029d7", false, 276368249,
         "clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.zst", "LLVM 23.1.5"},
        {"cpm-vulkanheaders", "github.com", "/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.345.tar.gz", HashAlg::Sha512,
         "d2846ea228415772645eea4b52a9efd33e6a563043dd3de059e798be6391a8f0ca089f455ae420ff22574939ed0f48ed7c6ff3d5a9987d5231dbf3b3f89b484b", false,
         3029194, "Vulkan-Headers-v1.4.345.tar.gz", "Vulkan-Headers v1.4.345"},
        {"cpm-spirv-headers", "github.com", "/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.341.0.tar.gz", HashAlg::Sha512,
         "d624371dd455c66a300344c89812598ffe11b5eedba555779f789e85c29dc67317741858c60e0744a1e6755cc0d2759b8659f0674f4cc31479c4cb6fc25ed23b", false,
         561287, "SPIRV-Headers-vulkan-sdk-1.4.341.0.tar.gz", "SPIRV-Headers vulkan-sdk-1.4.341.0"},
        {"cpm-vulkanmemoryallocator", "github.com", "/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz", HashAlg::Sha512,
         "deb5902ef8db0e329fbd5f3f4385eb0e26bdd9f14f3a2334823fb3fe18f36bc5d235d620d6e5f6fe3551ec3ea7038638899db8778c09f6d5c278f5ff95c3344b", false,
         962176, "VulkanMemoryAllocator-v3.3.0.tar.gz", "VulkanMemoryAllocator v3.3.0"},
        {"cpm-ffmpeg", "github.com", "/crueter-ci/FFmpeg/releases/download/9.0.2-1790726865-946fcce07b/ffmpeg-windows-amd64-9.0.2-1790726865-946fcce07b.tar.zst",
         HashAlg::Sha256, "12d23634e609137e134cfd24148c9e27ed8a4104010f010b4cd92b68b1000367", false, 7830170,
         "ffmpeg-windows-amd64-9.0.2-1790726865-946fcce07b.tar.zst", "FFmpeg 9.0.2 (Eden's CI build)"},
    };
    return v;
}

static const Pin& pin(const char* id) {
    for (const Pin& p : pins()) if (std::strcmp(p.id, id) == 0) return p;
    return pins()[0];
}

static bool pin_hash_ok(const Pin& p, const std::string& path) {
    std::vector<uint8_t> d;
    if (!hash_file(p.alg, path, &d)) return false;
    return (p.b64 ? to_base64(d) : to_hex(d)) == p.hash;
}

/* ======================================================================================================================== */
/* the swap                                                                                                                 */
/* ======================================================================================================================== */

static bool is_cache_name(const std::string& n_in) {
    const std::string n = lower(n_in);
    if (n == "mkw_vk_pipelines.bin" || n == "hookshot_options.ini") return true;
    return starts_with(n, "hookshot_") && n.size() > 13 && n.compare(n.size() - 4, 4, ".bin") == 0;
}

bool swap_in(const std::string& staging_s, const std::string& game_s, const std::string& old_s, std::string* err) {
    const fs::path staging = P(staging_s), game = P(game_s), old = P(old_s);
    std::error_code ec;
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    if (!is_dir(staging)) return fail("The new build is missing, so nothing was changed.");
    if (!pexists(game)) {   // first build: nothing to keep
        if (pexists(old) && !dir_empty(old / "user")) return fail("An earlier backup folder holds saves (" + old_s + "). It was left alone and nothing was changed.");
        fs::rename(staging, game, ec);
        return ec ? fail("Could not put the new build in place: " + ec.message()) : true;
    }
    std::vector<std::pair<fs::path, fs::path>> moved;   // from, to
    auto rollback = [&] {
        for (size_t i = moved.size(); i-- > 0;) { std::error_code e2; fs::rename(moved[i].second, moved[i].first, e2); }
        moved.clear();
    };
    const bool had_user = is_dir(game / "user");
    if (had_user) {
        if (pexists(staging / "user")) {
            if (dir_empty(staging / "user")) fs::remove(staging / "user", ec);
            else return fail("The new build has a user folder of its own, so nothing was changed.");
        }
        fs::rename(game / "user", staging / "user", ec);
        if (ec) return fail("Could not move your saves into the new build (" + ec.message() + "). Both folders were kept.");
        moved.push_back({game / "user", staging / "user"});
    }
    for (fs::directory_iterator it(game, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const std::string name = S(it->path().filename());
        if (!is_cache_name(name) || pexists(staging / it->path().filename())) continue;   // the new build's own file wins
        fs::rename(it->path(), staging / it->path().filename(), e2);
        if (e2) { rollback(); return fail("Could not move " + name + " into the new build (" + e2.message() + "). Both folders were kept."); }
        moved.push_back({it->path(), staging / it->path().filename()});
    }
    ec.clear();
    if (pexists(old)) {
        if (is_dir(old / "user") && !dir_empty(old / "user")) { rollback(); return fail("An earlier backup folder holds saves (" + old_s + "). It was left alone and nothing was changed."); }
        fs::remove_all(old, ec);
        if (ec || pexists(old)) { rollback(); return fail("Could not clear the old backup folder (" + ec.message() + ")."); }
    }
    fs::rename(game, old, ec);
    if (ec) { rollback(); return fail("Could not set the old build aside (" + ec.message() + "). Both folders were kept."); }
    fs::rename(staging, game, ec);
    if (ec) {
        std::error_code e2;
        fs::rename(old, game, e2);   // the old build comes back; the moved files go back into it
        rollback();
        return fail("Could not put the new build in place (" + ec.message() + "). The old build was kept.");
    }
    if (had_user && !is_dir(game / "user")) return fail("Your saves folder is missing from the new build. The old build was kept at " + old_s + ".");
    fs::remove_all(old, ec);   // best effort: a leftover is cleared by the next swap
    return true;
}

std::string recover_swap(const std::string& staging_s, const std::string& game_s, const std::string& old_s) {
    const fs::path staging = P(staging_s), game = P(game_s), old = P(old_s);
    std::error_code ec;
    std::string did;
    if (!pexists(game) && is_dir(old)) {
        fs::rename(old, game, ec);
        if (!ec) did += "put the previous build back. ";
    }
    if (is_dir(staging / "user") && !dir_empty(staging / "user")) {
        const fs::path target = pexists(game) ? game : pexists(old) ? old : fs::path();
        if (!target.empty() && !pexists(target / "user")) {
            ec.clear();
            fs::rename(staging / "user", target / "user", ec);
            if (!ec) did += "put the saves back. ";
        }
    }
    return trim(did);
}

/* ======================================================================================================================== */
/* running programs                                                                                                         */
/* ======================================================================================================================== */

namespace {

std::mutex g_spawn_mu;
std::atomic<HANDLE> g_job{nullptr};   // the build's job object, while it runs (the game monitor leaves its processes alone)

struct EnvEdit {
    std::vector<std::pair<std::wstring, std::wstring>> set;
    std::vector<std::wstring> drop_prefix;   // upper-case prefixes of variable names to remove
};

struct ICmp {
    bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
};

std::wstring make_env_block(const EnvEdit& ed) {
    std::map<std::wstring, std::wstring, ICmp> env;
    if (wchar_t* blk = GetEnvironmentStringsW()) {
        for (const wchar_t* p = blk; *p; p += wcslen(p) + 1) {
            const wchar_t* eq = wcschr(p + 1, L'=');   // p+1: names like "=C:" start with '='
            if (!eq) continue;
            env[std::wstring(p, eq)] = eq + 1;
        }
        FreeEnvironmentStringsW(blk);
    }
    for (auto it = env.begin(); it != env.end();) {
        std::wstring up = it->first;
        for (wchar_t& c : up) c = (wchar_t)towupper(c);
        bool drop = false;
        for (const std::wstring& pre : ed.drop_prefix) if (up.compare(0, pre.size(), pre) == 0) drop = true;
        it = drop ? env.erase(it) : std::next(it);
    }
    for (const auto& kv : ed.set) env[kv.first] = kv.second;
    std::wstring blk;
    for (const auto& kv : env) { blk += kv.first; blk += L'='; blk += kv.second; blk += L'\0'; }
    blk += L'\0';
    return blk;
}

struct Spawned {
    HANDLE proc = nullptr, thread = nullptr, rd = nullptr;
    DWORD pid = 0;
    void close() {
        if (thread) CloseHandle(thread);
        if (proc) CloseHandle(proc);
        if (rd) CloseHandle(rd);
        thread = proc = rd = nullptr;
    }
};

/* Starts `cmdline` with stdout + stderr in a pipe (read end in out->rd), no window, no console input. `flags` are added to the
 * creation flags (CREATE_SUSPENDED, BELOW_NORMAL_PRIORITY_CLASS). */
bool spawn_piped(const std::wstring& cmdline, const std::wstring& cwd, const EnvEdit& env, DWORD flags, Spawned* out, std::string* err) {
    std::lock_guard<std::mutex> lk(g_spawn_mu);   // so no other child of ours inherits this pipe's write end
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { if (err) *err = "could not create a pipe"; return false; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul == INVALID_HANDLE_VALUE ? nullptr : nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    std::wstring cmd = cmdline;
    std::wstring blk = make_env_block(env);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, flags | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, (LPVOID)blk.data(),
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    const DWORD le = GetLastError();
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        if (err) *err = "could not start " + winutil::to_utf8(cmdline.substr(0, cmdline.find(L' '))) + " (error " + std::to_string(le) + ")";
        return false;
    }
    out->proc = pi.hProcess;
    out->thread = pi.hThread;
    out->rd = rd;
    out->pid = pi.dwProcessId;
    return true;
}

std::wstring make_cmdline(const std::string& exe, const std::vector<std::string>& args) {
    std::wstring c = winutil::quote_arg(W(exe));
    for (const std::string& a : args) { c += L' '; c += winutil::quote_arg(W(a)); }
    return c;
}

/* Output of a short command (vswhere): all of it, or false after timeout_ms. */
bool capture_command(const std::string& exe, const std::vector<std::string>& args, DWORD timeout_ms, std::string* out) {
    Spawned sp;
    std::string err;
    if (!spawn_piped(make_cmdline(exe, args), L"", EnvEdit(), 0, &sp, &err)) return false;
    const DWORD t0 = GetTickCount();
    bool done = false;
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(sp.rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            DWORD n = 0;
            if (ReadFile(sp.rd, buf, (DWORD)std::min<size_t>(sizeof buf, avail), &n, nullptr) && n) out->append(buf, n);
            continue;
        }
        if (WaitForSingleObject(sp.proc, 50) == WAIT_OBJECT_0) {
            if (PeekNamedPipe(sp.rd, nullptr, 0, nullptr, &avail, nullptr) && avail) continue;
            done = true;
            break;
        }
        if (GetTickCount() - t0 > timeout_ms) { TerminateProcess(sp.proc, 1); break; }
    }
    sp.close();
    return done;
}

/* ---- pausing: every process of the job is suspended while a game runs ---- */
typedef LONG(NTAPI* NtProcFn)(HANDLE);

NtProcFn nt_fn(const char* name) {
    static HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt ? (NtProcFn)(void*)GetProcAddress(nt, name) : nullptr;
}

void job_pids(HANDLE job, std::vector<DWORD>* out) {
    std::vector<char> buf(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 4096 * sizeof(ULONG_PTR));
    auto* l = (JOBOBJECT_BASIC_PROCESS_ID_LIST*)buf.data();
    if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList, l, (DWORD)buf.size(), nullptr)) return;
    for (DWORD i = 0; i < l->NumberOfProcessIdsInList; ++i) out->push_back((DWORD)l->ProcessIdList[i]);
}

struct JobPauser {
    HANDLE job = nullptr;
    std::set<DWORD> suspended;
    bool active = false;
    void apply(bool want) {
        NtProcFn susp = nt_fn("NtSuspendProcess"), res = nt_fn("NtResumeProcess");
        if (!susp || !res) return;
        if (want) {
            // a few passes: a child that was just started is caught on the next one
            for (int pass = 0; pass < 3; ++pass) {
                std::vector<DWORD> pids;
                job_pids(job, &pids);
                bool any = false;
                for (DWORD pid : pids) {
                    if (suspended.count(pid)) continue;
                    if (HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid)) {
                        if (susp(h) == 0) { suspended.insert(pid); any = true; }
                        CloseHandle(h);
                    }
                }
                if (!any) break;
            }
            active = true;
        } else {
            for (DWORD pid : suspended)
                if (HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid)) { res(h); CloseHandle(h); }
            suspended.clear();
            active = false;
        }
    }
};

struct RunCtl {
    const std::atomic<bool>* cancel = nullptr;
    const std::atomic<bool>* pause = nullptr;
};

/* Runs a program inside a kill-on-close job at below-normal priority, without a window. Every output line goes to on_line (and the
 * caller's log). Returns the exit code, -1 when it could not be started (*err says why), -2 when cancelled. */
int run_in_job(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd, const EnvEdit& env, const RunCtl& ctl,
               const std::function<void(const std::string&)>& on_line, std::string* err, bool publish_job) {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) { if (err) *err = "could not create a job object"; return -1; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    Spawned sp;
    if (!spawn_piped(make_cmdline(exe, args), W(cwd), env, CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, &sp, err)) { CloseHandle(job); return -1; }
    if (!AssignProcessToJobObject(job, sp.proc)) {
        if (err) *err = "could not put the build in its job object (error " + std::to_string(GetLastError()) + ")";
        TerminateProcess(sp.proc, 1);
        sp.close();
        CloseHandle(job);
        return -1;
    }
    if (publish_job) g_job = job;
    ResumeThread(sp.thread);
    JobPauser pauser;
    pauser.job = job;
    std::string pending;
    char buf[65536];
    int result = 0;
    auto flush_lines = [&](bool final_) {
        size_t a = 0, nl;
        while ((nl = pending.find('\n', a)) != std::string::npos) {
            std::string line = pending.substr(a, nl - a);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (on_line) on_line(line);
            a = nl + 1;
        }
        pending.erase(0, a);
        if (final_ && !pending.empty()) { if (on_line) on_line(pending); pending.clear(); }
    };
    for (;;) {
        const bool want_pause = ctl.pause && ctl.pause->load();
        if (want_pause != pauser.active) pauser.apply(want_pause);
        if (ctl.cancel && ctl.cancel->load()) {
            TerminateJobObject(job, 1);
            WaitForSingleObject(sp.proc, 5000);
            result = -2;
            break;
        }
        DWORD avail = 0;
        if (PeekNamedPipe(sp.rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            DWORD n = 0;
            if (ReadFile(sp.rd, buf, (DWORD)std::min<size_t>(sizeof buf, avail), &n, nullptr) && n) { pending.append(buf, n); flush_lines(false); }
            continue;
        }
        if (WaitForSingleObject(sp.proc, 100) == WAIT_OBJECT_0) {
            if (PeekNamedPipe(sp.rd, nullptr, 0, nullptr, &avail, nullptr) && avail) continue;   // drain what is left
            DWORD code = 1;
            GetExitCodeProcess(sp.proc, &code);
            result = (int)code;
            break;
        }
    }
    flush_lines(true);
    if (pauser.active) pauser.apply(false);
    TerminateJobObject(job, 1);   // nothing the build started outlives it
    if (publish_job) g_job = nullptr;
    sp.close();
    CloseHandle(job);
    return result;
}

}   // namespace

int run_command(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd, const std::atomic<bool>* cancel,
                const std::atomic<bool>* pause, const std::function<void(const std::string&)>& on_line, std::string* err) {
    RunCtl ctl{cancel, pause};
    return run_in_job(exe, args, cwd, EnvEdit(), ctl, on_line, err, false);
}

/* ======================================================================================================================== */
/* the game monitor: is a game running?                                                                                     */
/* ======================================================================================================================== */

namespace {

struct G {
    std::mutex mu;
    BuildStatus st;
    std::thread th;
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
    std::atomic<bool> pause{false};
    std::function<bool()> probe;
    // requirements
    Requirements req;
    std::atomic<bool> req_busy{false};
    // kit
    std::atomic<int64_t> kit_size{-1};
    std::atomic<bool> kit_busy{false};
    std::mutex log_mu;
    std::ofstream log;
};

G& g() {
    static G* inst = new G;   // never destroyed: workers may outlive static destruction
    return *inst;
}

bool game_process_name(const std::wstring& exe_w) {
    std::string n = lower(winutil::to_utf8(exe_w));
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".exe") == 0) n.resize(n.size() - 4);
    static const char* prefixes[] = {"lichii", "dolphin", "cemu", "azahar", "pcsx2", "ppsspp"};
    static const char* exact[] = {"retrorun", "n64run", "battleship", "hookshot", "streamrun"};
    for (const char* p : prefixes) if (starts_with(n, p)) return true;
    for (const char* p : exact) if (n == p) return true;
    return false;
}

bool known_game_running() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    const HANDLE job = g_job.load();
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        if (!game_process_name(pe.szExeFile)) continue;
        if (job) {   // never one of the build's own processes
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                BOOL in = FALSE;
                IsProcessInJob(h, job, &in);
                CloseHandle(h);
                if (in) continue;
            }
        }
        found = true;
    }
    CloseHandle(snap);
    return found;
}

bool game_running_now() {
    std::function<bool()> probe;
    { std::lock_guard<std::mutex> lk(g().mu); probe = g().probe; }
    if (probe && probe()) return true;
    return known_game_running();
}

void log_line(const std::string& s) {
    std::lock_guard<std::mutex> lk(g().log_mu);
    if (g().log.is_open()) { g().log << s << "\n"; g().log.flush(); }
}

}   // namespace

void set_game_running_probe(std::function<bool()> probe) {
    std::lock_guard<std::mutex> lk(g().mu);
    g().probe = std::move(probe);
}

/* ======================================================================================================================== */
/* unzip (miniz)                                                                                                            */
/* ======================================================================================================================== */

static bool safe_zip_name(const std::string& n) {
    if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find(':') != std::string::npos) return false;
    size_t p = 0;
    while (p <= n.size()) {
        size_t q = n.find_first_of("/\\", p);
        if (n.substr(p, q == std::string::npos ? std::string::npos : q - p) == "..") return false;
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return true;
}

/* Unpacks the zip's entries below `prefix` ("" = all; "tools/") into dest, without the prefix. Every name is checked. */
static bool unzip_prefix(const std::string& zip, const std::string& dest, const std::string& prefix, const std::atomic<bool>* cancel,
                         const std::function<void(float)>& progress, std::string* err) {
    mz_zip_archive za;
    std::memset(&za, 0, sizeof za);
    FILE* zf = _wfopen(W(zip).c_str(), L"rb");
    if (!zf) { *err = "cannot open " + S(P(zip).filename()); return false; }
    if (!mz_zip_reader_init_cfile(&za, zf, 0, 0)) { std::fclose(zf); *err = S(P(zip).filename()) + " is not a valid zip"; return false; }
    bool ok = true;
    const mz_uint n = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < n && ok; i++) {
        if (cancel && cancel->load()) { ok = false; *err = "cancelled"; break; }
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st)) { ok = false; *err = "damaged zip entry"; break; }
        std::string name = st.m_filename;
        if (!safe_zip_name(name)) { ok = false; *err = "unsafe path in the zip: " + name; break; }
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        name.erase(0, prefix.size());
        if (name.empty()) continue;
        const fs::path target = P(dest) / fs::u8path(name);
        std::error_code ec;
        if (st.m_is_directory || name.back() == '/') { fs::create_directories(target, ec); continue; }
        fs::create_directories(target.parent_path(), ec);
        FILE* out = _wfopen(target.wstring().c_str(), L"wb");
        if (!out) { ok = false; *err = "cannot write " + name; break; }
        const bool r = mz_zip_reader_extract_to_cfile(&za, i, out, 0) != 0;
        const bool flushed = std::fflush(out) == 0;
        std::fclose(out);
        if (!r || !flushed) { ok = false; *err = "cannot unpack " + name; }
        if (progress && (i % 64 == 0)) progress((float)i / (float)std::max<mz_uint>(1, n));
    }
    mz_zip_reader_end(&za);
    std::fclose(zf);
    if (ok && progress) progress(1.f);
    return ok;
}

/* ======================================================================================================================== */
/* the kit                                                                                                                  */
/* ======================================================================================================================== */

namespace {

struct CpmDir { const char* pin_id; const char* name; const char* tag; const char* strip; const char* must_have; };   // strip: leading folders of the archive to drop
const CpmDir kCpm[] = {{"cpm-vulkanheaders", "vulkanheaders", "v1.4.345", "1", "include"},
                       {"cpm-spirv-headers", "spirv-headers", "vulkan-sdk-1.4.341.0", "1", "include"},
                       {"cpm-vulkanmemoryallocator", "vulkanmemoryallocator", "v3.3.0", "1", "include"},
                       {"cpm-ffmpeg", "ffmpeg", "9.0.2-1790726865-946fcce07b", "0", "lib/avcodec.lib"}};   // include\ and lib\ sit at the archive's top

/* The parts of LLVM the build runs: the compiler (clang, clang++), the linker (lld, lld-link), archivers and the two tools make_game
 * calls (llvm-cxxfilt, llvm-profdata), and clang's own headers + runtime (lib\clang). The other 3 GB of the archive (clang-tidy, clangd,
 * static libraries, ...) are not unpacked. */
const char* const kLlvmInclude[] = {"bin/clang.exe", "bin/clang++.exe", "bin/lld.exe", "bin/lld-link.exe", "bin/ld.lld.exe",
                                    "bin/llvm-ar.exe", "bin/llvm-lib.exe", "bin/llvm-ranlib.exe", "bin/llvm-nm.exe", "bin/llvm-objdump.exe",
                                    "bin/llvm-strip.exe", "bin/llvm-rc.exe", "bin/llvm-mt.exe", "bin/llvm-cxxfilt.exe", "bin/llvm-profdata.exe",
                                    "lib/clang/*"};
const char* kLlvmRev = "subset-1";

const std::wstring kMarker = L".retrohub-kit";

struct Kit {
    const KitIO& io;
    std::string err;
    explicit Kit(const KitIO& i) : io(i) {}
    bool cancelled() const { return io.cancel && io.cancel->load(); }
    void say(const std::string& id, const std::string& text, float p) { if (io.report) io.report(id, text, p); }
    void log(const std::string& s) { if (io.log) io.log(s); }
    bool fail(const std::string& m) { if (err.empty()) err = m; return false; }
    fs::path dir(const char* n) const { return P(io.kit) / n; }

    bool marker_is(const fs::path& d, const std::string& want) const {
        std::ifstream f(d / kMarker, std::ios::binary);
        if (!f) return false;
        std::string have((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return trim(have) == want;
    }
    bool write_marker(const fs::path& d, const std::string& text) const {
        std::ofstream f(d / kMarker, std::ios::binary | std::ios::trunc);
        f << text << "\n";
        return (bool)f;
    }
    void wipe(const fs::path& d) const { std::error_code ec; fs::remove_all(d, ec); }

    /* The kit's Python, for the unpack script, pip and make_game */
    std::string python_exe() const { return S(P(io.kit) / "python" / "python.exe"); }
    EnvEdit tool_env() const {
        EnvEdit e;
        e.drop_prefix = {L"PYTHON", L"PIP_"};
        e.set = {{L"PYTHONIOENCODING", L"utf-8"}, {L"PYTHONUTF8", L"1"}, {L"PIP_CONFIG_FILE", L"NUL"}, {L"PIP_DISABLE_PIP_VERSION_CHECK", L"1"},
                 {L"PIP_NO_INPUT", L"1"}, {L"PIP_NO_CACHE_DIR", L"1"}};
        return e;
    }

    /* Runs a program of the kit; its lines go to the log, "@@progress x" lines to on_progress. false + err on a failure. */
    bool run(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd, const std::string& what,
             const std::function<void(float)>& on_progress = nullptr) {
        RunCtl ctl{io.cancel, io.pause};
        std::string last_error;
        std::string e;
        log("> " + exe + " " + (args.empty() ? "" : args[0]));
        const int code = run_in_job(exe, args, cwd, tool_env(), ctl, [&](const std::string& line) {
            log(line);
            BuildStatus tmp;
            if (parse_protocol_line(line, &tmp)) {
                if (!tmp.error.empty()) last_error = tmp.error;
                if (on_progress && starts_with(line, "@@progress")) on_progress(tmp.progress);
            }
        }, &e, false);
        if (code == -2) return fail("cancelled");
        if (code == -1) return fail(e);
        if (code != 0) return fail(what + " failed" + (last_error.empty() ? " (exit code " + std::to_string(code) + ")" : ": " + last_error));
        return true;
    }

    /* A pinned download into <kit>\downloads (reused when it is already there and verifies). Returns its path. */
    bool fetch(const Pin& p, const std::string& stage_id, const std::string& text, float p0, float p1, std::string* path) {
        std::error_code ec;
        const fs::path dl = P(io.kit) / "downloads";
        fs::create_directories(dl, ec);
        const fs::path dest = dl / p.file;
        *path = S(dest);
        if (is_file(dest) && fs::file_size(dest, ec) == p.bytes && pin_hash_ok(p, *path)) { log("using the download already in the kit: " + std::string(p.file)); return true; }
        fs::remove(dest, ec);
        const std::string part = *path + ".part";
        for (int attempt = 1; attempt <= 3; ++attempt) {
            if (cancelled()) return fail("cancelled");
            std::atomic<uint64_t> got{0}, total{0};
            std::atomic<bool> done{false};
            std::string e;
            bool ok = false;
            log("downloading https://" + std::string(p.host) + p.path + " (attempt " + std::to_string(attempt) + ")");
            std::thread th([&] {
                ok = hubemu::http_get_file(p.host, p.path, part, &got, &total, io.cancel, io.pause, &e);
                done = true;
            });
            while (!done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                const double t = total.load() ? (double)got.load() / (double)total.load() : (double)got.load() / (double)p.bytes;
                say(stage_id, text, p0 + (p1 - p0) * (float)std::min(1.0, t));
            }
            th.join();
            if (cancelled()) { fs::remove(part, ec); return fail("cancelled"); }
            if (ok) {
                const uint64_t sz = is_file(P(part)) ? (uint64_t)fs::file_size(P(part), ec) : 0;
                if (sz == p.bytes && pin_hash_ok(p, part)) {
                    fs::rename(P(part), dest, ec);
                    if (ec) return fail("could not save the download: " + ec.message());
                    return true;
                }
                log("the download did not match its checksum (" + std::to_string(sz) + " bytes)");
                e = "the download did not match its checksum";
            }
            fs::remove(P(part), ec);
            log("download failed: " + e);
            if (attempt == 3) return fail(std::string("Could not download ") + p.what + ": " + e + ". Nothing was installed.");
            std::this_thread::sleep_for(std::chrono::seconds(2 * attempt));
        }
        return false;
    }

    bool step_src() {
        say("kit-src", "Unpacking the Hookshot source", 0.f);
        const std::string zip = S(P(io.data) / kSourceZipName);
        const std::string sha = hash_file_hex(HashAlg::Sha256, zip);
        if (sha != kSourceZipSha256) return fail("RetroHub's copy of the Hookshot source is missing or damaged. Reinstall RetroHub.");
        const fs::path d = dir("src");
        const std::string want = "src " + sha;
        if (!marker_is(d, want)) {
            wipe(d);
            std::string e;
            if (!unzip_prefix(zip, S(d), "", io.cancel, [&](float f) { say("kit-src", "Unpacking the Hookshot source", f); }, &e)) return fail(e == "cancelled" ? e : "Could not unpack the Hookshot source: " + e);
            if (!is_file(d / "nx2" / "make_game.py")) return fail("The Hookshot source is incomplete.");
            write_marker(d, want);
        }
        // RetroHub's own SDL2.dll (every build, so it follows RetroHub's updates)
        std::error_code ec;
        fs::create_directories(d / "third_party" / "sdl2", ec);
        fs::copy_file(P(io.sdl2_dll), d / "third_party" / "sdl2" / "SDL2.dll", fs::copy_options::overwrite_existing, ec);
        if (ec) return fail("Could not copy SDL2.dll from RetroHub (" + ec.message() + ").");
        say("kit-src", "Unpacking the Hookshot source", 1.f);
        return true;
    }

    bool step_python() {
        const Pin& p = pin("python");
        say("kit-python", "Getting Python", 0.f);
        const fs::path d = dir("python");
        const std::string req = S(P(io.data) / "requirements.txt");
        const std::string req_sha = hash_file_hex(HashAlg::Sha256, req);
        if (req_sha.empty()) return fail("RetroHub's list of Python packages is missing. Reinstall RetroHub.");
        const std::string want = std::string("python ") + p.hash + " " + req_sha;
        if (marker_is(d, want) && is_file(d / "python.exe")) { say("kit-python", "Getting Python", 1.f); return true; }
        std::string pkg;
        if (!fetch(p, "kit-python", "Downloading Python", 0.f, 0.5f, &pkg)) return false;
        wipe(d);
        say("kit-python", "Unpacking Python", 0.5f);
        std::string e;
        if (!unzip_prefix(pkg, S(d), "tools/", io.cancel, nullptr, &e)) return fail(e == "cancelled" ? e : "Could not unpack Python: " + e);
        if (!is_file(d / "python.exe")) return fail("The Python download did not contain python.exe.");
        say("kit-python", "Setting up Python", 0.6f);
        if (!run(python_exe(), {"-I", "-m", "ensurepip"}, io.kit, "Setting up pip")) return false;
        say("kit-python", "Installing the build's Python packages", 0.75f);
        if (!run(python_exe(), {"-I", "-m", "pip", "install", "--require-hashes", "--only-binary", ":all:", "--no-deps", "--no-warn-script-location", "-r", req},
                 io.kit, "Installing the Python packages"))
            return false;
        if (!run(python_exe(), {"-I", "-c", "import lz4, numpy, capstone, unicorn, zstandard"}, io.kit, "Checking the Python packages")) return false;
        write_marker(d, want);
        say("kit-python", "Getting Python", 1.f);
        return true;
    }

    bool step_llvm() {
        const Pin& p = pin("llvm");
        say("kit-llvm", "Getting the compiler", 0.f);
        const fs::path d = dir("llvm");
        const std::string want = std::string("llvm ") + p.hash + " " + kLlvmRev;
        if (marker_is(d, want) && is_file(d / "bin" / "clang.exe")) { say("kit-llvm", "Getting the compiler", 1.f); return true; }
        std::string pkg;
        if (!fetch(p, "kit-llvm", "Downloading the compiler (LLVM, 263 MB)", 0.f, 0.5f, &pkg)) return false;
        wipe(d);
        say("kit-llvm", "Unpacking the compiler", 0.5f);
        std::vector<std::string> args = {"-I", S(P(io.data) / "unpack_tar.py"), pkg, S(d), "--strip", "1", "--include"};
        for (const char* inc : kLlvmInclude) args.push_back(inc);
        if (!run(python_exe(), args, io.kit, "Unpacking the compiler", [&](float f) { say("kit-llvm", "Unpacking the compiler", 0.5f + 0.5f * f); })) return false;
        if (!is_file(d / "bin" / "clang.exe") || !is_file(d / "bin" / "lld.exe")) return fail("The compiler download is missing clang.exe or lld.exe.");
        write_marker(d, want);
        say("kit-llvm", "Getting the compiler", 1.f);
        return true;
    }

    bool step_cpm() {
        say("kit-cpm", "Getting the Vulkan headers", 0.f);
        int i = 0;
        for (const CpmDir& c : kCpm) {
            const Pin& p = pin(c.pin_id);
            const fs::path d = dir("cpm") / c.name / c.tag;
            const std::string want = std::string("cpm ") + p.hash;
            if (!(marker_is(d, want) && is_dir(d / "include"))) {
                std::string pkg;
                if (!fetch(p, "kit-cpm", std::string("Downloading ") + p.what, (float)i / 4.f, (float)(i + 0.6) / 4.f, &pkg)) return false;
                wipe(d);
                if (!run(python_exe(), {"-I", S(P(io.data) / "unpack_tar.py"), pkg, S(d), "--strip", c.strip}, io.kit, std::string("Unpacking ") + p.what)) return false;
                if (!is_dir(d / "include") || !is_file(d / c.must_have)) return fail(std::string(p.what) + " is missing its include or lib files.");
                write_marker(d, want);
            }
            ++i;
            say("kit-cpm", "Getting the Vulkan headers and FFmpeg", (float)i / 4.f);
        }
        return true;
    }
};

}   // namespace

bool kit_prepare(const KitIO& io, std::string* err) {
    Kit k(io);
    std::error_code ec;
    fs::create_directories(P(io.kit), ec);
    fs::create_directories(P(io.kit) / "logs", ec);
    const bool ok = k.step_src() && k.step_python() && k.step_llvm() && k.step_cpm();
    if (!ok && err) *err = k.err.empty() ? std::string("The build tools could not be set up.") : k.err;
    return ok;
}

/* ======================================================================================================================== */
/* the kit's size and removal                                                                                               */
/* ======================================================================================================================== */

uint64_t kit_size_bytes() {
    uint64_t total = 0;
    std::error_code ec;
    if (!is_dir(P(kit_dir()))) return 0;
    for (fs::recursive_directory_iterator it(P(kit_dir()), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) total += (uint64_t)it->file_size(e2);
    }
    return total;
}

void kit_size_async() {
    if (g().kit_busy.exchange(true)) return;
    std::thread([] {
        g().kit_size = (int64_t)kit_size_bytes();
        g().kit_busy = false;
    }).detach();
}

int64_t kit_size_cached() { return g().kit_size.load(); }
bool kit_busy() { return g().kit_busy.load(); }

bool remove_kit(std::string* err) {
    if (g().running.load()) { if (err) *err = "A build is running."; return false; }
    std::error_code ec;
    fs::remove_all(P(kit_dir()), ec);
    if (ec || pexists(P(kit_dir()))) { if (err) *err = "Could not delete everything in " + kit_dir() + (ec ? " (" + ec.message() + ")" : ""); return false; }
    g().kit_size = 0;
    return true;
}

void remove_kit_async() {
    if (g().running.load()) return;
    if (g().kit_busy.exchange(true)) return;
    std::thread([] {
        std::string e;
        remove_kit(&e);
        g().kit_size = (int64_t)kit_size_bytes();
        g().kit_busy = false;
    }).detach();
}

/* ======================================================================================================================== */
/* requirements                                                                                                             */
/* ======================================================================================================================== */

namespace {

/* Just enough of Vulkan to ask "is there a 1.3 device with VK_EXT_mesh_shader": no headers, no window, no device created. */
struct VkAppInfo { uint32_t sType; const void* pNext; const char* appName; uint32_t appVer; const char* engName; uint32_t engVer; uint32_t apiVersion; };
struct VkInstInfo { uint32_t sType; const void* pNext; uint32_t flags; const VkAppInfo* app; uint32_t nLayers; const char* const* layers; uint32_t nExts; const char* const* exts; };
struct VkExtProps { char name[256]; uint32_t spec; };
typedef void*(__stdcall* PFN_gipa)(void*, const char*);
typedef int32_t(__stdcall* PFN_create)(const VkInstInfo*, const void*, void**);
typedef void(__stdcall* PFN_destroy)(void*, const void*);
typedef int32_t(__stdcall* PFN_enum_pd)(void*, uint32_t*, void**);
typedef void(__stdcall* PFN_pd_props)(void*, void*);
typedef int32_t(__stdcall* PFN_enum_ext)(void*, const char*, uint32_t*, VkExtProps*);

void probe_vulkan(bool* mesh, std::string* gpu) {
    *mesh = false;
    gpu->clear();
    HMODULE dll = LoadLibraryW(L"vulkan-1.dll");   // left loaded: some drivers do not like being unloaded
    if (!dll) return;
    auto gipa = (PFN_gipa)(void*)GetProcAddress(dll, "vkGetInstanceProcAddr");
    auto create = (PFN_create)(void*)GetProcAddress(dll, "vkCreateInstance");
    if (!gipa || !create) return;
    VkAppInfo app{0, nullptr, "RetroHub", 1, nullptr, 0, (1u << 22) | (3u << 12)};   // API 1.3
    VkInstInfo ci{1, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    void* inst = nullptr;
    if (create(&ci, nullptr, &inst) != 0 || !inst) return;
    auto destroy = (PFN_destroy)gipa(inst, "vkDestroyInstance");
    auto enum_pd = (PFN_enum_pd)gipa(inst, "vkEnumeratePhysicalDevices");
    auto props = (PFN_pd_props)gipa(inst, "vkGetPhysicalDeviceProperties");
    auto enum_ext = (PFN_enum_ext)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    if (enum_pd && props && enum_ext) {
        uint32_t n = 0;
        std::vector<void*> devs;
        if (enum_pd(inst, &n, nullptr) == 0 && n > 0) {
            devs.resize(n);
            if (enum_pd(inst, &n, devs.data()) < 0) devs.clear();
        }
        for (void* d : devs) {
            alignas(8) unsigned char pr[2048] = {0};   // VkPhysicalDeviceProperties is 824 bytes: apiVersion at 0, deviceType at 16, deviceName at 20
            props(d, pr);
            uint32_t api = 0, type = 0;
            std::memcpy(&api, pr, 4);
            std::memcpy(&type, pr + 16, 4);
            const std::string name((const char*)pr + 20, strnlen((const char*)pr + 20, 255));
            bool has_mesh = false;
            uint32_t ne = 0;
            if (enum_ext(d, nullptr, &ne, nullptr) >= 0 && ne > 0) {
                std::vector<VkExtProps> ex(ne);
                if (enum_ext(d, nullptr, &ne, ex.data()) >= 0)
                    for (uint32_t i = 0; i < ne; ++i) if (std::strcmp(ex[i].name, "VK_EXT_mesh_shader") == 0) has_mesh = true;
            }
            const bool good = api >= ((1u << 22) | (3u << 12)) && has_mesh;
            if (good && !*mesh) { *mesh = true; *gpu = name; }
            else if (!*mesh && gpu->empty() && type != 4 /* CPU */) *gpu = name;
        }
    }
    if (destroy) destroy(inst, nullptr);
}

bool find_vs_tools() {
    wchar_t pf[MAX_PATH] = {0};
    if (!GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH)) return false;
    const std::string vswhere = S(fs::path(pf) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe");
    if (!is_file(P(vswhere))) return false;
    std::string out;
    if (!capture_command(vswhere, {"-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"},
                         20000, &out))
        return false;
    return !trim(out).empty();
}

bool find_windows_sdk() {
    wchar_t pf[MAX_PATH] = {0};
    if (!GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH)) return false;
    const fs::path inc = fs::path(pf) / "Windows Kits" / "10" / "Include";
    std::error_code ec;
    for (fs::directory_iterator it(inc, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = S(it->path().filename());
        if (starts_with(n, "10.") && is_file(it->path() / "um" / "windows.h")) return true;
    }
    return false;
}

}   // namespace

Requirements check_requirements(const hub::GameEntry& e) {
    Requirements r;
    r.checked = true;
    r.game_path = e.path;
    const GameSource src = resolve_game_source(e.path);
    r.game_ok = src.kind != GameSource::None;
    r.game_problem = src.problem;
    r.game_fix = src.fix;
    r.vs_tools = find_vs_tools();
    r.win_sdk = find_windows_sdk();
    probe_vulkan(&r.vulkan_mesh, &r.gpu_name);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) r.ram_gb = (ms.ullTotalPhys + (512ull << 20)) >> 30;   // rounded: a 16 GB PC reports a bit less
    ULARGE_INTEGER freeb{};
    std::error_code ec;
    fs::create_directories(P(emu_root()), ec);
    if (GetDiskFreeSpaceExW(W(emu_root()).c_str(), &freeb, nullptr, nullptr)) r.free_gb = freeb.QuadPart >> 30;
    fill_requirement_problem(&r);
    return r;
}

Requirements requirements_cached() {
    std::lock_guard<std::mutex> lk(g().mu);
    return g().req;
}

bool requirements_busy() { return g().req_busy.load(); }

void refresh_requirements_async(const hub::GameEntry& e) {
    if (g().req_busy.exchange(true)) return;
    hub::GameEntry copy;
    copy.path = e.path;
    copy.key = e.key;
    std::thread([copy] {
        Requirements r = check_requirements(copy);
        { std::lock_guard<std::mutex> lk(g().mu); g().req = std::move(r); }
        g().req_busy = false;
    }).detach();
}

/* ======================================================================================================================== */
/* the build                                                                                                                */
/* ======================================================================================================================== */

namespace {

std::string stamp_text() {
    char b[32];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return b;
}

void set_state(BuildState s, const std::string& id, const std::string& text) {
    std::lock_guard<std::mutex> lk(g().mu);
    g().st.state = s;
    if (!id.empty()) g().st.stage_id = id;
    g().st.stage = text;
    g().st.progress = 0.f;
}

void raise_overall(const std::string& id, float p) {
    const float o = overall_for(id, p);
    std::lock_guard<std::mutex> lk(g().mu);
    if (o >= 0.f && o > g().st.overall) g().st.overall = o;
}

void finish(BuildState s, const std::string& error) {
    {
        std::lock_guard<std::mutex> lk(g().mu);
        g().st.state = s;
        g().st.error = error;
        if (s == BuildState::Done) { g().st.overall = 1.f; g().st.progress = 1.f; g().st.stage = "Ready"; }
    }
    log_line(s == BuildState::Done ? std::string("== done") : s == BuildState::Cancelled ? std::string("== cancelled") : "== failed: " + error);
    {
        std::lock_guard<std::mutex> lk(g().log_mu);
        if (g().log.is_open()) g().log.close();
    }
    g().running = false;
}

void prune_logs(const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> logs;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (starts_with(S(it->path().filename()), "build-") && it->path().extension() == ".log") logs.push_back(it->path());
    std::sort(logs.begin(), logs.end());
    while (logs.size() > 5) { fs::remove(logs.front(), ec); logs.erase(logs.begin()); }
}

/* Looks every 2 s for a running game and sets g().pause; stops (and joins) on halt() or when it goes out of scope. */
struct Monitor {
    std::atomic<bool> stop{false};
    std::thread th;
    Monitor() {
        th = std::thread([this] {
            while (!stop) {
                g().pause = game_running_now();
                for (int i = 0; i < 20 && !stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            g().pause = false;
        });
    }
    void halt() { stop = true; if (th.joinable()) th.join(); }
    ~Monitor() { halt(); }
};

uint64_t total_phys_bytes() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    return GlobalMemoryStatusEx(&ms) ? ms.ullTotalPhys : 0;
}

std::wstring current_path_env() {
    std::vector<wchar_t> b(32768);
    DWORD n = GetEnvironmentVariableW(L"PATH", b.data(), (DWORD)b.size());
    return n < b.size() ? std::wstring(b.data(), n) : std::wstring();
}

void run_build(std::string game_path, std::string key, std::string language) {
    (void)key;
    const std::string kit = kit_dir();
    const std::string staging = staging_dir();
    const std::string game = hookshot::game_dir();
    const std::string old = old_dir();
    const std::string work = S(P(kit) / "work");
    Monitor mon;
    auto stop_monitor = [&] { mon.halt(); };
    auto cancelled = [&] { return g().cancel.load(); };
    auto bail = [&](const std::string& msg) {   // a failure: the partly built files go, the tools stay
        std::error_code ec;
        fs::remove_all(P(staging), ec);
        stop_monitor();
        finish(BuildState::Failed, msg);
    };
    auto was_cancelled = [&] {
        std::error_code ec;
        fs::remove_all(P(staging), ec);
        fs::remove_all(P(work), ec);
        stop_monitor();
        finish(BuildState::Cancelled, "");
    };

    std::error_code ec;
    fs::create_directories(P(kit) / "logs", ec);
    prune_logs(P(kit) / "logs");
    const std::string log_path = S(P(kit) / "logs" / ("build-" + stamp_text() + ".log"));
    {
        std::lock_guard<std::mutex> lk(g().log_mu);
        g().log.open(P(log_path), std::ios::binary | std::ios::trunc);
    }
    { std::lock_guard<std::mutex> lk(g().mu); g().st.log_path = log_path; }
    log_line(std::string("RetroHub Hookshot build, source ") + kSourceCommit + ", language " + language);
    log_line("game: " + game_path);

    // 0. an earlier swap that was cut short
    const std::string rec = recover_swap(staging, game, old);
    if (!rec.empty()) log_line("recovered an interrupted swap: " + rec);

    // 1. requirements, fresh
    set_state(BuildState::Preparing, "prepare", "Checking your PC");
    hub::GameEntry probe_entry;
    probe_entry.path = game_path;
    Requirements req = check_requirements(probe_entry);
    { std::lock_guard<std::mutex> lk(g().mu); g().req = req; }
    if (!req.problem.empty()) { log_line("requirement: " + req.problem + " | " + req.fix_hint); return bail(req.problem + (req.fix_hint.empty() ? "" : " " + req.fix_hint + ".")); }
    const GameSource src = resolve_game_source(game_path);
    if (src.kind == GameSource::None) return bail(src.problem);
    if (cancelled()) return was_cancelled();

    // a leftover staging folder from a crash goes (its saves, if any, were put back above); the build starts clean
    if (is_dir(P(staging) / "user") && !dir_empty(P(staging) / "user")) return bail("An earlier build left a folder with saves in it (" + staging + "). It was kept: move it away and try again.");
    fs::remove_all(P(staging), ec);

    // 2. the kit
    set_state(BuildState::Downloading, "kit-src", "Getting the build tools ready");
    KitIO io;
    io.kit = kit;
    io.data = hub::exe_dir() + "data/hookshot";
    io.sdl2_dll = hub::exe_dir() + "SDL2.dll";
    io.cancel = &g().cancel;
    io.pause = &g().pause;
    io.report = [](const std::string& id, const std::string& text, float p) {
        {
            std::lock_guard<std::mutex> lk(g().mu);
            if (g().st.stage_id != id) g().st.stage_id = id;
            g().st.stage = text;
            g().st.progress = p;
        }
        raise_overall(id, p);
    };
    io.log = log_line;
    std::string err;
    if (!kit_prepare(io, &err)) { log_line("kit: " + err); if (cancelled() || err == "cancelled") return was_cancelled(); return bail(err); }
    if (cancelled()) return was_cancelled();

    // 3. the check
    set_state(BuildState::Checking, "check", "Checking the game file");
    const std::string py = S(P(kit) / "python" / "python.exe");
    const std::string srcdir = S(P(kit) / "src");
    EnvEdit env;
    env.drop_prefix = {L"PYTHON", L"PIP_"};
    const int jobs = jobs_for(total_phys_bytes(), (int)std::max(1u, std::thread::hardware_concurrency()));
    env.set = {{L"PYTHONIOENCODING", L"utf-8"},
               {L"PYTHONUTF8", L"1"},
               {L"PATH", W(S(P(kit) / "llvm" / "bin")) + L";" + current_path_env()},
               {L"EMSDK", W(S(P(kit) / "no-emsdk"))},
               {L"RX_CPM", W(S(P(kit) / "cpm"))},
               {L"WGPU_NATIVE", W(S(P(kit) / "no-wgpu"))},
               {L"RX_JOBS", W(std::to_string(jobs))}};
    RunCtl ctl{&g().cancel, &g().pause};
    bool check_ok = false;
    std::string check_text;
    {
        std::vector<std::string> a = {"-I", "nx2\\make_game.py"};
        for (const std::string& s : game_source_args(src)) a.push_back(s);
        a.push_back("--work");
        a.push_back(work);
        a.push_back("--check");
        BuildStatus tmp;
        std::string e;
        const int code = run_in_job(py, a, srcdir, env, ctl, [&](const std::string& line) {
            log_line(line);
            BuildStatus t;
            if (parse_protocol_line(line, &t)) {
                if (starts_with(line, "@@check ok")) { check_ok = true; check_text = t.stage; }
                else if (starts_with(line, "@@check bad")) { check_ok = false; check_text = t.error; }
                else if (starts_with(line, "@@error")) { check_ok = false; check_text = t.error; }
            }
        }, &e, true);
        if (code == -2) return was_cancelled();
        if (code == -1) return bail("The build tools could not start: " + e);
        if (!check_ok) return bail(check_text.empty() ? "This game file can't be used (exit code " + std::to_string(code) + "). See the log." : check_text);
        log_line("check: " + check_text);
    }
    raise_overall("check", 1.f);

    // 4. the build
    set_state(BuildState::Building, "read", "Reading your game files");
    std::string done_dir, build_error;
    {
        std::vector<std::string> a = {"-I", "nx2\\make_game.py"};
        for (const std::string& s : game_source_args(src)) a.push_back(s);
        a.push_back("--out");
        a.push_back(staging);
        a.push_back("--work");
        a.push_back(work);
        a.push_back("--language");
        a.push_back(language);
        a.push_back("--jobs");
        a.push_back(std::to_string(jobs));
        std::string e;
        const int code = run_in_job(py, a, srcdir, env, ctl, [&](const std::string& line) {
            log_line(line);
            std::lock_guard<std::mutex> lk(g().mu);
            BuildStatus t = g().st;
            if (!parse_protocol_line(line, &t)) return;
            if (starts_with(line, "@@error")) { build_error = t.error; return; }
            if (starts_with(line, "@@done")) { done_dir = t.stage; return; }
            g().st.stage_id = t.stage_id;
            g().st.stage = t.stage;
            g().st.progress = t.progress;
            const float o = overall_for(t.stage_id, t.progress);
            if (o >= 0.f && o > g().st.overall) g().st.overall = o;
        }, &e, true);
        if (code == -2) return was_cancelled();
        if (code == -1) return bail("The build tools could not start: " + e);
        if (code != 0 || done_dir.empty()) return bail(!build_error.empty() ? build_error : "The build stopped (exit code " + std::to_string(code) + "). See the log.");
    }

    // 5. stamp and swap
    set_state(BuildState::Building, "swap", "Putting the game in place");
    raise_overall("swap", 0.f);
    if (cancelled()) return was_cancelled();   // (the build is done but not in place: nothing is lost by stopping here)
    if (!is_file(P(staging) / "SDL2.dll")) {   // the build normally copies it; RetroHub's own is the same one
        std::error_code e2;
        fs::copy_file(P(io.sdl2_dll), P(staging) / "SDL2.dll", fs::copy_options::overwrite_existing, e2);
    }
    hookshot::Stamp stamp;
    stamp.commit = kSourceCommit;
    stamp.game_build = kGameBuild;
    stamp.source = game_path;
    stamp.built = (int64_t)std::time(nullptr);
    stamp.size = src.kind == GameSource::Nup ? (uint64_t)fs::file_size(P(src.nup), ec) : 0;
    if (!hookshot::write_stamp(staging, stamp)) return bail("Could not write the build's stamp file.");
    std::string why;
    if (!hookshot::ready(&why, staging)) return bail("The build finished but is incomplete: " + why + ".");
    std::string swap_err;
    if (!swap_in(staging, game, old, &swap_err)) {   // both folders stay: no partial removal
        log_line("swap: " + swap_err);
        stop_monitor();
        finish(BuildState::Failed, swap_err);
        return;
    }
    fs::remove_all(P(work), ec);   // tens of GB
    stop_monitor();
    finish(BuildState::Done, "");
}

}   // namespace

bool start_build(const hub::GameEntry& e, const std::string& language, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    if (!supports(e)) return fail("This game can't be built.");
    if (!language_valid(language)) return fail("Unknown language.");
    if (g().kit_busy.load()) return fail("The build tools are being removed. Try again in a moment.");
    if (g().running.exchange(true)) return fail("A build is already running.");
    if (g().th.joinable()) g().th.join();
    g().cancel = false;
    g().pause = false;
    {
        std::lock_guard<std::mutex> lk(g().mu);
        g().st = BuildStatus();
        g().st.state = BuildState::Preparing;
        g().st.stage_id = "prepare";
        g().st.stage = "Starting";
        g().st.started_unix = (int64_t)std::time(nullptr);
    }
    const std::string path = e.path, key = e.key;
    g().th = std::thread([path, key, language] {
        try {
            run_build(path, key, language);
        } catch (const std::exception& ex) {
            finish(BuildState::Failed, std::string("The build stopped unexpectedly: ") + ex.what());
        } catch (...) {
            finish(BuildState::Failed, "The build stopped unexpectedly.");
        }
    });
    return true;
}

void cancel_build() { g().cancel = true; }

bool build_running() { return g().running.load(); }

BuildStatus build_status() {
    std::lock_guard<std::mutex> lk(g().mu);
    BuildStatus s = g().st;
    if (g().running.load() && g().pause.load() && s.state != BuildState::Done && s.state != BuildState::Failed && s.state != BuildState::Cancelled) {
        s.state = BuildState::Paused;
        s.paused_for_game = true;
    }
    return s;
}

}   // namespace hookshot
