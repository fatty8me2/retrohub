// Save-sync tests.
//   n64run_save_sync_test                 simulated lossy network (no server needed)
//   n64run_save_sync_test wss://HOST     also: two real hubnet::Sessions through the live server
// The simulated part covers loss, duplication, reordering, corrupted DATA and the limits.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "save_sync.h"
#include "session.h"

using namespace n64run;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static fs::path temp_root() {
    fs::path p = fs::temp_directory_path() / "n64run_save_sync_test";
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

static std::vector<SaveFile> make_files(uint32_t seed) {
    std::mt19937 rng(seed);
    const std::pair<const char*, size_t> spec[] = {
        {"Game.eep", 512}, {"Game2.eep", 2048}, {"Game.sra", 32768}, {"Game.fla", 131072}, {"Pak.mpk", 32768}, {"odd.sra", 1234},
    };
    std::vector<SaveFile> out;
    for (auto& s : spec) {
        SaveFile f;
        f.name = s.first;
        f.data.resize(s.second);
        for (auto& b : f.data) b = (uint8_t)rng();
        out.push_back(std::move(f));
    }
    return out;
}

static bool same_files(const fs::path& dir, const std::vector<SaveFile>& want) {
    for (auto& f : want) {
        std::ifstream in(dir / f.name, std::ios::binary);
        if (!in) return false;
        std::vector<uint8_t> got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (got != f.data) return false;
    }
    return true;
}

// ------------------------------------------------------------------ limits and names
static void test_names_and_limits() {
    CHECK(valid_save_name("Game.eep"), "eep valid");
    CHECK(valid_save_name("GAME.FLA"), "upper-case ext valid");
    CHECK(!valid_save_name("../x.eep"), "path traversal rejected");
    CHECK(!valid_save_name("a\\b.eep"), "backslash rejected");
    CHECK(!valid_save_name("notes.txt"), "non-battery ext rejected");
    CHECK(!valid_save_name(std::string(70, 'a') + ".eep"), "long name rejected");

    fs::path d = temp_root() / "vault";
    fs::create_directories(d);
    std::ofstream(d / "ok.eep", std::ios::binary) << std::string(512, 'x');
    std::ofstream(d / "huge.fla", std::ios::binary) << std::string(kSaveMaxFile + 1, 'y');
    std::ofstream(d / "readme.txt", std::ios::binary) << "hello";
    std::ofstream(d / "empty.sra", std::ios::binary) << "";
    std::vector<SaveFile> got;
    std::vector<std::string> skipped;
    CHECK(load_save_dir(d.string(), &got, &skipped), "load_save_dir");
    CHECK(got.size() == 1 && got[0].name == "ok.eep", "only ok.eep loaded (got %zu)", got.size());
    CHECK(skipped.size() == 2 && skipped[0] == "empty.sra" && skipped[1] == "huge.fla", "empty.sra and huge.fla skipped (got %zu)", skipped.size());
    CHECK(!load_save_dir((d / "missing").string(), &got, &skipped), "missing dir reports failure");
}

// Crafted HDR with a traversal name must fail the receiver, not write outside the dir.
static void test_hostile_hdr() {
    fs::path dir = temp_root() / "hostile";
    SaveReceiver rcv(1, 2, dir.string());
    std::string name = "../evil.eep";
    std::vector<uint8_t> h{kPktSaveHdr, 0, 1, (uint8_t)name.size()};
    h.insert(h.end(), name.begin(), name.end());
    for (int i = 0; i < 4; i++) h.push_back(i == 0 ? 0x10 : 0);   // size 16
    for (int i = 0; i < 8; i++) h.push_back(0);                   // hash
    rcv.on_packet(0, h.data(), h.size());
    CHECK(rcv.failed(), "traversal HDR must fail the receiver");
    CHECK(!fs::exists(dir.parent_path() / "evil.eep"), "nothing written outside the dir");
}

// ------------------------------------------------------------------ simulated network
struct Dgram { uint32_t due; int to; int from; std::vector<uint8_t> bytes; };

static void run_sim(const char* name, int clients, double loss, double dup, uint32_t max_jitter_ms, double corrupt, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> U(0, 1);
    auto files = make_files(seed);
    fs::path root = temp_root() / name;
    const int slots = clients + 1;
    uint32_t now = 1000;
    SaveSender snd(files, slots, now, 60000);
    std::vector<std::unique_ptr<SaveReceiver>> rcv;
    std::vector<fs::path> dirs;
    for (int c = 1; c <= clients; c++) {
        dirs.push_back(root / ("slot" + std::to_string(c)));
        rcv.emplace_back(new SaveReceiver(c, slots, dirs.back().string()));
    }
    std::vector<Dgram> inflight;
    long sent = 0, dropped = 0, corrupted = 0;
    auto push = [&](int to, int from, const std::vector<uint8_t>& m, bool is_data) {
        sent++;
        if (U(rng) < loss) { dropped++; return; }
        std::vector<uint8_t> copy = m;
        if (is_data && U(rng) < corrupt && copy.size() > 4) { copy[copy.size() / 2] ^= 0x5A; corrupted++; }
        int copies = U(rng) < dup ? 2 : 1;
        for (int k = 0; k < copies; k++)
            inflight.push_back({now + (uint32_t)(rng() % (max_jitter_ms + 1)), to, from, copy});
    };
    SendFn host_send = [&](const std::vector<uint8_t>& m) {
        for (int c = 1; c <= clients; c++) push(c, 0, m, m[0] == kPktSaveData);
    };
    std::vector<SendFn> client_send;
    for (int c = 1; c <= clients; c++) client_send.push_back([&, c](const std::vector<uint8_t>& m) { push(0, c, m, false); });

    uint32_t t_end = now + 600000;
    bool all_fin = false;
    while (now < t_end) {
        // deliver due datagrams (shuffled order by swap-with-back)
        for (size_t i = 0; i < inflight.size();) {
            if (inflight[i].due <= now) {
                Dgram d = std::move(inflight[i]);
                inflight[i] = std::move(inflight.back());
                inflight.pop_back();
                if (d.to == 0) snd.on_packet(d.from, d.bytes.data(), d.bytes.size());
                else rcv[d.to - 1]->on_packet(0, d.bytes.data(), d.bytes.size());
            } else i++;
        }
        snd.tick(now, host_send);
        for (int c = 1; c <= clients; c++) rcv[c - 1]->tick(now, client_send[c - 1]);
        all_fin = snd.finished();
        for (auto& r : rcv) all_fin = all_fin && r->finished();
        if (all_fin) break;
        if (snd.failed()) break;
        for (auto& r : rcv) if (r->failed()) break;
        now++;
    }
    CHECK(!snd.failed(), "%s: sender failed: %s", name, snd.error().c_str());
    CHECK(all_fin, "%s: did not finish (sender verified=%d)", name, (int)snd.all_verified());
    for (int c = 0; c < clients; c++) {
        CHECK(!rcv[c]->failed(), "%s: receiver %d failed: %s", name, c + 1, rcv[c]->error().c_str());
        CHECK(same_files(dirs[c], files), "%s: receiver %d files differ from host", name, c + 1);
    }
    std::printf("%-36s clients=%d loss=%2.0f%% dup=%2.0f%% jitter<=%ums corrupt=%2.0f%%: %s  "
                "%ld datagrams (%ld dropped, %ld corrupted), %u ms simulated, %zu bytes of save data sent\n",
                name, clients, loss * 100, dup * 100, max_jitter_ms, corrupt * 100,
                all_fin ? "OK" : "NOT FINISHED", sent, dropped, corrupted, now - 1000, snd.bytes_sent());
}

// ------------------------------------------------------------------ live server
static bool live_test(const std::string& server) {
    auto files = make_files(99);
    fs::path cdir = temp_root() / "live_client";
    std::string room = hubnet::new_room_code() + "-7";
    hubnet::Session host, client;
    hubnet::SessionConfig hc; hc.server = server; hc.room = room; hc.slot = 0; hc.expected = 2;
    hubnet::SessionConfig cc = hc; cc.slot = 1;
    if (!host.start(hc) || !client.start(cc)) { std::printf("live: session start failed\n"); return false; }
    uint32_t t0 = hubnet::now_ms();
    while (!(host.ready() && client.ready())) {
        host.update(); client.update();
        if (host.failed() || client.failed() || hubnet::now_ms() - t0 > 30000) {
            std::printf("live: not ready (host '%s', client '%s')\n", host.status().c_str(), client.status().c_str());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::string e_host, e_client;
    bool ok_host = false, ok_client = false;
    std::thread th([&] { ok_host = run_save_sync(host, 0, 2, &files, "", 20000, &e_host); });
    std::thread tc([&] { ok_client = run_save_sync(client, 1, 2, nullptr, cdir.string(), 20000, &e_client); });
    th.join();
    tc.join();
    CHECK(ok_host, "live: host sync failed: %s", e_host.c_str());
    CHECK(ok_client, "live: client sync failed: %s", e_client.c_str());
    bool same = same_files(cdir, files);
    CHECK(same, "live: client files differ from host");
    std::printf("live server (%s, relay=%s): host %s, client %s, files %s, %u ms\n", server.c_str(),
                getenv("HUBNET_FORCE_RELAY") ? "forced" : "auto",
                ok_host ? "ok" : e_host.c_str(), ok_client ? "ok" : e_client.c_str(), same ? "identical" : "DIFFER",
                hubnet::now_ms() - t0);
    return ok_host && ok_client && same;
}

int main(int argc, char** argv) {
    test_names_and_limits();
    test_hostile_hdr();
    run_sim("1 client, clean", 1, 0.0, 0.0, 0, 0.0, 1);
    run_sim("1 client, 30% loss, jitter", 1, 0.30, 0.10, 20, 0.0, 2);
    run_sim("3 clients, 15% loss, reorder", 3, 0.15, 0.05, 15, 0.0, 3);
    run_sim("3 clients, 30% loss, corrupt 5%", 3, 0.30, 0.10, 20, 0.05, 4);
    run_sim("3 clients, 40% loss (stress)", 3, 0.40, 0.10, 30, 0.02, 5);
    if (argc > 1) {
        if (!live_test(argv[1])) g_fail++;
    }
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
