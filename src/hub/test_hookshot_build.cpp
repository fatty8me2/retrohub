/* Console test for the Hookshot build pipeline (hookshot_build.h): the protocol parser, RX_JOBS, hashes against known vectors, the pins
 * and requirements.txt, the game-source rules, the requirement texts, the stage weights, and the folder swap (temp folders only: it never
 * touches a real data folder, never downloads and never starts a build).
 *   hub_hookshot_build_test                      runs everything
 *   hub_hookshot_build_test --kit <data> <sdl2> <temp-kit-folder>
 *        MANUAL smoke test: runs kit_prepare() for real (downloads ~290 MB, verifies, unpacks) into the given folder. <data> = the
 *        folder with hookshot-src-137c423.zip, unpack_tar.py and requirements.txt. Never run by ctest. */
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <thread>

#include "../common/library.h"
#include "../common/winutil.h"
#include "hookshot.h"
#include "hookshot_build.h"

namespace fs = std::filesystem;
using namespace hookshot;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (_a == _b) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

static void write(const fs::path& p, const std::string& s) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}
static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static bool has(const fs::path& p) { std::error_code ec; return fs::exists(p, ec); }

/* ---- protocol ---- */
static void test_protocol() {
    BuildStatus st;
    CHECK(parse_protocol_line("@@stage lift Translating the game to PC code", &st));
    CHECK_EQ(st.stage_id, std::string("lift"));
    CHECK_EQ(st.stage, std::string("Translating the game to PC code"));
    CHECK(st.state == BuildState::Building);
    CHECK_EQ(st.progress, 0.f);

    CHECK(parse_protocol_line("@@progress 0.250", &st));
    CHECK(st.progress > 0.249f && st.progress < 0.251f);
    CHECK(parse_protocol_line("@@progress 7", &st));
    CHECK_EQ(st.progress, 1.f);
    CHECK(parse_protocol_line("@@progress -3\r\n", &st));
    CHECK_EQ(st.progress, 0.f);

    BuildStatus c;
    CHECK(parse_protocol_line("@@check ok This is the right game and version.", &c));
    CHECK_EQ(c.stage_id, std::string("check"));
    CHECK_EQ(c.stage, std::string("This is the right game and version."));
    CHECK(c.error.empty());
    CHECK(parse_protocol_line("@@check bad This is build 12345678. Hookshot supports build e0f3ccfa.", &c));
    CHECK_EQ(c.error, std::string("This is build 12345678. Hookshot supports build e0f3ccfa."));

    BuildStatus d;
    CHECK(parse_protocol_line("@@done C:\\Users\\me\\out folder", &d));
    CHECK_EQ(d.stage_id, std::string("done"));
    CHECK_EQ(d.stage, std::string("C:\\Users\\me\\out folder"));
    CHECK_EQ(d.progress, 1.f);

    BuildStatus e;
    CHECK(parse_protocol_line("@@error nx2/build.py stopped: error: boom", &e));
    CHECK(e.state == BuildState::Failed);
    CHECK_EQ(e.error, std::string("nx2/build.py stopped: error: boom"));
    BuildStatus e2;
    CHECK(parse_protocol_line("@@error", &e2));
    CHECK(e2.state == BuildState::Failed && !e2.error.empty());

    // not protocol lines: nothing changes
    BuildStatus j;
    j.stage_id = "keep";
    j.progress = 0.5f;
    const char* junk[] = {"", "hello", "  @@stage x y", "@@", "@@unknown thing", "@@progress abc", "@@progress 0.5x", "@@progress", "@@stage",
                          "@@check maybe nope", "@@check", "compiling foo.c", "[1/200] clang -c a.c", "@stage x y"};
    for (const char* line : junk) CHECK(!parse_protocol_line(line, &j));
    CHECK_EQ(j.stage_id, std::string("keep"));
    CHECK_EQ(j.progress, 0.5f);
    CHECK(j.state == BuildState::Idle);
    CHECK(parse_protocol_line("@@stage read Reading your game files", nullptr));   // null status: only classifies
}

