/* hubattach_test (online, LOCAL wrangler only): attachment round trip through the real Worker.
 *   RETROHUB_TEST_SERVER=ws://127.0.0.1:8772 hubattach_test      (refuses the production server like hubfriends_test)
 * Three throwaway "qa-" accounts: A sends files to its friend B; C is A's friend too but not a recipient. */
#include "attach_xfer.h"
#include "friends.h"
#include "net_internal.h"
#include "session.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <thread>

using namespace hubnet;
namespace fs = std::filesystem;
namespace at = hub::attach;

static int g_fail = 0;
#define CHECK(c, ...) do { if (c) printf("  ok   %s\n", #c); else { g_fail++; printf("  FAIL %s  ", #c); printf("" __VA_ARGS__); printf("\n"); } } while (0)

struct Peer {
    std::string name, dir, vault;
    FriendsClient c;
    std::vector<Event> log;
    void pump() { c.update(); Event e; while (c.poll_event(&e)) log.push_back(e); }
    const Event* find(Event::Kind k) const { for (const Event& e : log) if (e.kind == k) return &e; return nullptr; }
    void clear() { log.clear(); }
};
static std::vector<Peer*> g_all;
static bool wait_for(const std::function<bool()>& pred, int ms = 8000) {
    uint32_t t0 = now_ms();
    while (now_ms() - t0 < (uint32_t)ms) {
        for (Peer* p : g_all) p->pump();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
static std::unique_ptr<Peer> make_peer(const std::string& root, const std::string& name) {
    auto p = std::make_unique<Peer>();
    p->name = name; p->dir = root + "/" + name; p->vault = root + "/" + name + "-vault";
    fs::create_directories(p->dir); fs::create_directories(p->vault);
    p->c.set_paths(p->dir, p->vault);
    return p;
}
static std::string g_base;
static void act_as(Peer& p) {
    std::string uid = p.c.my_uid(), secret = p.c.my_secret(), base = g_base;
    at::set_account_provider([=] { at::Account a; a.base_url = base; a.user_id = uid; a.secret = secret; a.device_id = ""; return a; });
}
static std::string write_file(const std::string& path, const std::string& bytes) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path, std::ios::binary); f.write(bytes.data(), (std::streamsize)bytes.size());
    return path;
}
static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::string rnd_bytes(size_t n, unsigned seed) {
    std::mt19937 g(seed); std::string s(n, 0);
    for (char& c : s) c = (char)g();
    return s;
}

int main(int argc, char** argv) {
    std::string server = hubnet::test_server_or_exit(argc, argv);
    g_base = at::http_base_from_server(server);
    printf("[online] %s -> %s\n", server.c_str(), g_base.c_str());
    std::string root = (fs::temp_directory_path() / ("hubattach_" + std::to_string(now_ms()))).string();
    auto A = make_peer(root, "alice"), B = make_peer(root, "bob"), C = make_peer(root, "carol");
    g_all = {A.get(), B.get(), C.get()};
    A->c.start(server, "qa-alice"); B->c.start(server, "qa-bob"); C->c.start(server, "qa-carol");
    bool up = wait_for([&] { return A->c.connected() && B->c.connected() && C->c.connected(); });
    CHECK(up);
    if (!up) { printf("FAILED\n"); return 1; }
    A->c.add_friend(B->c.my_code());
    CHECK(wait_for([&] { return B->find(Event::RequestReceived); }));
    B->c.accept(A->c.my_uid());
    CHECK(wait_for([&] { return A->c.find_friend(B->c.my_uid()) && B->c.find_friend(A->c.my_uid()); }));
    A->c.add_friend(C->c.my_code());
    CHECK(wait_for([&] { return C->find(Event::RequestReceived); }));
    C->c.accept(A->c.my_uid());
    CHECK(wait_for([&] { return A->c.find_friend(C->c.my_uid()) != nullptr; }));

    printf("[round trip]\n");
    std::string png = rnd_bytes(123457, 1);
    std::string src = write_file(root + "/src/My Shot.png", png);
    act_as(*A);
    int j = at::upload(src, "shot", "My Shot.png", {B->c.my_uid()});
    at::Result r;
    CHECK(at::wait(j, &r) && r.ok, "upload: %s", r.error.c_str());
    CHECK(r.ref.id.size() == 32 && r.ref.size == (int64_t)png.size() && r.ref.kind == "shot" && r.ref.name == "My Shot.png" && r.ref.sha.size() == 64);
    int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    CHECK(r.ref.expires > now + 6LL * 86400000 && r.ref.expires < now + 8LL * 86400000, "7 day expiry");
    hubnet::Attach ref = r.ref;
    act_as(*B);
    std::string dest = root + "/recv/got.png";
    j = at::download(ref, dest);
    CHECK(at::wait(j, &r) && r.ok, "download: %s", r.error.c_str());
    CHECK(read_file(dest) == png, "bytes identical");
    CHECK(!fs::exists(dest + ".part"), "no partial file left");
    printf("[non-recipient and sender]\n");
    act_as(*C);
    j = at::download(ref, root + "/recv/c.png");
    CHECK(at::wait(j, &r) && !r.ok && r.http_status == 404 && !r.error.empty() && !fs::exists(root + "/recv/c.png"), "%s", r.error.c_str());
    printf("    -> \"%s\"\n", r.error.c_str());
    printf("[friends only]\n");
    act_as(*B);
    j = at::upload(src, "shot", "x.png", {C->c.my_uid()});   // B and C are not friends
    CHECK(at::wait(j, &r) && !r.ok && r.http_status == 403 && r.error.find("friends") != std::string::npos, "%s", r.error.c_str());
    printf("[dedupe]\n");
    act_as(*A);
    j = at::upload(src, "shot", "My Shot.png", {C->c.my_uid()});
    CHECK(at::wait(j, &r) && r.ok && r.ref.id == ref.id, "same file to another friend reuses the object: %s", r.error.c_str());
    printf("[size caps]\n");
    std::string big = write_file(root + "/src/big.state", rnd_bytes(8 * 1024 * 1024 + 1, 2));
    j = at::upload(big, "state", "big.state", {B->c.my_uid()});
    CHECK(at::wait(j, &r) && !r.ok && r.http_status == 0 && r.error.find("8 MB") != std::string::npos, "client refuses 8 MB + 1: %s", r.error.c_str());
    std::string full = write_file(root + "/src/full.state", rnd_bytes(8 * 1024 * 1024, 3));
    j = at::upload(full, "state", "full.state", {B->c.my_uid()});
    CHECK(at::wait(j, &r, 120000) && r.ok && r.ref.size == 8 * 1024 * 1024, "exactly 8 MB goes through: %s", r.error.c_str());
    hubnet::Attach fr = r.ref;
    act_as(*B);
    j = at::download(fr, root + "/recv/full.state");
    CHECK(at::wait(j, &r, 120000) && r.ok && read_file(root + "/recv/full.state").size() == 8u * 1024 * 1024, "8 MB downloads: %s", r.error.c_str());
    printf("[damaged download is rejected]\n");
    hubnet::Attach bad = ref; bad.sha = std::string(64, '0');
    j = at::download(bad, root + "/recv/bad.png");
    CHECK(at::wait(j, &r) && !r.ok && !fs::exists(root + "/recv/bad.png") && r.error.find("damaged") != std::string::npos, "%s", r.error.c_str());

    A->c.stop(); B->c.stop(); C->c.stop();
    std::error_code ec; fs::remove_all(root, ec);
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
