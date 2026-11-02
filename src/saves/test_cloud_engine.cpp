/* Console test for the cloud saves v2 engine: pure rules (classification, three-way, wire parsing, queue/backoff, recovery code)
 * and end-to-end scenarios against an in-process mock Worker on loopback (two "PCs" = two scratch data/vault dirs, one account).
 * Loopback only; never touches the network, the user's vault or identity. */
#include "cloud_internal.h"
#include "cloud_http.h"
#include "cloud_mock.h"
#include "savestore.h"
#include "prefs.h"
#include "library.h"
#include "json.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace hub::saves;
using str = std::string;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static str rnd(size_t n, unsigned seed) {   // incompressible bytes
    str s(n, 0);
    unsigned x = seed * 2654435761u + 12345u;
    for (size_t i = 0; i < n; i++) { x = x * 1664525u + 1013904223u; s[i] = (char)(x >> 24); }
    return s;
}
static void put(const fs::path& p, const str& c, int64_t mtime = 0) {
    fs::create_directories(p.parent_path());
    { std::ofstream o(p, std::ios::binary | std::ios::trunc); o << c; }
    if (mtime) set_file_mtime_unix(p.u8string(), mtime);
}
static str get(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    std::stringstream s; s << i.rdbuf();
    return s.str();
}
static str b32(size_t n, unsigned seed) {
    static const char* a = "abcdefghijklmnopqrstuvwxyz234567";
    str s;
    unsigned x = seed * 2246822519u + 7u;
    for (size_t i = 0; i < n; i++) { x = x * 1664525u + 1013904223u; s += a[(x >> 24) & 31]; }
    return s;
}
static int64_t now_unix() { return (int64_t)time(nullptr); }
static int64_t ms() { return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

/* ---------------------------------------------------------------- pure rules */
static void test_classify() {
    CHECK(classify_path("meta.json", 100) == FileClass::Settings);
    CHECK(classify_path("_account/prefs.json", 100) == FileClass::Settings);
    CHECK(classify_path("battery/ssb64.sra", 32768) == FileClass::Battery);
    CHECK(classify_path("battery/Super Mario 64.eep", 512) == FileClass::Battery);
    CHECK(classify_path("battery/game.srm", 131072) == FileClass::Battery);
    CHECK(classify_path("battery/card1.mcd", 131072) == FileClass::Card);
    CHECK(classify_path("battery/Memory Card 1.ps2", 8 << 20) == FileClass::Card);
    CHECK(classify_path("battery/01-GALE-Data.gci", 8192) == FileClass::Card);
    CHECK(classify_path("states/slot0.state", 2 << 20) == FileClass::State);
    CHECK(classify_path("states/auto.state", 1000) == FileClass::State);
    CHECK(classify_path("states/slot0.png", 1000) == FileClass::Shot);
    CHECK(classify_path("states/slot0.state.png", 1000) == FileClass::Shot);
    CHECK(classify_path("cheat_backups/20250101/save.sra", 100) == FileClass::Battery);
    CHECK(is_protected(FileClass::Settings, 10) && is_protected(FileClass::Battery, 10 << 20));
    CHECK(is_protected(FileClass::Card, 512 * 1024) && !is_protected(FileClass::Card, 512 * 1024 + 1));
    CHECK(!is_protected(FileClass::State, 10) && !is_protected(FileClass::Shot, 10));
    CHECK(str(class_name(FileClass::Card)) == "card" && class_from_name("shot") == FileClass::Shot);
#ifdef CLOUD_CLASSIFY_CASES
    std::ifstream f(CLOUD_CLASSIFY_CASES);   // golden table shared with the Worker lane (optional until it lands): "path size class"
    str line;
    int n = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        str path, cls; long long size = 0;
        if (!(ls >> path >> size >> cls)) continue;
        if (str(class_name(classify_path(path, size))) != cls) printf("  golden mismatch: '%s' %lld want %s\n", path.c_str(), size, cls.c_str());
        CHECK(str(class_name(classify_path(path, size))) == cls);
        n++;
    }
    printf("  golden classify cases: %d\n", n);
#endif
}

static Side S(const char* sha, int64_t mtime = 100) { Side s; s.present = sha[0] != 0; s.sha = sha; s.mtime = mtime; return s; }
static void test_three_way() {
    const FileClass B = FileClass::Battery;
    CHECK(three_way(S("a"), S("a"), S("a"), false, B) == Act::Nothing);
    CHECK(three_way(S("b"), S("a"), S("a"), false, B) == Act::Push);
    CHECK(three_way(S("a"), S("a"), S("b"), false, B) == Act::Pull);
    CHECK(three_way(S("c"), S("a"), S("c"), false, B) == Act::Adopt);
    CHECK(three_way(S("b"), S("a"), S("c"), false, B) == Act::Conflict);
    /* the custom cover (game root cover.png) is settings-class: both PCs edited it, newest wins, never a prompt */
    const FileClass CV = classify_path("cover.png", 40960);
    CHECK(CV == FileClass::Settings);
    CHECK(three_way(S("b", 300), S("a"), S("c", 200), false, CV) == Act::Push);
    CHECK(three_way(S("b", 100), S("a"), S("c", 200), false, CV) == Act::Pull);
    CHECK(three_way(S(""), S("a"), S("a"), false, B) == Act::Pull);                 // absent locally, present in cloud: heal
    CHECK(three_way(S(""), S(""), S("a"), false, B) == Act::Pull);                  // new game on this PC
    CHECK(three_way(S("a"), S(""), S(""), false, B) == Act::Push);                  // present, cloud absent, base absent
    CHECK(three_way(S("a"), S(""), S("a"), false, B) == Act::Adopt);                // first run, same bytes
    CHECK(three_way(S("a"), S(""), S("b"), false, B) == Act::Conflict);             // first run, different bytes
    CHECK(three_way(S("a"), S("a"), S(""), true, FileClass::State) == Act::LocalOnlyCap);
    CHECK(three_way(S(""), S("a"), S(""), true, FileClass::State) == Act::Nothing);
    CHECK(three_way(S(""), S("a"), S(""), false, B) == Act::Nothing);               // never delete anything
    /* states / shots / settings never prompt: newest mtime wins */
    CHECK(three_way(S("b", 200), S("a"), S("c", 100), false, FileClass::State) == Act::Push);
    CHECK(three_way(S("b", 100), S("a"), S("c", 200), false, FileClass::State) == Act::Pull);
    CHECK(three_way(S("b", 100), S("a"), S("c", 200), false, FileClass::Settings) == Act::Pull);
    CHECK(three_way(S("b", 100), S("a"), S("c", 200), false, FileClass::Shot) == Act::Pull);
    CHECK(three_way(S("b", 100), S("a"), S("c", 200), false, FileClass::Card) == Act::Conflict);
}