static void test_jobs() {
    const uint64_t GB = 1ull << 30;
    CHECK_EQ(jobs_for(32 * GB, 22), 7);   // 32 * 0.8 / 3.5 = 7.3
    CHECK_EQ(jobs_for(16 * GB, 8), 3);    // 3.6
    CHECK_EQ(jobs_for(8 * GB, 16), 1);
    CHECK_EQ(jobs_for(4 * GB, 8), 1);     // never 0
    CHECK_EQ(jobs_for(64 * GB, 4), 4);    // never more than the cores
    CHECK_EQ(jobs_for(0, 6), 6);          // memory unknown: one a core, as toolchain.py does
    CHECK_EQ(jobs_for(32 * GB, 0), 1);    // cores unknown: counted as 1
}

/* ---- hashes ---- */
static void test_hashes() {
    CHECK_EQ(hash_data_hex(HashAlg::Sha256, "abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(hash_data_hex(HashAlg::Sha256, ""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(hash_data_hex(HashAlg::Sha512, "abc"),
             std::string("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
    CHECK_EQ(to_base64({'M', 'a', 'n'}), std::string("TWFu"));
    CHECK_EQ(to_base64({'M', 'a'}), std::string("TWE="));
    CHECK_EQ(to_base64({'M'}), std::string("TQ=="));
    CHECK_EQ(to_base64({}), std::string(""));
    CHECK_EQ(to_hex({0x00, 0xab, 0xff}), std::string("00abff"));
    // sha512("abc") in base64, the way nuget publishes package hashes
    std::vector<uint8_t> d;
    CHECK(hash_data(HashAlg::Sha512, "abc", 3, &d));
    CHECK_EQ(to_base64(d), std::string("3a81oZNherrMQXNJriBBMRLm+k6JqX6iCp7u5ktV05ohkpkqJ0/BqDa6PCOj/uu9RU1EI2Q86A4qmslPpUyknw=="));
    // a file, bigger than one read buffer
    const fs::path tmp = fs::temp_directory_path() / ("hs_hash_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(tmp, ec);
    std::string big(3 * 1024 * 1024 + 17, 'x');
    write(tmp / "big.bin", big);
    CHECK_EQ(hash_file_hex(HashAlg::Sha256, (tmp / "big.bin").u8string()), hash_data_hex(HashAlg::Sha256, big));
    CHECK_EQ(hash_file_hex(HashAlg::Sha256, (tmp / "missing.bin").u8string()), std::string(""));
    fs::remove_all(tmp, ec);
}

static void test_pins() {
    CHECK_EQ(pins().size(), (size_t)6);
    for (const Pin& p : pins()) {
        CHECK(std::string(p.host).find('/') == std::string::npos);
        CHECK(p.path[0] == '/');
        CHECK(p.bytes > 0);
        const size_t want = p.b64 ? 88 : p.alg == HashAlg::Sha256 ? 64 : 128;
        CHECK_EQ(std::string(p.hash).size(), want);
        if (!p.b64) for (const char* c = p.hash; *c; ++c) CHECK((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'));
    }
    // the official upstreams only
    CHECK_EQ(std::string(pins()[0].host), std::string("api.nuget.org"));
    CHECK_EQ(std::string(pins()[1].host), std::string("github.com"));
    CHECK(std::string(pins()[1].path).find("/llvm/llvm-project/releases/download/llvmorg-23.1.5/") == 0);
    CHECK_EQ(std::string(pins()[1].hash), std::string("b2be33385ddc86e83511417a060de927ae8b09d1f3017cc3cdce5c73cd5029d7"));
    CHECK_EQ(pins()[0].bytes, (uint64_t)14826465);
#ifdef HOOKSHOT_DATA_DIR
    const fs::path data = HOOKSHOT_DATA_DIR;
    CHECK_EQ(hash_file_hex(HashAlg::Sha256, (data / kSourceZipName).u8string()), std::string(kSourceZipSha256));
    // requirements.txt: every package pinned with at least one hash, nothing else installed
    const std::string req = slurp(data / "requirements.txt");
    for (const char* name : {"capstone==5.0.9", "lz4==4.4.5", "numpy==2.5.4", "unicorn==2.1.4", "zstandard==0.25.0"}) CHECK(req.find(name) != std::string::npos);
    size_t pinned = 0, hashes = 0;
    for (size_t p = 0; (p = req.find("==", p)) != std::string::npos; p += 2) ++pinned;
    for (size_t p = 0; (p = req.find("--hash=sha256:", p)) != std::string::npos; p += 14) ++hashes;
    CHECK_EQ(pinned, (size_t)5);
    CHECK(hashes >= 5);
    CHECK(req.find("--index-url") == std::string::npos && req.find("--extra-index-url") == std::string::npos && req.find("git+") == std::string::npos);
    CHECK(has(data / "unpack_tar.py"));
#endif
}

/* ---- languages ---- */
static void test_languages() {
    CHECK_EQ(languages().size(), (size_t)16);
    CHECK_EQ(std::string(languages()[0].code), std::string("en-US"));
    CHECK(language_valid("en-US") && language_valid("zh-Hant") && language_valid("es-419"));
    CHECK(!language_valid("") && !language_valid("en") && !language_valid("EN-us") && !language_valid("en-US --jobs 99") && !language_valid("../x"));
}

/* ---- game source ---- */
static void test_game_source() {
    const fs::path tmp = fs::temp_directory_path() / ("hs_src_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    write(tmp / "zelda.nup", "NUP");
    write(tmp / "zelda.nsp", "NSP");
    write(tmp / "dump" / "exefs" / "main", "NSO0");
    write(tmp / "dump" / "romfs" / "a.bin", "x");
    write(tmp / "bad" / "exefs" / "main", "NSO0");

    GameSource n = resolve_game_source((tmp / "zelda.nup").u8string());
    CHECK(n.kind == GameSource::Nup);
    CHECK_EQ(n.nup, (tmp / "zelda.nup").u8string());
    std::vector<std::string> a = game_source_args(n);
    CHECK_EQ(a.size(), (size_t)2);
    CHECK_EQ(a[0], std::string("--nup"));

    GameSource d = resolve_game_source((tmp / "dump" / "exefs" / "main").u8string());
    CHECK(d.kind == GameSource::Dump);
    CHECK_EQ(d.exefs, (tmp / "dump" / "exefs").u8string());
    CHECK_EQ(d.romfs, (tmp / "dump" / "romfs").u8string());
    a = game_source_args(d);
    CHECK_EQ(a.size(), (size_t)4);
    CHECK_EQ(a[0], std::string("--romfs"));
    CHECK_EQ(a[2], std::string("--exefs"));
    CHECK(resolve_game_source((tmp / "dump").u8string()).kind == GameSource::Dump);   // the dump's top folder works too

    GameSource b = resolve_game_source((tmp / "bad" / "exefs" / "main").u8string());
    CHECK(b.kind == GameSource::None && !b.problem.empty() && !b.fix.empty());
    GameSource s = resolve_game_source((tmp / "zelda.nsp").u8string());
    CHECK(s.kind == GameSource::None);
    CHECK(s.problem.find(".nsp") != std::string::npos);
    GameSource m = resolve_game_source((tmp / "gone.nup").u8string());
    CHECK(m.kind == GameSource::None && !m.problem.empty());
    CHECK(resolve_game_source("").kind == GameSource::None);
    CHECK(game_source_args(m).empty());
    fs::remove_all(tmp, ec);
}

/* ---- the requirement texts ---- */
static void test_problems() {
    Requirements ok;
    ok.checked = ok.vs_tools = ok.win_sdk = ok.game_ok = ok.vulkan_mesh = true;
    ok.ram_gb = 32;
    ok.free_gb = 200;
    fill_requirement_problem(&ok);
    CHECK(ok.vs_cpp);
    CHECK(ok.problem.empty() && ok.fix_hint.empty());
    CHECK(requirement_problems(ok).empty());

    Requirements bad;   // everything wrong
    bad.checked = true;
    bad.game_problem = "The game file can't be found.";
    bad.game_fix = "Connect the drive, then check again.";
    bad.ram_gb = 8;
    bad.free_gb = 12;
    bad.gpu_name = "Intel UHD";
    fill_requirement_problem(&bad);
    CHECK(!bad.vs_cpp);
    std::vector<Problem> p = requirement_problems(bad);
    CHECK_EQ(p.size(), (size_t)5);
    CHECK_EQ(p[0].id, std::string("game"));
    CHECK_EQ(p[1].id, std::string("vs"));
    CHECK_EQ(p[2].id, std::string("vulkan"));
    CHECK_EQ(p[3].id, std::string("ram"));
    CHECK_EQ(p[4].id, std::string("disk"));
    CHECK_EQ(bad.problem, p[0].text);                 // the first one is "the" problem
    CHECK_EQ(p[1].hint, std::string("Install Visual Studio Build Tools (free, from Microsoft) with 'Desktop development with C++'"));
    CHECK_EQ(p[1].url, std::string("https://visualstudio.microsoft.com/downloads/"));
    CHECK(!p[1].button.empty());
    CHECK(p[2].text.find("Intel UHD") != std::string::npos);
    CHECK(p[3].text.find("8 GB") != std::string::npos && p[3].text.find("16 GB") != std::string::npos);
    CHECK(p[4].text.find("12 GB") != std::string::npos && p[4].text.find("70 GB") != std::string::npos);
    for (const Problem& q : p) CHECK(q.url.empty() || q.url.rfind("https://", 0) == 0);

    Requirements sdk = ok;   // tools there, SDK not: the SDK problem, not the VS one
    sdk.win_sdk = false;
    p = requirement_problems(sdk);
    CHECK_EQ(p.size(), (size_t)1);
    CHECK_EQ(p[0].id, std::string("sdk"));

    Requirements edge = ok;   // exactly enough
    edge.ram_gb = 16;
    edge.free_gb = 70;
    CHECK(requirement_problems(edge).empty());
    edge.ram_gb = 15;
    CHECK_EQ(requirement_problems(edge).size(), (size_t)1);
    edge.ram_gb = 16;
    edge.free_gb = 69;
    CHECK_EQ(requirement_problems(edge)[0].id, std::string("disk"));
    Requirements nogpu = ok;
    nogpu.vulkan_mesh = false;
    CHECK(requirement_problems(nogpu)[0].text.find("No graphics card") != std::string::npos);
}

static void test_overall() {
    float prev = -1.f;
    for (const char* id : {"kit-src", "kit-python", "kit-llvm", "kit-cpm", "check", "read", "lift", "compile", "finish", "swap"}) {
        const float a = overall_for(id, 0.f), b = overall_for(id, 1.f);
        CHECK(a >= prev - 1e-6f);   // stages follow each other
        CHECK(b > a);
        prev = b;
    }
    CHECK_EQ(overall_for("swap", 1.f), 1.f);
    CHECK_EQ(overall_for("done", 0.f), 1.f);
    CHECK(overall_for("what", 0.5f) < 0.f);
    CHECK_EQ(overall_for("compile", 7.f), overall_for("compile", 1.f));
}

/* ---- the swap ---- */
struct Dirs {
    fs::path root, staging, game, old;
    Dirs() {
        root = fs::temp_directory_path() / ("hs_swap_test_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount()));
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        staging = root / "hookshot.staging";
        game = root / "hookshot";
        old = root / "hookshot.old";
    }
    ~Dirs() { std::error_code ec; fs::remove_all(root, ec); }
    void make_old_game() {
        write(game / "hookshot.exe", "old exe");
        write(game / "romfs" / "r.bin", "old romfs");
        write(game / "user" / "save" / "5a3c9e1f0b7d24680000000000000001" / "account.sav", "ACCOUNT");
        write(game / "user" / "save" / "5a3c9e1f0b7d24680000000000000001" / "slot_00" / "progress.sav", "PROGRESS");
        write(game / "hookshot_shaders.bin", "old shaders");
        write(game / "hookshot_pipelines.bin", "old pipelines");
        write(game / "mkw_vk_pipelines.bin", "old mkw");
        write(game / "hookshot_options.ini", "[menu]\nscale=2\n");
        write(game / "hookshot_log.txt", "old log");
    }
    void make_staging() {
        write(staging / "hookshot.exe", "new exe");
        write(staging / "romfs" / "r.bin", "new romfs");
        write(staging / "hookshot_shaders.bin", "new shaders");
        write(staging / "language.txt", "en-US");
    }
};

static void test_swap() {
    // 1. first build: nothing to keep
    {
        Dirs d;
        d.make_staging();
        std::string err;
        CHECK(swap_in(d.staging.u8string(), d.game.u8string(), d.old.u8string(), &err));
        CHECK(slurp(d.game / "hookshot.exe") == "new exe");
        CHECK(!has(d.staging));
    }
    // 2. a rebuild keeps the saves and the caches, and the new build's own files win
    {
        Dirs d;
        d.make_old_game();
        d.make_staging();
        std::string err;
        CHECK(swap_in(d.staging.u8string(), d.game.u8string(), d.old.u8string(), &err));
        CHECK(err.empty());
        CHECK(slurp(d.game / "hookshot.exe") == "new exe");
        CHECK(slurp(d.game / "romfs" / "r.bin") == "new romfs");
        const fs::path sv = d.game / "user" / "save" / "5a3c9e1f0b7d24680000000000000001";
        CHECK(slurp(sv / "account.sav") == "ACCOUNT");
        CHECK(slurp(sv / "slot_00" / "progress.sav") == "PROGRESS");
        CHECK(slurp(d.game / "hookshot_shaders.bin") == "new shaders");        // the new build's file wins
        CHECK(slurp(d.game / "hookshot_pipelines.bin") == "old pipelines");     // carried
        CHECK(slurp(d.game / "mkw_vk_pipelines.bin") == "old mkw");             // carried
        CHECK(slurp(d.game / "hookshot_options.ini") == "[menu]\nscale=2\n");   // carried
        CHECK(!has(d.game / "hookshot_log.txt"));                               // not a cache: left with the old build
        CHECK(!has(d.old));
        CHECK(!has(d.staging));
        CHECK(slurp(d.game / "language.txt") == "en-US");
    }
    // 3. the old build had no saves yet
    {
        Dirs d;
        write(d.game / "hookshot.exe", "old");
        d.make_staging();
        std::string err;
        CHECK(swap_in(d.staging.u8string(), d.game.u8string(), d.old.u8string(), &err));
        CHECK(!has(d.game / "user"));
        CHECK(slurp(d.game / "hookshot.exe") == "new exe");
    }
    // 4. the new build has a user folder of its own: nothing changes
    {
        Dirs d;
        d.make_old_game();
        d.make_staging();
        write(d.staging / "user" / "x.txt", "unexpected");
        std::string err;
        CHECK(!swap_in(d.staging.u8string(), d.game.u8string(), d.old.u8string(), &err));
        CHECK(!err.empty());
        CHECK(slurp(d.game / "hookshot.exe") == "old exe");
        CHECK(has(d.game / "user" / "save" / "5a3c9e1f0b7d24680000000000000001" / "account.sav"));
        CHECK(has(d.staging / "user" / "x.txt"));
    }
    // 5. a hookshot.old that holds saves is never deleted
    {
        Dirs d;
        d.make_old_game();
        d.make_staging();
        write(d.old / "user" / "precious.sav", "KEEP");
        std::string err;
        CHECK(!swap_in(d.staging.u8string(), d.game.u8string(), d.old.u8string(), &err));
        CHECK(slurp(d.old / "user" / "precious.sav") == "KEEP");
        CHECK(slurp(d.game / "hookshot.exe") == "old exe");
        CHECK(has(d.game / "user" / "save"));   // moves rolled back
        CHECK(slurp(d.game / "hookshot_pipelines.bin") == "old pipelines");
        CHECK(slurp(d.staging / "hookshot.exe") == "new exe");
        CHECK(!has(d.staging / "user"));
    }
    // 6. a leftover, empty-of-saves hookshot.old is cleared
    {
        Dirs d;
        d.make_old_game();
        d.make_staging();
        write(d.old / "stale.txt", "stale");
        std::string err;
        CHECK(swap_in(d.staging.u8string(), d.game.u8string(), d.old.u8string(), &err));
        CHECK(!has(d.old));
        CHECK(has(d.game / "user" / "save"));
    }
    // 7. a move that fails (a file in user\ is held open without delete sharing): both folders stay, nothing is lost
    {
        Dirs d;
        d.make_old_game();
        d.make_staging();
        const fs::path held = d.game / "user" / "save" / "5a3c9e1f0b7d24680000000000000001" / "account.sav";
        HANDLE h = CreateFileW(held.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        std::string err;
        const bool ok = swap_in(d.staging.u8string(), d.game.u8string(), d.old.u8string(), &err);
        CloseHandle(h);
        if (ok) {   // (a file system that lets a folder move under an open file: the saves must still have come along)
            CHECK(slurp(d.game / "user" / "save" / "5a3c9e1f0b7d24680000000000000001" / "account.sav") == "ACCOUNT");
        } else {
            CHECK(!err.empty());
            CHECK(slurp(d.game / "hookshot.exe") == "old exe");
            CHECK(slurp(d.game / "user" / "save" / "5a3c9e1f0b7d24680000000000000001" / "account.sav") == "ACCOUNT");
            CHECK(slurp(d.staging / "hookshot.exe") == "new exe");
            CHECK(!has(d.old));
        }
    }
    // 8. no staging: nothing happens
    {
        Dirs d;
        d.make_old_game();
        std::string err;
        CHECK(!swap_in(d.staging.u8string(), d.game.u8string(), d.old.u8string(), &err));
        CHECK(slurp(d.game / "hookshot.exe") == "old exe");
    }
}

static void test_recover() {
    {   // cut short after the saves moved into staging: they go back
        Dirs d;
        d.make_old_game();
        d.make_staging();
        std::error_code ec;
        fs::rename(d.game / "user", d.staging / "user", ec);
        CHECK(!ec);
        CHECK(!recover_swap(d.staging.u8string(), d.game.u8string(), d.old.u8string()).empty());
        CHECK(has(d.game / "user" / "save" / "5a3c9e1f0b7d24680000000000000001" / "account.sav"));
        CHECK(!has(d.staging / "user"));
    }
    {   // cut short between the two renames: the old build comes back, with the saves
        Dirs d;
        d.make_old_game();
        d.make_staging();
        std::error_code ec;
        fs::rename(d.game / "user", d.staging / "user", ec);
        fs::rename(d.game, d.old, ec);
        CHECK(!ec);
        CHECK(!recover_swap(d.staging.u8string(), d.game.u8string(), d.old.u8string()).empty());
        CHECK(slurp(d.game / "hookshot.exe") == "old exe");
        CHECK(has(d.game / "user" / "save" / "5a3c9e1f0b7d24680000000000000001" / "account.sav"));
    }
    {   // nothing wrong: nothing done
        Dirs d;
        d.make_old_game();
        d.make_staging();
        CHECK(recover_swap(d.staging.u8string(), d.game.u8string(), d.old.u8string()).empty());
        CHECK(has(d.game / "user"));
    }
    {   // the game's user\ already exists: staging\user is never put on top of it
        Dirs d;
        d.make_old_game();
        d.make_staging();
        write(d.staging / "user" / "other.sav", "OTHER");
        recover_swap(d.staging.u8string(), d.game.u8string(), d.old.u8string());
        CHECK(has(d.staging / "user" / "other.sav"));
    }
}

/* ---- the process runner: output, exit code, pause (suspends the whole job), cancel ---- */
static void test_runner() {
    const std::string ps = R"(C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe)";
    if (!has(ps)) { std::printf("SKIP runner tests (no powershell.exe)\n"); return; }
    std::string err;
    {   // lines and the exit code
        std::vector<std::string> lines;
        int code = run_command(ps, {"-NoProfile", "-NonInteractive", "-Command", "'@@stage a b'; 'plain'; exit 3"}, "", nullptr, nullptr,
                               [&](const std::string& l) { lines.push_back(l); }, &err);
        CHECK_EQ(code, 3);
        CHECK_EQ(lines.size(), (size_t)2);
        if (lines.size() == 2) { CHECK_EQ(lines[0], std::string("@@stage a b")); CHECK_EQ(lines[1], std::string("plain")); }
        CHECK_EQ(run_command(R"(C:\no\such\thing.exe)",{}, "", nullptr, nullptr, nullptr, &err), -1);
        CHECK(!err.empty());
    }
    {   // pause: a program that prints a line every 100 ms stops printing while paused, and carries on afterwards
        std::atomic<int> n{0};
        std::atomic<bool> pause{false};
        std::thread driver([&] {
            while (n < 5) Sleep(20);
            pause = true;
            Sleep(400);                 // lines already in the pipe may still arrive
            const int at = n;
            Sleep(1500);
            CHECK(n.load() <= at);      // nothing new while suspended
            pause = false;
        });
        int code = run_command(ps, {"-NoProfile", "-NonInteractive", "-Command", "1..25 | ForEach-Object { 'line ' + $_; Start-Sleep -Milliseconds 100 }"}, "",
                               nullptr, &pause, [&](const std::string& l) { if (l.rfind("line ", 0) == 0) ++n; }, &err);
        driver.join();
        CHECK_EQ(code, 0);
        CHECK_EQ(n.load(), 25);
    }
    {   // cancel: a long sleep ends within seconds
        std::atomic<bool> cancel{false};
        std::thread driver([&] { Sleep(1500); cancel = true; });
        const DWORD t0 = GetTickCount();
        int code = run_command(ps, {"-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 120"}, "", &cancel, nullptr, nullptr, &err);
        driver.join();
        CHECK_EQ(code, -2);
        CHECK(GetTickCount() - t0 < 15000);
    }
}

/* The tar guard lives in data\hookshot\unpack_tar.py (--selftest): run it when a Python 3 is at hand */
static void test_unpack_selftest() {
#ifdef HOOKSHOT_DATA_DIR
    wchar_t found[MAX_PATH] = {0};
    const wchar_t* cand = _wgetenv(L"HOOKSHOT_TEST_PYTHON");
    std::wstring py = cand ? cand : L"";
    if (py.empty() && SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, found, nullptr)) py = found;
    if (py.empty()) { std::printf("SKIP unpack_tar.py --selftest (no python.exe; set HOOKSHOT_TEST_PYTHON)\n"); return; }
    const std::wstring cmd = L"\"" + py + L"\" -I \"" + (fs::path(HOOKSHOT_DATA_DIR) / "unpack_tar.py").wstring() + L"\" --selftest";
    std::wstring c = cmd;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { std::printf("SKIP unpack_tar.py --selftest (python did not start)\n"); return; }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code == 9009) { std::printf("SKIP unpack_tar.py --selftest (python is only a stub)\n"); return; }
    CHECK_EQ((int)code, 0);
#endif
}

/* ---- manual smoke test ---- */
static int smoke_kit(const std::string& data, const std::string& sdl, const std::string& kit) {
    KitIO io;
    io.kit = kit;
    io.data = data;
    io.sdl2_dll = sdl;
    std::string last;
    io.report = [&](const std::string& id, const std::string& text, float p) {
        const std::string line = id + " | " + text;
        if (line != last) { std::printf("[%s] %.0f%%\n", line.c_str(), p * 100.f); last = line; }
    };
    io.log = [](const std::string& l) { std::printf("   %s\n", l.c_str()); };
    std::string err;
    const bool ok = kit_prepare(io, &err);
    std::printf("kit_prepare: %s %s\n", ok ? "OK" : "FAILED", err.c_str());
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 5 && std::string(argv[1]) == "--kit") return smoke_kit(argv[2], argv[3], argv[4]);
    test_protocol();
    test_jobs();
    test_hashes();
    test_pins();
    test_languages();
    test_game_source();
    test_problems();
    test_overall();
    test_swap();
    test_recover();
    test_runner();
    test_unpack_selftest();
    std::printf("hookshot build test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