static void test_wire() {
    str man = str(
        "{\"ok\":true,\"rev\":412,\"full\":false,\"account\":{\"used\":8123456,\"protUsed\":1204433,\"cap\":52428800,\"protCap\":20971520,"
        "\"devices\":2,\"pool\":\"tight\",\"paused\":false},\"games\":{\"n64/smash-64\":{\"playedAt\":1760000000,\"device\":\"PC-DESK\","
        "\"pin\":true,\"rev\":411,\"lease\":{\"dev\":\"d-ab12cd34\",\"name\":\"PC-LIVING\",\"until\":1760000900},\"files\":{"
        "\"battery/ssb64.sra\":{\"cur\":7,\"cloud\":\"synced\",\"vers\":[{\"ver\":7,\"sha\":\"" + str(64, 'a') + "\",\"size\":32768,\"stored\":1210,"
        "\"enc\":\"deflate\",\"mtime\":1760000000,\"dev\":\"d-ab12cd34\",\"devName\":\"PC-DESK\",\"ts\":1760000010,\"class\":\"battery\"}]},"
        "\"states/s0.state\":{\"cur\":2,\"cloud\":\"evicted\",\"vers\":[]},\"../evil\":{\"cur\":1,\"vers\":[]}}}}}");
    WManifest m; str err;
    CHECK(parse_manifest(man, &m, &err));
    CHECK(m.rev == 412 && !m.full && m.account.devices == 2 && m.account.pool == "tight" && m.account.cap == 52428800);
    CHECK(m.games.size() == 1 && m.games[0].gkey == "n64/smash-64" && m.games[0].pin && m.games[0].lease.present && m.games[0].lease.name == "PC-LIVING");
    CHECK(m.games[0].files.size() == 2);                         // "../evil" is dropped
    const WFile* f = m.games[0].find("battery/ssb64.sra");
    CHECK(f && f->cur == 7 && f->current() && f->current()->enc == "deflate" && f->current()->stored == 1210 && f->current()->dev_name == "PC-DESK");
    const WFile* e = m.games[0].find("states/s0.state");
    CHECK(e && e->evicted() && e->vers.empty());
    CHECK(!parse_manifest("{not json", &m, &err));
    CHECK(!parse_manifest("{\"ok\":false,\"error\":\"nope\"}", &m, &err));

    std::vector<PlanFile> pf(1);
    pf[0].path = "battery/a.sra"; pf[0].sha = str(64, 'b'); pf[0].size = 10; pf[0].stored = 8; pf[0].enc = "deflate"; pf[0].mtime = 5; pf[0].cls = "battery"; pf[0].base = 3;
    str body = build_plan_body("n64/x", 99, "rw", pf);
    json::Value j;
    CHECK(json::parse(body, &j) && j.str("game") == "n64/x" && j.i64("played_at") == 99 && j.get("files")->a.size() == 1 &&
          j.get("files")->a[0].i64("base") == 3 && j.get("files")->a[0].str("class") == "battery");
    PlanReply pr;
    CHECK(parse_plan("{\"ok\":true,\"ticket\":\"T1\",\"files\":[{\"path\":\"a\",\"action\":\"upload\"},{\"path\":\"b\",\"action\":\"skip\",\"reason\":\"over-cap\"},"
                     "{\"path\":\"c\",\"action\":\"conflict\",\"current\":{\"ver\":4,\"sha\":\"" + str(64, 'c') + "\",\"mtime\":7}}]}", &pr, &err));
    CHECK(pr.ticket == "T1" && pr.files.size() == 3 && pr.files[1].reason == "over-cap" && pr.files[2].has_current && pr.files[2].current.ver == 4);
    CommitReply cr;
    CHECK(parse_commit("{\"ok\":true,\"rev\":9,\"results\":[{\"path\":\"a\",\"ver\":2,\"cloud\":\"synced\"},{\"path\":\"b\",\"conflict\":true}],"
                       "\"evicted\":[{\"game\":\"n64/y\",\"path\":\"states/s\"}]}", &cr, &err));
    CHECK(cr.rev == 9 && cr.results.size() == 2 && cr.results[0].ver == 2 && cr.results[1].conflict && cr.evicted.size() == 1 && cr.evicted[0].first == "n64/y");
    LeaseReply lr;
    CHECK(parse_lease("{\"ok\":false,\"held\":{\"dev\":\"d-1\",\"name\":\"PC\",\"since\":5,\"until\":9}}", &lr) && lr.held && lr.name == "PC" && lr.until == 9);
    ServerError se = parse_error("{\"ok\":false,\"error\":\"account-daily-limit\",\"retryAfter\":3600}");
    CHECK(se.code == "account-daily-limit" && se.retry_after == 3600);
}

/* Per-process scratch root: parallel ctest runs (and stale dirs from crashed runs) must never share paths. */
static fs::path scratch_root() {
    static const fs::path r = fs::temp_directory_path() / ("rhq_cloudc_" + std::to_string((unsigned long)GetCurrentProcessId()));
    return r;
}

static void test_queue_and_base() {
    CHECK(backoff_secs(1) == 30 && backoff_secs(2) == 120 && backoff_secs(3) == 600 && backoff_secs(4) == 3600 && backoff_secs(5) == -1);
    str dir = (scratch_root() / "queue").u8string();
    fs::remove_all(dir); fs::create_directories(dir);
    str qp = dir + "\\cloud-queue.json";
    CloudQueue q;
    q.set_path(qp);
    q.put("n64/a", "push", 100);
    q.put("n64/b", "pull", 200);
    q.put("n64/a", "push", 100);                                  // duplicate: one item
    CHECK(q.size() == 2);
    QItem it;
    CHECK(q.next_due(100, &it) && it.gkey == "n64/a" && !q.next_due(99, &it));
    q.fail("n64/a", "push", 1000);
    CHECK(q.items()[0].tries == 1 && q.items()[0].next_at == 1030);
    q.fail("n64/a", "push", 1030); CHECK(q.items()[0].next_at == 1030 + 120);
    q.fail("n64/a", "push", 2000); CHECK(q.items()[0].next_at == 2000 + 600);
    q.fail("n64/a", "push", 3000); CHECK(q.items()[0].next_at == 3000 + 3600);
    q.fail("n64/a", "push", 9000); CHECK(q.items()[0].next_at == -1);   // parked: waits for a trigger
    QItem x;
    CHECK(!q.next_due(1000000, &x) || x.gkey == "n64/b");
    q.fail("n64/b", "pull", 500, 7200);                           // the server said when
    CHECK(q.items()[1].next_at == 7700);
    CHECK(q.save());
    CloudQueue q2;                                               // persisted across a restart
    CHECK(q2.load(qp) && q2.size() == 2 && q2.items()[0].tries == 5 && q2.items()[0].next_at == -1 && q2.items()[1].next_at == 7700);
    q2.wake_parked(10000);
    CHECK(q2.next_due(10000, &x) && q2.items()[0].next_at == 10000 && q2.items()[0].tries == 0);
    q2.remove("n64/a", "push");
    CHECK(q2.size() == 1);

    BaseStore b;
    b.rev = 77; b.device = "d-1"; b.restore = true; b.cap_toast_day = 5;
    b.games["n64/a"].files["battery/x.sra"] = BFile{3, 100, 55, "sha1", ""};
    b.games["n64/a"].files["states/s.state"] = BFile{1, 9, 66, "sha2", "cap"};
    b.games["n64/a"].lo_user = true; b.games["n64/a"].played = 123;
    CHECK(b.save(dir + "\\base.json"));
    BaseStore c;
    CHECK(c.load(dir + "\\base.json") && c.rev == 77 && c.restore && c.games["n64/a"].files["battery/x.sra"].ver == 3 &&
          c.games["n64/a"].files["states/s.state"].lo == "cap" && c.games["n64/a"].lo_user && c.games["n64/a"].played == 123);
    fs::remove_all(dir);
}

/* ---------------------------------------------------------------- engine scenarios */
struct Pc {
    str name;
    fs::path root, data, vault;
    explicit Pc(const str& n) : name(n) {
        root = scratch_root() / n;
        fs::remove_all(root);
        data = root / "data"; vault = root / "vault";
        fs::create_directories(data); fs::create_directories(vault);
    }
    fs::path game(const str& sys = "n64", const str& key = "smash-64") const { return vault / sys / key; }
};
static MockServer* g_srv = nullptr;
static str g_last_data;
static str g_uid, g_secret;

static bool g_expire_on_switch = true;   // switching PCs in a test = the other PC quit: its lease is released/expired
static void use(const Pc& pc, bool with_identity = true) {
    cloud_engine_stop();
    if (g_expire_on_switch) g_srv->expire_leases();
    set_vault_dir(pc.vault.u8string());
    EngineConfig ec;
    ec.data_dir = pc.data.u8string();
    ec.device_name = pc.name;
    ec.no_threads = true;
    ec.beat_ms = 200;
    cloud_engine_configure(ec);
    g_last_data = ec.data_dir;
    if (with_identity) cloud_set_identity(g_uid, g_secret);
    cloud_engine_start(g_srv->url());
}
static const GameId G{"n64", "smash-64"};
static const str GK = "n64/smash-64";

static bool pop_event(CloudEvent::Kind k, CloudEvent* out = nullptr) {
    CloudEvent e;
    bool found = false;
    std::vector<CloudEvent> keep;
    while (cloud_pop_event(&e)) {
        if (!found && e.kind == k) { found = true; if (out) *out = e; }
        else keep.push_back(e);
    }
    return found;
}

static void dump_log() {
    if (g_last_data.empty()) return;
    str l = get(fs::u8path(g_last_data) / "cloud_saves.log");
    if (!l.empty()) printf("  --- cloud_saves.log ---\n%s  ---\n", l.c_str());
}
/* Runs queued jobs. Jobs the engine deliberately delays by a second or two (e.g. a pull queued at now+1) are waited
 * for, so the result does not depend on where the wall-clock second boundary falls or on machine load (parallel
 * ctest). Long backoffs and parked items are NOT waited for - those tests assert they stay queued. */
static void pump() {
    cloud_engine_pump(200);
    for (int spin = 0; spin < 60; spin++) {                       // at most ~6 s
        str d = cloud_queue_dump();
        bool soon = false;
        size_t pos = 0;
        while (pos < d.size()) {
            size_t e = d.find(';', pos); if (e == str::npos) break;
            str item = d.substr(pos, e - pos); pos = e + 1;
            size_t c = item.rfind(':'); if (c == str::npos) continue;
            long long at = atoll(item.c_str() + c + 1);
            long long delta = at - (long long)now_unix();
            if (at > 0 && delta >= 0 && delta <= 2) soon = true;
        }
        if (!soon) break;
        Sleep(100);
        cloud_engine_pump(200);
    }
}
static void play(const GameId& g = G) { cloud_game_started(g, SyncMode::ReadWrite); }
static void exit_game(const GameId& g = G) { cloud_game_exited(g); pump(); }

static void fresh_account() {
    g_uid = b32(26, (unsigned)ms());
    g_secret = b32(52, (unsigned)ms() + 1);
    g_srv->add_account(g_uid, g_secret);
}

static void test_http_timeout() {
    printf(" http timeouts\n");
    CloudConfig cc;
    cc.base_url = g_srv->url(); cc.user_id = g_uid; cc.secret = g_secret;
    {
        CloudHttp h(cc, 1000, 1000);
        HttpReply ok = h.call("GET", "/saves/v2/status");
        CHECK(ok.status == 200);
        g_srv->delay_ms = 3500;
        CloudHttp h2(cc, 1000, 1000);
        int64_t t0 = ms();
        HttpReply r = h2.call("GET", "/saves/v2/status");
        int64_t el = ms() - t0;
        printf("  (status call with 3.5 s server delay: status=%d el=%lld)\n", r.status, (long long)el);
        CHECK(r.status == 0 && el < 2500);
        for (int k = 0; k < 2; k++) {
            CloudHttp h3(cc, 1000, 1000);
            int64_t t3 = ms();
            HttpReply rr = k ? h3.call("POST", "/saves/v2/device", {}, "{\"dev\":\"d-12345678\",\"name\":\"x\"}") : h3.call("GET", "/saves/v2/manifest");
            printf("  (%s: status=%d err=%s el=%lld)\n", k ? "POST" : "GET", rr.status, rr.error.c_str(), (long long)(ms() - t3));
        }
        g_srv->delay_ms = 0;
    }
    CloudHttp dead(CloudConfig{"http://127.0.0.1:1", g_uid, g_secret, false, "", 1}, 500, 500);
    int64_t t1 = ms();
    HttpReply d = dead.call("GET", "/saves/v2/status");
    CHECK(d.status == 0 && ms() - t1 < 3000);
}

static void test_basic_two_pcs() {
    printf(" two PCs, pull before launch, history\n");
    Pc pc1("pc1"), pc2("pc2");
    use(pc1);
    str save1 = rnd(4096, 1);
    put(pc1.game() / "battery" / "ssb64.sra", save1, now_unix() - 1000);
    put(pc1.game() / "meta.json", "{\"title\":\"Smash\"}", now_unix() - 1000);
    put(pc1.game() / "states" / "slot0.state", rnd(100000, 2), now_unix() - 1000);
    PrelaunchResult r = cloud_prelaunch(G, SyncMode::ReadWrite);
    CHECK(r.action == PrelaunchResult::Proceed && !r.offline && r.files_pulled == 0);
    play();
    put(pc1.game() / "battery" / "ssb64.sra", save1 + "x", now_unix() - 500);
    exit_game();
    str sha1 = sha256_hex_file((pc1.game() / "battery" / "ssb64.sra").u8string());
    CHECK(g_srv->cur_sha(g_uid, GK, "battery/ssb64.sra") == sha1);
    CHECK(!g_srv->cur_sha(g_uid, GK, "states/slot0.state").empty());
    CHECK(!g_srv->cur_sha(g_uid, GK, "meta.json").empty());
    GameCloudInfo gi;
    CHECK(cloud_game(G, &gi) && gi.state == CloudState::Synced && gi.last_synced > 0 && gi.cloud_bytes > 0);
    CHECK(pop_event(CloudEvent::Pushed));
    CloudAccountInfo acct = cloud_account();
    CHECK(acct.known && acct.online && acct.cap == 50ll << 20 && acct.devices == 1 && acct.queued_games == 0);
    CHECK(!cloud_take_recovery_reminder() == false);                     // first successful push, recovery code not yet saved: remind once
    CHECK(!cloud_take_recovery_reminder());

    /* PC 2: same account, nothing local. Pull before launch fetches the protected files; the state follows in the background. */
    use(pc2);
    r = cloud_prelaunch(G, SyncMode::ReadWrite);
    CHECK(r.action == PrelaunchResult::Proceed && !r.offline && r.files_pulled >= 2);
    CHECK(get(pc2.game() / "battery" / "ssb64.sra") == save1 + "x");
    CHECK(file_mtime_unix((pc2.game() / "battery" / "ssb64.sra").u8string()) == file_mtime_unix((pc1.game() / "battery" / "ssb64.sra").u8string()));
    CHECK(!fs::exists(pc2.game() / "states" / "slot0.state"));         // large items are not blocking
    CHECK(cloud_game(G, &gi) && gi.state == CloudState::Waiting);
    play();
    pump();
    CHECK(fs::exists(pc2.game() / "states" / "slot0.state"));
    CHECK(get(pc2.game() / "states" / "slot0.state") == rnd(100000, 2));
    CHECK(g_srv->devices(g_uid) >= 2);
    cloud_game_exited(G);
    pump();
    CHECK(cloud_account().devices >= 2);

    /* history: protected files keep 3 versions, large ones 1 */
    use(pc1);
    for (int i = 0; i < 4; i++) {
        PrelaunchResult rr = cloud_prelaunch(G, SyncMode::ReadWrite);
        (void)rr;
        play();
        put(pc1.game() / "battery" / "ssb64.sra", save1 + "v" + std::to_string(i), now_unix() - 400 + i * 10);
        put(pc1.game() / "states" / "slot0.state", rnd(100000, 10 + i), now_unix() - 400 + i * 10);
        exit_game();
    }
    CHECK(g_srv->versions(g_uid, GK, "battery/ssb64.sra") == 3);
    CHECK(g_srv->versions(g_uid, GK, "states/slot0.state") == 1);
    auto vers = cloud_versions(G);
    bool found = false;
    for (auto& v : vers) if (v.first == "battery/ssb64.sra") { found = true; CHECK(v.second.size() == 3 && v.second[0].ver > v.second[2].ver); }
    CHECK(found);
    /* restore the oldest retained version */
    str err;
    int oldest = 0;
    for (auto& v : vers) if (v.first == "battery/ssb64.sra") oldest = v.second.back().ver;
    CHECK(cloud_restore_version(G, "battery/ssb64.sra", oldest, &err));
    CHECK(get(pc1.game() / "battery" / "ssb64.sra") == save1 + "v1");
    CHECK(pop_event(CloudEvent::Restored));
}

static void test_offline_retry_resume() {
    printf(" offline, timeout, retry queue, resume\n");
    Pc pc1("pc1");
    use(pc1);
    put(pc1.game() / "battery" / "a.sra", rnd(2000, 5), now_unix() - 100);
    /* timeout: the manifest takes longer than the 4 s budget -> play at once, offline */
    g_srv->delay_ms = 3500;
    int64_t t0 = ms();
    PrelaunchResult r = cloud_prelaunch(G, SyncMode::ReadWrite, 800);
    int64_t el = ms() - t0;
    printf("  (timeout test: offline=%d el=%lld)\n", (int)r.offline, (long long)el);
    CHECK(r.action == PrelaunchResult::Proceed && r.offline);
    CHECK(el < 3400);
    g_srv->delay_ms = 0;
    CloudEvent ev;
    CHECK(pop_event(CloudEvent::OfflineStarted, &ev) && !ev.good && ev.text.find("Offline") == 0);
    play();
    put(pc1.game() / "battery" / "a.sra", rnd(2000, 6), now_unix() - 50);
    /* the server errors: exit queues the push, it fails and backs off, nothing blocks */
    g_srv->fail_status = 503; g_srv->fail_count = 1000;
    cloud_game_exited(G);
    pump();
    GameCloudInfo gi;
    CHECK(cloud_game(G, &gi) && gi.state == CloudState::Waiting);
    CHECK(cloud_account().queued_games == 1);
    CHECK(cloud_engine_idle());                                    // backoff: nothing due yet
    CHECK(g_srv->cur_sha(g_uid, GK, "battery/a.sra").empty());
    g_srv->fail_count = 0;
    cloud_wake_queue();                                           // a trigger (next launch / hub start / manual)
    pump();
    CHECK(g_srv->cur_sha(g_uid, GK, "battery/a.sra") == sha256_hex_file((pc1.game() / "battery" / "a.sra").u8string()));
    if (cloud_account().queued_games) printf("  queue: %s (now %lld)\n", cloud_queue_dump().c_str(), (long long)now_unix());
    CHECK(cloud_account().queued_games == 0);
    CHECK(cloud_game(G, &gi) && gi.state == CloudState::Synced);

    /* the persistent queue survives a restart */
    play();
    put(pc1.game() / "battery" / "a.sra", rnd(2000, 7), now_unix() - 20);
    g_srv->fail_status = 503; g_srv->fail_count = 1000;
    cloud_game_exited(G);
    pump();
    CHECK(cloud_account().queued_games == 1);
    use(pc1);                                                      // restart the engine ("hub restart")
    CHECK(cloud_account().queued_games == 1);
    g_srv->fail_count = 0;
    cloud_wake_queue();
    pump();
    printf("  (after restart: queue=%s sha_ok=%d)\n", cloud_queue_dump().c_str(), (int)(g_srv->cur_sha(g_uid, GK, "battery/a.sra") == sha256_hex_file((pc1.game() / "battery" / "a.sra").u8string())));
    CHECK(cloud_account().queued_games == 0 && g_srv->cur_sha(g_uid, GK, "battery/a.sra") == sha256_hex_file((pc1.game() / "battery" / "a.sra").u8string()));

    /* resumable download: the first transfer breaks half way, the next one continues from the partial file and verifies the sha */
    str big = rnd(300000, 9);
    g_srv->seed_file(g_uid, "n64/other", "battery/big.sra", big, now_unix() - 10, "PC-X", now_unix() - 10);
    Pc pc2("pc2");
    use(pc2);
    g_srv->truncate_get = 100000;
    GameId og{"n64", "other"};
    r = cloud_prelaunch(og, SyncMode::ReadWrite);
    CHECK(r.offline || r.files_pulled == 0);
    CHECK(fs::exists(pc2.game("n64", "other") / "battery" / "big.sra.dl.hubtmp"));
    CHECK(!fs::exists(pc2.game("n64", "other") / "battery" / "big.sra"));
    r = cloud_prelaunch(og, SyncMode::ReadWrite);
    CHECK(!r.offline && r.files_pulled == 1);
    CHECK(get(pc2.game("n64", "other") / "battery" / "big.sra") == big);
    CHECK(!fs::exists(pc2.game("n64", "other") / "battery" / "big.sra.dl.hubtmp"));
}

static void test_conflict() {
    printf(" conflict detection and resolution\n");
    Pc pc1("pc1"), pc2("pc2");
    use(pc1);
    put(pc1.game() / "battery" / "s.sra", rnd(3000, 1), now_unix() - 5000);
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();
    str base_sha = g_srv->cur_sha(g_uid, GK, "battery/s.sra");
    CHECK(!base_sha.empty());
    use(pc2);
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();     // pc2 now has the base
    /* both PCs change the file; pc1 uploads first */
    use(pc1);
    put(pc1.game() / "battery" / "s.sra", rnd(3000, 2), now_unix() - 3000);
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();
    str pc1_sha = g_srv->cur_sha(g_uid, GK, "battery/s.sra");
    CHECK(pc1_sha != base_sha);
    use(pc2);
    put(pc2.game() / "battery" / "s.sra", rnd(3000, 3), now_unix() - 2000);       // newer than the cloud copy
    PrelaunchResult r = cloud_prelaunch(G, SyncMode::ReadWrite);
    CHECK(r.action == PrelaunchResult::NeedConflictDecision && r.conflicts.size() == 1);
    if (!r.conflicts.empty()) {
        CHECK(r.conflicts[0].path == "battery/s.sra" && r.conflicts[0].cls == FileClass::Battery);
        CHECK(r.conflicts[0].local.present && r.conflicts[0].cloud.present && r.conflicts[0].local.sha != r.conflicts[0].cloud.sha);
        CHECK(r.conflicts[0].local.mtime > r.conflicts[0].cloud.mtime);
    }
    GameCloudInfo gi;
    CHECK(cloud_game(G, &gi) && gi.state == CloudState::Conflict && gi.conflicts.size() == 1);
    CHECK(pop_event(CloudEvent::ConflictFound));
    CHECK(get(pc2.game() / "battery" / "s.sra") == rnd(3000, 3));       // nothing overwritten before the player decides

    /* decide later: play local only, no push for that game */
    cloud_resolve_conflict(G, ConflictChoice::DecideLater);
    play();
    put(pc2.game() / "battery" / "s.sra", rnd(3000, 4), now_unix() - 1000);
    exit_game();
    CHECK(g_srv->cur_sha(g_uid, GK, "battery/s.sra") == pc1_sha);
    CHECK(cloud_game(G, &gi) && gi.state == CloudState::Conflict && gi.local_only == LocalOnlyReason::DecideLater);
    r = cloud_prelaunch(G, SyncMode::ReadWrite);                       // asked again at the next launch
    CHECK(r.action == PrelaunchResult::NeedConflictDecision);

    /* use this PC's: it becomes the cloud copy, the cloud's is kept in _conflicts */
    cloud_resolve_conflict(G, ConflictChoice::UseLocal);
    pump();
    CHECK(g_srv->cur_sha(g_uid, GK, "battery/s.sra") == sha256_hex_file((pc2.game() / "battery" / "s.sra").u8string()));
    CHECK(get(pc2.game() / "battery" / "s.sra") == rnd(3000, 4));
    int kept = 0;
    for (auto& e : fs::recursive_directory_iterator(pc2.vault / "_conflicts")) if (e.is_regular_file()) { kept++; CHECK(get(e.path()) == rnd(3000, 2)); }
    CHECK(kept == 1);
    CHECK(pop_event(CloudEvent::ConflictResolved));
    CHECK(g_srv->versions(g_uid, GK, "battery/s.sra") >= 2);          // the loser is also a cloud version

    /* use the cloud's: the local copy is kept in _conflicts */
    use(pc1);
    put(pc1.game() / "battery" / "s.sra", rnd(3000, 5), now_unix() - 100);
    use(pc2);
    put(pc2.game() / "battery" / "s.sra", rnd(3000, 6), now_unix() - 90);
    play(); exit_game();                                              // pc2 pushes first
    use(pc1);
    r = cloud_prelaunch(G, SyncMode::ReadWrite);
    printf("  (use-cloud scenario: action=%d conflicts=%d pulled=%d offline=%d)\n", (int)r.action, (int)r.conflicts.size(), r.files_pulled, (int)r.offline);
    CHECK(r.action == PrelaunchResult::NeedConflictDecision);
    cloud_resolve_conflict(G, ConflictChoice::UseCloud);
    CHECK(get(pc1.game() / "battery" / "s.sra") == rnd(3000, 6));
    int kept1 = 0;
    for (auto& e : fs::recursive_directory_iterator(pc1.vault / "_conflicts")) if (e.is_regular_file()) { kept1++; CHECK(get(e.path()) == rnd(3000, 5)); }
    CHECK(kept1 == 1);

    /* a CAS miss at push time is a conflict too (the cloud changed between the pull and the exit) */
    use(pc2);
    cloud_prelaunch(G, SyncMode::ReadWrite); play();
    put(pc2.game() / "battery" / "s.sra", rnd(3000, 7), now_unix() - 50);
    g_srv->seed_file(g_uid, GK, "battery/s.sra", rnd(3000, 8), now_unix() - 40, "PC-ELSE", now_unix());   // another PC pushed meanwhile
    exit_game();
    CHECK(cloud_game(G, &gi) && gi.state == CloudState::Conflict);
    CHECK(g_srv->cur_sha(g_uid, GK, "battery/s.sra") == sha256_hex_file((pc2.game() / "battery" / "s.sra").u8string()) == false);
}

/* A plain both-sides-edited conflict (no "decide later" involved) must be visible without launching the game:
 * the background pass flags the game as Conflict with its sides, and the next launch asks. */
static void test_plain_conflict_surfaces() {
    printf(" plain conflict surfaces in the background and at next launch\n");
    Pc pc1("pc1"), pc2("pc2");
    use(pc1);
    put(pc1.game() / "battery" / "s.sra", rnd(3000, 11), now_unix() - 5000);
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();
    use(pc2);
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();     // pc2 has the base
    use(pc1);
    put(pc1.game() / "battery" / "s.sra", rnd(3000, 12), now_unix() - 3000);
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();     // cloud moves on
    use(pc2);
    put(pc2.game() / "battery" / "s.sra", rnd(3000, 13), now_unix() - 2000);   // pc2 edited too, never launched through the hub
    cloud_sync_all_now();                                             // hub start / "Sync now": background pass
    pump();
    GameCloudInfo gi;
    CHECK(cloud_game(G, &gi) && gi.state == CloudState::Conflict && gi.local_only == LocalOnlyReason::None && gi.conflicts.size() == 1);
    CHECK(get(pc2.game() / "battery" / "s.sra") == rnd(3000, 13));      // nothing overwritten
    PrelaunchResult r = cloud_prelaunch(G, SyncMode::ReadWrite);
    CHECK(r.action == PrelaunchResult::NeedConflictDecision && r.conflicts.size() == 1);
}

static void test_states_never_prompt() {
    printf(" states: newest wins silently\n");
    Pc pc1("pc1"), pc2("pc2");
    use(pc1);
    put(pc1.game() / "states" / "slot1.state", rnd(5000, 1), now_unix() - 900);
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();
    use(pc2);
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); pump(); exit_game();
    CHECK(fs::exists(pc2.game() / "states" / "slot1.state"));
    use(pc1);
    put(pc1.game() / "states" / "slot1.state", rnd(5000, 2), now_unix() - 300);
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();
    use(pc2);
    put(pc2.game() / "states" / "slot1.state", rnd(5000, 3), now_unix() - 100);    // newer
    PrelaunchResult r = cloud_prelaunch(G, SyncMode::ReadWrite);
    CHECK(r.action == PrelaunchResult::Proceed && r.conflicts.empty());
    play(); exit_game();
    CHECK(g_srv->cur_sha(g_uid, GK, "states/slot1.state") == sha256_hex_file((pc2.game() / "states" / "slot1.state").u8string()));
}

static void test_quota() {
    printf(" quota classes, eviction, local only\n");
    Pc pc1("pc1");
    g_srv->account_cap = 300 * 1024;
    use(pc1);
    GameId a{"n64", "game-a"}, b{"n64", "game-b"};
    put(pc1.game("n64", "game-a") / "battery" / "a.sra", rnd(2048, 1), now_unix() - 900);
    put(pc1.game("n64", "game-a") / "states" / "slot0.state", rnd(200 * 1024, 2), now_unix() - 900);
    cloud_prelaunch(a, SyncMode::ReadWrite); play(a); exit_game(a);
    CHECK(!g_srv->cur_sha(g_uid, "n64/game-a", "states/slot0.state").empty());
    g_srv->set_played(g_uid, "n64/game-a", now_unix() - 5000);          // A is the least recently played
    put(pc1.game("n64", "game-b") / "battery" / "b.sra", rnd(2048, 3), now_unix() - 100);
    put(pc1.game("n64", "game-b") / "states" / "slot0.state", rnd(200 * 1024, 4), now_unix() - 100);
    cloud_prelaunch(b, SyncMode::ReadWrite); play(b); exit_game(b);
    /* B's state fits because A's older state was evicted; A's battery (protected) stays */
    CHECK(!g_srv->cur_sha(g_uid, "n64/game-b", "states/slot0.state").empty());
    CHECK(g_srv->evicted(g_uid, "n64/game-a", "states/slot0.state"));
    CHECK(!g_srv->cur_sha(g_uid, "n64/game-a", "battery/a.sra").empty());
    CHECK(fs::exists(pc1.game("n64", "game-a") / "states" / "slot0.state"));       // the local copy is untouched
    GameCloudInfo gi;
    CHECK(cloud_game(a, &gi) && gi.files_local_only == 1);
    CloudEvent ev;
    CHECK(pop_event(CloudEvent::WentLocalOnly, &ev) && ev.text.find("Cloud space is full") == 0 && ev.text.find("game-a") != str::npos && !ev.good);
    /* the evicted item is not retried while unchanged */
    int puts = g_srv->n_put;
    cloud_prelaunch(a, SyncMode::ReadWrite); play(a); exit_game(a);
    CHECK(g_srv->n_put == puts + 0 || g_srv->n_put == puts + 1);                  // only meta.json may move
    CHECK(g_srv->evicted(g_uid, "n64/game-a", "states/slot0.state"));
    /* a third game too big for the cap: its large item is local only (over-cap), its protected file still syncs */
    GameId c{"n64", "game-c"};
    put(pc1.game("n64", "game-c") / "battery" / "c.sra", rnd(2048, 5), now_unix() - 10);
    put(pc1.game("n64", "game-c") / "states" / "slot0.state", rnd(400 * 1024, 6), now_unix() - 10);
    cloud_prelaunch(c, SyncMode::ReadWrite); play(c); exit_game(c);
    CHECK(!g_srv->cur_sha(g_uid, "n64/game-c", "battery/c.sra").empty());
    CHECK(g_srv->cur_sha(g_uid, "n64/game-c", "states/slot0.state").empty());
    CHECK(cloud_game(c, &gi) && gi.files_local_only == 1 && gi.state == CloudState::Synced);
    /* a file whose stored form exceeds 16 MB is never uploaded */
    g_srv->account_cap = 100ll << 20;
    GameId d{"ps2", "big-card"};
    put(pc1.game("ps2", "big-card") / "battery" / "mc.ps2", rnd(17 << 20, 7), now_unix() - 10);
    cloud_prelaunch(d, SyncMode::ReadWrite); play(d); exit_game(d);
    CHECK(g_srv->cur_sha(g_uid, "ps2/big-card", "battery/mc.ps2").empty());
    CHECK(cloud_game(d, &gi) && gi.state == CloudState::LocalOnly && gi.local_only == LocalOnlyReason::TooLarge);
    g_srv->account_cap = 50ll << 20;
}

static void test_lease_and_readonly() {
    g_expire_on_switch = false;
    printf(" other-PC lease, read-only sessions\n");
    Pc pc1("pc1"), pc2("pc2");
    use(pc1);
    put(pc1.game() / "battery" / "l.sra", rnd(500, 1), now_unix() - 100);
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();
    use(pc2);
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();       // 2 devices now: leases apply
    CHECK(cloud_account().devices >= 2);
    use(pc1);
    int leases = g_srv->n_lease;
    PrelaunchResult r = cloud_prelaunch(G, SyncMode::ReadWrite);
    CHECK(r.action == PrelaunchResult::Proceed && g_srv->n_lease == leases + 1);
    play();                                                          // pc1 holds the lease and heartbeats
    use(pc2);
    r = cloud_prelaunch(G, SyncMode::ReadWrite);
    CHECK(r.action == PrelaunchResult::NeedLeaseDecision && r.held_by == "pc1" && r.held_since > 0);
    GameCloudInfo gi;
    CHECK(cloud_game(G, &gi) && gi.state == CloudState::PlayingElsewhere && gi.playing_on == "pc1");
    /* a read-only (online guest) session never takes a lease */
    int l2 = g_srv->n_lease;
    r = cloud_prelaunch(G, SyncMode::ReadOnly);
    CHECK(r.action == PrelaunchResult::Proceed && g_srv->n_lease == l2);
    cloud_game_started(G, SyncMode::ReadOnly);
    put(pc2.game() / "battery" / "l.sra", rnd(500, 2), now_unix());
    int puts = g_srv->n_put;
    cloud_game_exited(G); pump();
    CHECK(g_srv->n_put == puts);                                     // nothing pushed after a read-only session
    PrelaunchResult c = cloud_resolve_lease(G, LeaseChoice::Cancel);
    CHECK(c.action == PrelaunchResult::Proceed);
    r = cloud_resolve_lease(G, LeaseChoice::PlayAnyway);
    CHECK(r.action == PrelaunchResult::Proceed);
    r = cloud_resolve_lease(G, LeaseChoice::TakeOver);
    CHECK(r.action == PrelaunchResult::Proceed);
    play();
    use(pc1);
    r = cloud_prelaunch(G, SyncMode::ReadWrite);                       // the lease moved to pc2
    CHECK(r.action == PrelaunchResult::NeedLeaseDecision && r.held_by == "pc2");
    g_srv->expire_leases();                                          // crash safe: leases expire
    r = cloud_prelaunch(G, SyncMode::ReadWrite);
    CHECK(r.action == PrelaunchResult::Proceed);
}

static void test_migration_and_restore() {
    g_expire_on_switch = true;
    printf(" migration, restore mode, recovery code, new PC\n");
    /* v1 user: a vault with saves, an old cloud_mirror folder, no v2 base yet; the cloud holds a game this PC lacks and a
     * differing copy of one it has. */
    Pc pc1("pc1");
    str uid2 = b32(26, 77), secret2 = b32(52, 78);
    g_srv->add_account(uid2, secret2);
    g_srv->seed_file(uid2, "n64/cloud-only", "battery/c.sra", rnd(1000, 1), now_unix() - 500, "PC-OLD", now_unix() - 500);
    g_srv->seed_file(uid2, "n64/both", "battery/b.sra", rnd(1000, 2), now_unix() - 400, "PC-OLD", now_unix() - 400);
    put(pc1.vault / "n64" / "both" / "battery" / "b.sra", rnd(1000, 3), now_unix() - 300);       // differs from the cloud
    put(pc1.vault / "n64" / "local-only" / "battery" / "l.sra", rnd(1000, 4), now_unix() - 300);  // cloud has nothing
    fs::path mirror = pc1.root / "cloud_mirror" / uid2.substr(0, 8);
    put(mirror / "n64" / "both" / "battery" / "b.sra", rnd(10, 9));
    CHECK(fs::exists(mirror));
    cloud_engine_stop();
    set_vault_dir(pc1.vault.u8string());
    EngineConfig ec; ec.data_dir = pc1.data.u8string(); ec.device_name = "pc1"; ec.no_threads = true;
    cloud_engine_configure(ec);
    cloud_set_identity(uid2, secret2);
    cloud_engine_start(g_srv->url());
    pump();
    /* cloud wins for the game this PC had no save for */
    CHECK(get(pc1.vault / "n64" / "cloud-only" / "battery" / "c.sra") == rnd(1000, 1));
    /* local-only saves are uploaded once the pull phase is over */
    CHECK(!g_srv->cur_sha(uid2, "n64/local-only", "battery/l.sra").empty());
    /* a differing local save is never overwritten: it becomes a conflict, decided at the game's launch */
    CHECK(get(pc1.vault / "n64" / "both" / "battery" / "b.sra") == rnd(1000, 3));
    CHECK(g_srv->cur_sha(uid2, "n64/both", "battery/b.sra") == sha256_hex_of(rnd(1000, 2)));
    GameId both{"n64", "both"};
    PrelaunchResult r = cloud_prelaunch(both, SyncMode::ReadWrite);
    CHECK(r.action == PrelaunchResult::NeedConflictDecision && r.conflicts.size() == 1);
    CHECK(!fs::exists(mirror));                                       // v1 mirror removed after the first full pass
    int done = 0, total = 0;
    cloud_backup_progress(&done, &total);
    CHECK(total >= 1 && done >= 1);

    /* a brand new PC adopts the account through the recovery code and pulls everything */
    Pc pc3("pc3");
    cloud_engine_stop();
    set_vault_dir(pc3.vault.u8string());
    ec.data_dir = pc3.data.u8string(); ec.device_name = "pc3";
    cloud_engine_configure(ec);
    str err;
    CHECK(!cloud_adopt_recovery_code("garbage", &err) && !err.empty());
    CHECK(cloud_adopt_account(uid2, secret2, &err));                  // the account code redeem ends here (identity_link is covered by hublink_test + the real-Worker QA)
    CHECK(fs::exists(pc3.data / "identity.json"));
    cloud_engine_start(g_srv->url());
    pump();
    CHECK(get(pc3.vault / "n64" / "cloud-only" / "battery" / "c.sra") == rnd(1000, 1));
    CHECK(get(pc3.vault / "n64" / "both" / "battery" / "b.sra") == rnd(1000, 2));     // cloud wins where this PC has nothing
    CHECK(fs::exists(pc3.vault / "n64" / "local-only" / "battery" / "l.sra"));
    CHECK(cloud_account().known && cloud_account().online);
    /* the secret never appears in the log */
    str log = fs::exists(pc3.data / "cloud_saves.log") ? get(pc3.data / "cloud_saves.log") : str();
    CHECK(log.find(secret2) == str::npos);
}

static void test_local_flags() {
    g_expire_on_switch = true;
    printf(" pin, local-only, remove, pause\n");
    Pc pc1("pc1");
    use(pc1);
    put(pc1.game() / "battery" / "p.sra", rnd(500, 1), now_unix() - 100);
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();
    cloud_set_pin(G, true);
    GameCloudInfo gi;
    CHECK(cloud_game(G, &gi) && gi.pinned);
    cloud_set_local_only(G, true);
    put(pc1.game() / "battery" / "p.sra", rnd(500, 2), now_unix() - 10);
    str before = g_srv->cur_sha(g_uid, GK, "battery/p.sra");
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();
    CHECK(g_srv->cur_sha(g_uid, GK, "battery/p.sra") == before);
    CHECK(cloud_game(G, &gi) && gi.state == CloudState::LocalOnly && gi.local_only == LocalOnlyReason::User);
    cloud_set_local_only(G, false);
    pump();
    CHECK(g_srv->cur_sha(g_uid, GK, "battery/p.sra") == sha256_hex_file((pc1.game() / "battery" / "p.sra").u8string()));
    cloud_remove_game(G);
    CHECK(g_srv->cur_sha(g_uid, GK, "battery/p.sra").empty());
    CHECK(fs::exists(pc1.game() / "battery" / "p.sra"));              // local copy stays
    CHECK(cloud_game(G, &gi) && gi.state == CloudState::LocalOnly);
    cloud_set_paused(true);
    CHECK(cloud_game(G, &gi) && gi.state == CloudState::Paused);
    int m = g_srv->n_manifest;
    PrelaunchResult r = cloud_prelaunch(G, SyncMode::ReadWrite);
    CHECK(r.action == PrelaunchResult::Proceed && !r.offline && g_srv->n_manifest == m);
    cloud_set_paused(false);
}

static void test_daily_limit_and_auth() {
    printf(" daily limit, bad account\n");
    Pc pc1("pc1");
    use(pc1);
    put(pc1.game() / "battery" / "d.sra", rnd(500, 1), now_unix() - 100);
    put(pc1.game() / "battery" / "d2.sra", rnd(500, 2), now_unix() - 100);
    g_srv->put_limit = 1;
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();
    CHECK(cloud_account().queued_games == 1);                         // 429 account-daily-limit: retried after retryAfter, not hammered
    CHECK(cloud_engine_idle());
    g_srv->put_limit = 0;
    cloud_wake_queue(); pump();
    CHECK(cloud_account().queued_games == 0 && !g_srv->cur_sha(g_uid, GK, "battery/d2.sra").empty());
    /* an unknown account: needs attention, never blocks play */
    cloud_engine_stop();
    set_vault_dir(pc1.vault.u8string());
    EngineConfig ec; ec.data_dir = pc1.data.u8string(); ec.device_name = "pc1"; ec.no_threads = true;
    cloud_engine_configure(ec);
    cloud_set_identity(b32(26, 500), b32(52, 501));
    cloud_engine_start(g_srv->url());
    PrelaunchResult r = cloud_prelaunch(G, SyncMode::ReadWrite);
    CHECK(r.action == PrelaunchResult::Proceed);
    CHECK(cloud_account().needs_attention && cloud_account().attention == "account");
    GameCloudInfo gi;
    CHECK(cloud_game(G, &gi) && gi.state == CloudState::NeedsAttention);
}

static void test_states_switch() {
    printf(" Back up save states toggle: off keeps states and thumbnails local, battery still syncs\n");
    Pc pc1("pc1");
    use(pc1);
    cloud_set_sync_states(false);
    put(pc1.game() / "battery" / "ssb64.sra", rnd(4096, 31), now_unix() - 1000);
    put(pc1.game() / "meta.json", "{\"title\":\"Smash\"}", now_unix() - 1000);
    put(pc1.game() / "states" / "slot2.state", rnd(50000, 32), now_unix() - 1000);
    put(pc1.game() / "states" / "slot2.png", rnd(3000, 33), now_unix() - 1000);
    cloud_prelaunch(G, SyncMode::ReadWrite);
    play();
    exit_game();
    CHECK(!g_srv->cur_sha(g_uid, GK, "battery/ssb64.sra").empty());
    CHECK(g_srv->cur_sha(g_uid, GK, "states/slot2.state").empty());
    CHECK(g_srv->cur_sha(g_uid, GK, "states/slot2.png").empty());
    cloud_set_sync_states(true);
    play();
    exit_game();
    CHECK(!g_srv->cur_sha(g_uid, GK, "states/slot2.state").empty());
    CHECK(!g_srv->cur_sha(g_uid, GK, "states/slot2.png").empty());
}

/* Rivalries: each match is its own small file under _account/rivals/ (append-only), so two PCs never conflict and both end up with all of them. */
static void test_account_rivals_two_pcs() {
    printf(" account rivals: per-match files sync both ways without conflicts\n");
    Pc pc1("pr1"), pc2("pr2");
    auto rv = [](const Pc& pc, const char* id) { return pc.vault / "_account" / "rivals" / (str(id) + ".json"); };
    use(pc1);
    put(rv(pc1, "AAAA-1"), "{\"id\":\"AAAA-1\",\"at\":1000}", now_unix() - 500);
    cloud_account_prefs_changed();
    pump();
    CHECK(!g_srv->cur_sha(g_uid, "_account/_", "_account/rivals/AAAA-1.json").empty());
    use(pc2);
    put(rv(pc2, "BBBB-2"), "{\"id\":\"BBBB-2\",\"at\":2000}", now_unix() - 400);   // PC2 played its own match before ever syncing
    cloud_account_prefs_changed();   // what the hub does after storing a match
    cloud_sync_all_now();
    pump();
    CHECK(fs::is_regular_file(rv(pc2, "AAAA-1")));                     // pulled
    CHECK(!g_srv->cur_sha(g_uid, "_account/_", "_account/rivals/BBBB-2.json").empty());   // pushed
    use(pc1);
    cloud_sync_all_now();
    pump();
    CHECK(fs::is_regular_file(rv(pc1, "BBBB-2")));
    CHECK(fs::is_regular_file(rv(pc1, "AAAA-1")));                     // untouched
}

/* Favorites follow the account: two PCs favorite different games offline, sync, and both end up with both (prefs::merge_text). */
static void test_account_prefs_two_pcs() {
    printf(" account prefs: two PCs favorite different games\n");
    namespace pf = hub::prefs;
    Pc pc1("pa1"), pc2("pa2");
    fs::path lib = scratch_root() / "pa_lib";
    fs::remove_all(lib); fs::create_directories(lib);
    hub::library_set_data_dir(lib.u8string());
    auto write_prefs = [&](const Pc& pc, const std::vector<str>& favs, long long t) {
        pf::Prefs p; pf::load(pc.vault.u8string(), &p);
        pf::set_favorites(&p, favs, t);
        CHECK(pf::save(pc.vault.u8string(), p));
    };
    use(pc1);
    write_prefs(pc1, {"n64:one"}, 1000);
    cloud_account_prefs_changed();
    pump();
    CHECK(!g_srv->cur_sha(g_uid, "_account/_", "_account/prefs.json").empty());
    /* PC2 starts empty, pulls the prefs, and its library favorites follow */
    use(pc2);
    cloud_sync_all_now();
    pump();
    pf::Prefs got;
    CHECK(pf::load(pc2.vault.u8string(), &got) && got.favorites == std::vector<str>{"n64:one"});
    CHECK(hub::game_state("n64:one").favorite);
    /* both go "offline" (no sync in between) and each favorites a different game */
    { pf::Prefs p; pf::load(pc2.vault.u8string(), &p); pf::set_favorite(&p, "gba:two", true, 3000); pf::save(pc2.vault.u8string(), p); }
    cloud_account_prefs_changed();
    pump();                                                     // PC2 pushes first
    use(pc1);
    { pf::Prefs p; pf::load(pc1.vault.u8string(), &p); pf::set_favorite(&p, "snes:three", true, 2000); pf::save(pc1.vault.u8string(), p); }
    cloud_account_prefs_changed();
    pump();                                                     // PC1 conflicts with the cloud copy -> merged, not keep-both
    pf::Prefs m1; pf::load(pc1.vault.u8string(), &m1);
    CHECK(m1.favorites == (std::vector<str>{"gba:two", "n64:one", "snes:three"}));
    pump();
    use(pc2);
    cloud_sync_all_now();
    pump();
    pf::Prefs m2; pf::load(pc2.vault.u8string(), &m2);
    CHECK(m2.favorites == m1.favorites);
    hub::library_set_data_dir("");
}

static void test_linked_pcs() {
    printf(" linked PCs: list, revoke, signed-out PC, sign in again\n");
    Pc pc1("pc1"), pc2("pc2");
    use(pc1);
    put(pc1.game() / "battery" / "p.sra", rnd(600, 1), now_unix() - 100);
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();
    use(pc2);
    put(pc2.game() / "battery" / "q.sra", rnd(600, 2), now_unix() - 50);
    cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();
    CHECK(g_srv->devices(g_uid) == 2);

    use(pc1);
    std::vector<LinkedPc> pcs;
    str err;
    CHECK(cloud_list_pcs(&pcs, &err) && pcs.size() == 2);
    const LinkedPc* me = nullptr; const LinkedPc* other = nullptr;
    for (auto& p : pcs) (p.current ? me : other) = &p;
    CHECK(me && other && me->name == "pc1" && other->name == "pc2" && other->last_seen > 0 && !other->current);
    /* this PC cannot sign itself out: refused by the client before any request */
    int rv0 = g_srv->n_revoke;
    CHECK(me && !cloud_revoke_pc(me->dev, &err) && !err.empty() && g_srv->n_revoke == rv0 && g_srv->devices(g_uid) == 2);
    CHECK(!cloud_revoke_pc("", &err));
    CHECK(!cloud_revoke_pc("d-nosuchpc0", &err) && err.find("no longer") != str::npos);   // unknown PC: 404 from the server
    CHECK(!cloud_revoked());

    /* sign pc2 out from pc1 */
    CHECK(other && cloud_revoke_pc(other->dev, &err) && g_srv->devices(g_uid) == 1 && g_srv->revoked(g_uid) == 1);
    CHECK(cloud_list_pcs(&pcs, &err) && pcs.size() == 1 && pcs[0].current);

    /* pc2 finds out on its next cloud call: clean signed-out state, local saves untouched, no more traffic */
    use(pc2);
    CHECK(!cloud_revoked());
    PrelaunchResult r = cloud_prelaunch(G, SyncMode::ReadWrite);
    CHECK(r.action == PrelaunchResult::Proceed);                    // play is never blocked
    CHECK(cloud_revoked());
    CloudAccountInfo ai = cloud_account();
    CHECK(ai.needs_attention && ai.attention == "device-revoked");
    CloudEvent ev;
    CHECK(pop_event(CloudEvent::NeedsAttention, &ev) && ev.text.find("stopped") != str::npos && !ev.good);
    CHECK(fs::exists(pc2.game() / "battery" / "q.sra"));
    put(pc2.game() / "battery" / "r.sra", rnd(600, 3), now_unix());
    int m0 = g_srv->n_manifest, d0 = g_srv->n_devices;
    play(); exit_game();
    cloud_sync_all_now(); pump();
    CHECK(g_srv->n_manifest == m0 && g_srv->devices(g_uid) == 1);   // nothing sent while signed out
    CHECK(!cloud_list_pcs(&pcs, &err) && g_srv->n_devices == d0);
    CHECK(g_srv->cur_sha(g_uid, GK, "battery/r.sra").empty());
    /* the signed-out state survives a restart of the app */
    use(pc2);
    CHECK(cloud_revoked());
    r = cloud_prelaunch(G, SyncMode::ReadWrite);
    CHECK(r.action == PrelaunchResult::Proceed && g_srv->n_manifest == m0);

    /* sign in again: a new device id, sync resumes, the saves go up */
    cloud_sign_in_again();
    CHECK(!cloud_revoked() && !cloud_account().needs_attention);
    r = cloud_prelaunch(G, SyncMode::ReadWrite); play(); exit_game();
    CHECK(g_srv->devices(g_uid) == 2 && g_srv->n_manifest > m0);
    CHECK(!g_srv->cur_sha(g_uid, GK, "battery/r.sra").empty());
    CHECK(cloud_list_pcs(&pcs, &err) && pcs.size() == 2);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::set_terminate([]() {
        try { if (auto e = std::current_exception()) std::rethrow_exception(e); } catch (const std::exception& x) { printf("terminate: %s\n", x.what()); } catch (...) { printf("terminate: unknown\n"); }
        printf("terminate called (last data dir %s)\n", g_last_data.c_str());
        abort();
    });
    _putenv_s("RETROHUB_QA", "1");
    printf("pure rules\n");
    test_classify();
    test_three_way();
    test_wire();
    test_queue_and_base();

    printf("engine against the mock Worker\n");
    MockServer srv;
    if (!srv.start(0)) { printf("cannot start the mock server\n"); return 1; }
    g_srv = &srv;
    g_uid = b32(26, 11);
    g_secret = b32(52, 12);
    srv.add_account(g_uid, g_secret);
    test_http_timeout();
    { int f0 = g_fail; fresh_account(); test_basic_two_pcs(); if (g_fail != f0) dump_log(); }
    { int f0 = g_fail; fresh_account(); test_offline_retry_resume(); if (g_fail != f0) dump_log(); }
    { int f0 = g_fail; fresh_account(); test_account_prefs_two_pcs(); if (g_fail != f0) dump_log(); }
    { int f0 = g_fail; fresh_account(); test_account_rivals_two_pcs(); if (g_fail != f0) dump_log(); }
    { int f0 = g_fail; fresh_account(); test_conflict(); if (g_fail != f0) dump_log(); }
    { int f0 = g_fail; fresh_account(); test_plain_conflict_surfaces(); if (g_fail != f0) dump_log(); }
    { int f0 = g_fail; fresh_account(); test_states_switch(); if (g_fail != f0) dump_log(); }
    { int f0 = g_fail; fresh_account(); test_states_never_prompt(); if (g_fail != f0) dump_log(); }
    { int f0 = g_fail; fresh_account(); test_quota(); if (g_fail != f0) dump_log(); }
    { int f0 = g_fail; fresh_account(); test_lease_and_readonly(); if (g_fail != f0) dump_log(); }
    { int f0 = g_fail; fresh_account(); test_local_flags(); if (g_fail != f0) dump_log(); }
    { int f0 = g_fail; fresh_account(); test_daily_limit_and_auth(); if (g_fail != f0) dump_log(); }
    { int f0 = g_fail; fresh_account(); test_migration_and_restore(); if (g_fail != f0) dump_log(); }
    { int f0 = g_fail; fresh_account(); test_linked_pcs(); if (g_fail != f0) dump_log(); }
    cloud_engine_stop();
    srv.stop();
    { std::error_code ec; fs::remove_all(scratch_root(), ec); }
    CHECK(srv.sig_checks_failed == 0);                              // every request carried a valid RHSIG signature
    if (g_fail) dump_log();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
