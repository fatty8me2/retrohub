// QA driver (not a ctest): invite codes end to end against a LOCAL worker, with the REAL FriendsClient and the lock verdict (hublock::Verdict).
//   qa_invite_e2e ws://127.0.0.1:PORT <work dir>      (qa\invite_e2e.ps1 starts the worker with the right vars)
// The owner is the account the worker was started with as ADMIN_UID (26 x 'a'); its secret is made up here and registered by its first hello.
// Prints PASS / FAIL per scenario with the evidence; exit code 1 if anything failed. Refuses anything but a loopback server.
#include "friends.h"
#include "log.h"
#include "../hub/lockout_logic.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <thread>

using namespace hubnet;
namespace fs = std::filesystem;

static int g_fail = 0;
static void report(bool ok, const std::string& what, const std::string& evidence = "")
{
    if (!ok) ++g_fail;
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), evidence.empty() ? "" : "   | ", evidence.c_str());
    std::fflush(stdout);
}
static int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
static const char* lock_name(hublock::Lock l) { return l == hublock::Lock::Pending ? "pending" : l == hublock::Lock::Unavailable ? "unavailable" : l == hublock::Lock::Setup ? "setup" : "none"; }

struct Node {
    std::string label, dir;
    FriendsClient cli;
    hublock::Verdict V;
    std::vector<Event> events;
    bool started = false;
    void start(const std::string& server, const std::string& name, const std::string& base, const Identity* fixed = nullptr)
    {
        label = name;
        dir = base + name + "\\";
        fs::remove_all(dir);
        fs::create_directories(dir + "vault");
        if (fixed) identity_adopt(dir, dir + "vault\\", *fixed);
        cli.set_paths(dir, dir + "vault\\");
        started = cli.start(server, name);
        V.init(hublock::Cache(), /*had_account=*/false, cli.my_uid());
    }
    void tick()
    {
        cli.update();
        Event e;
        while (cli.poll_event(&e)) events.push_back(e);
        V.on_live(cli.access(), cli.connected(), cli.is_admin());
    }
    hublock::Lock lock() const { return V.lock(); }
    const Event* find(Event::Kind k, const std::string& uid = "") const
    {
        for (const Event& e : events) if (e.kind == k && (uid.empty() || e.uid == uid)) return &e;
        return nullptr;
    }
    int count(Event::Kind k) const { int n = 0; for (const Event& e : events) if (e.kind == k) ++n; return n; }
};
static std::vector<Node*> g_nodes;
static bool until(const std::function<bool()>& cond, double secs)
{
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        for (Node* n : g_nodes) n->tick();
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
static void settle(double secs) { until([] { return false; }, secs); }
static std::string b32s(int n)
{
    static const char a[] = "abcdefghijklmnopqrstuvwxyz234567";
    std::random_device rd;
    std::string s;
    for (int i = 0; i < n; ++i) s += a[rd() % 32];
    return s;
}
static std::string base_of(const std::string& s) { return s; }

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: qa_invite_e2e ws://127.0.0.1:PORT <work dir>\n"); return 2; }
    const std::string server = argv[1];
    std::string work = argv[2];
    if (work.back() != '\\') work += '\\';
    if (server.rfind("ws://127.0.0.1:", 0) != 0 && server.rfind("ws://localhost:", 0) != 0) { std::printf("refusing a non-loopback server: %s\n", server.c_str()); return 2; }

    // ---- owner ----
    Identity oid; oid.user_id = std::string(26, 'a'); oid.secret = std::string(52, (char)113);   // fixed: the first hello registers it, so a rerun against the same worker still works oid.created = (int64_t)(now_ms() / 1000);
    Node owner;
    owner.start(server, "Owner", work, &oid);
    g_nodes.push_back(&owner);
    report(until([&] { return owner.cli.connected() && owner.cli.is_admin(); }, 15), "owner connects and the welcome says admin", "uid=" + owner.cli.my_uid().substr(0, 6) + "...");
    if (!owner.cli.is_admin()) { std::printf("cannot continue without the owner\n"); return 1; }

    if (argc > 3 && std::string(argv[3]) == "debug") {   // watch one newcomer's state transitions
        Node d; d.start(server, "Debug1", work); g_nodes.push_back(&d);
        std::string last;
        const auto t0 = std::chrono::steady_clock::now();
        until([&] {
            char b[200];
            std::snprintf(b, sizeof b, "access=%d connected=%d lock=%s status='%s'", (int)d.cli.access(), d.cli.connected(), lock_name(d.lock()), d.cli.status_text().c_str());
            if (b != last) { last = b; std::printf("%5.2fs %s%c", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), b, 10); std::fflush(stdout); }
            return false;
        }, 20);
        return 0;
    }
    // ---- 1. owner creates an invite (real client path) ----
    owner.cli.admin_invite_create(1, 168, "e2e test");
    const bool got = until([&] { return owner.cli.admin_invites().has_created; }, 8);
    const AdminInvites& inv = owner.cli.admin_invites();
    const AdminInvite one = inv.created;
    report(got && invite_valid(one.code) && one.uses == 1 && one.used == 0 && one.note == "e2e test", "1 create: admin_invites parsed (code, uses, note)", "code=" + one.code);
    const int64_t t = now_ms(), H = 3600000LL;
    report(one.created > t - 60000 && one.created < t + 60000, "1 time units: created is ms since the epoch", "created=" + std::to_string(one.created) + " now=" + std::to_string(t));
    report(one.exp - one.created > 167 * H && one.exp - one.created < 169 * H, "1 time units: exp - created is 168 h in ms", "delta_h=" + std::to_string((one.exp - one.created) / (double)H));
    report(!inv.list.empty() && inv.list[0].code == one.code, "1 the list contains it");

    // ---- 2. a fresh pending client; owner is told (bell source) ----
    Node n1; n1.start(server, "Newcomer1", work); g_nodes.push_back(&n1);
    report(until([&] { return n1.lock() == hublock::Lock::Pending; }, 15), "2 new account is locked Pending", std::string("lock=") + lock_name(n1.lock()));
    const bool told = until([&] { return owner.find(Event::AdminJoinWaiting, n1.cli.my_uid()) != nullptr; }, 8);
    const Event* jw = owner.find(Event::AdminJoinWaiting, n1.cli.my_uid());
    report(told && jw && jw->name == "Newcomer1" && jw->text == n1.cli.my_code(), "2 owner gets admin_pending.latest -> AdminJoinWaiting (name + friend code)", jw ? "name=" + jw->name + " code=" + jw->text : "none");
    report(jw && jw->msg.at > t - 120000 && jw->msg.at < now_ms() + 60000, "2 time units: latest.created is ms", jw ? std::to_string(jw->msg.at) : "none");
    report(owner.cli.admin_pending() >= 1, "2 owner pending count", std::to_string(owner.cli.admin_pending()));

    // ---- 3. wrong code ----
    report(n1.cli.redeem_invite("ABCD-EFGH"), "3 redeem path sends on the waiting socket");
    until([&] { return n1.count(Event::InviteRedeemed) >= 1; }, 8);
    const Event* r1 = n1.find(Event::InviteRedeemed);
    report(r1 && !r1->ok && r1->text == "invalid", "3 wrong code -> invalid", r1 ? "ok=" + std::to_string(r1->ok) + " reason=" + r1->text + " | \"" + redeem_result_text(r1->ok, r1->text) + "\"" : "no answer");
    report(n1.lock() == hublock::Lock::Pending, "3 still locked after a wrong code");

    // ---- 4. real code unlocks without a restart ----
    n1.events.clear();
    report(n1.cli.redeem_invite(one.code), "4 redeem good code");
    const bool ok4 = until([&] { return n1.count(Event::InviteRedeemed) >= 1; }, 8);
    const Event* r2 = n1.find(Event::InviteRedeemed);
    report(ok4 && r2 && r2->ok, "4 invite_result ok:true", r2 ? "ok=" + std::to_string(r2->ok) : "no answer");
    const bool unl = until([&] { return n1.cli.connected() && n1.lock() == hublock::Lock::None; }, 15);
    report(unl, "4 the client unlocks without a restart (4011 -> fresh hello welcomed)", std::string("lock=") + lock_name(n1.lock()) + " connected=" + std::to_string(n1.cli.connected()));
    const bool usedev = until([&] { return owner.find(Event::AdminInviteUsed, n1.cli.my_uid()) != nullptr; }, 8);
    const Event* iu = owner.find(Event::AdminInviteUsed, n1.cli.my_uid());
    report(usedev && iu && iu->name == "Newcomer1" && iu->text == one.code, "4 owner gets admin_invite_used (name, code)", iu ? iu->name + " " + iu->text : "none");
    owner.cli.admin_invite_list();
    until([&] { for (const AdminInvite& a : owner.cli.admin_invites().list) if (a.code == one.code && a.used == 1) return true; return false; }, 8);
    {
        bool f = false; std::string who;
        for (const AdminInvite& a : owner.cli.admin_invites().list) if (a.code == one.code) { f = a.used == 1 && a.spent(); for (auto& w : a.by_names) who += w + ","; }
        report(f && who == "Newcomer1,", "4 list shows 1/1 used by Newcomer1", who);
    }

    // ---- 5. used-up ----
    Node n2; n2.start(server, "Newcomer2", work); g_nodes.push_back(&n2);
    report(until([&] { return n2.lock() == hublock::Lock::Pending; }, 15), "5 second newcomer is Pending");
    report(n2.cli.redeem_invite(one.code), "5 redeem sent");
    until([&] { return n2.count(Event::InviteRedeemed) >= 1; }, 8);
    const Event* r3 = n2.find(Event::InviteRedeemed);
    report(r3 && !r3->ok && r3->text == "used-up", "5 spent code -> used-up", r3 ? r3->text + " | \"" + redeem_result_text(r3->ok, r3->text) + "\"" : "no answer");

    // ---- 6. approve by friend code ----
    Node n3; n3.start(server, "Newcomer3", work); g_nodes.push_back(&n3);
    until([&] { return n3.lock() == hublock::Lock::Pending; }, 15);
    owner.events.clear();
    owner.cli.admin_approve_code(n3.cli.my_code());
    const bool unl3 = until([&] { return n3.cli.connected() && n3.lock() == hublock::Lock::None; }, 15);
    report(unl3, "6 admin_approve_code by friend code unlocks the pending client", std::string("code=") + n3.cli.my_code() + " lock=" + lock_name(n3.lock()));
    owner.events.clear();
    owner.cli.admin_approve_code("RH-ZZZZ-ZZZZ");
    until([&] { return owner.count(Event::AdminResult) >= 1; }, 6);
    const Event* ar = owner.find(Event::AdminResult);
    report(ar && !ar->ok && ar->detail == "not-found", "6 unknown friend code -> not-found", ar ? ar->detail + " | \"" + ar->text + "\"" : "no answer");
    owner.events.clear();
    owner.cli.admin_approve_code(n3.cli.my_code());   // already approved
    until([&] { return owner.count(Event::AdminResult) >= 1; }, 6);
    ar = owner.find(Event::AdminResult);
    report(ar && !ar->ok && ar->detail == "not-pending", "6 an approved account -> not-pending", ar ? ar->detail + " | \"" + ar->text + "\"" : "no answer");

    // ---- 7. pending queue: Approve and Deny ----
    Node n4, n5; n4.start(server, "Newcomer4", work); n5.start(server, "Newcomer5", work); g_nodes.push_back(&n4); g_nodes.push_back(&n5);
    until([&] { return n4.lock() == hublock::Lock::Pending && n5.lock() == hublock::Lock::Pending; }, 15);
    settle(1.0);
    owner.cli.admin_request_pending();
    const bool q = until([&] {
        const AdminList& P = owner.cli.admin_pending_list();
        int found = 0;
        for (const AdminAccount& a : P.rows) if (a.uid == n4.cli.my_uid() || a.uid == n5.cli.my_uid()) ++found;
        return found == 2;
    }, 8);
    {
        std::string names;
        for (const AdminAccount& a : owner.cli.admin_pending_list().rows) names += a.name + "(" + a.status + "," + a.code + ") ";
        report(q, "7 pending queue (admin_pending_list) holds both waiting accounts", names);
    }
    const auto tq0 = std::chrono::steady_clock::now();
    owner.cli.admin_act("approve", n4.cli.my_uid());
    owner.cli.admin_act("deny", n5.cli.my_uid());
    const bool a4 = until([&] { return n4.cli.connected() && n4.lock() == hublock::Lock::None; }, 30);
    std::printf("     (approve took %.1fs)%c", std::chrono::duration<double>(std::chrono::steady_clock::now() - tq0).count(), 10);
    const bool d5 = until([&] { return n5.lock() == hublock::Lock::Unavailable; }, 30);
    report(a4, "7 Approve from the queue unlocks the account", std::string("lock=") + lock_name(n4.lock()));
    report(d5, "7 Deny from the queue locks the account as unavailable", std::string("lock=") + lock_name(n5.lock()));
    owner.cli.admin_request_pending();
    settle(1.0);
    {   // only Newcomer2 (never approved, never redeemed) is still waiting
        bool only_n2 = !owner.cli.admin_pending_list().rows.empty();
        for (const AdminAccount& a : owner.cli.admin_pending_list().rows) if (a.uid != n2.cli.my_uid()) only_n2 = false;
        report(only_n2 && owner.cli.admin_pending() == 1, "7 the queue now holds only the account nobody approved", "pending=" + std::to_string(owner.cli.admin_pending()) + " rows=" + std::to_string(owner.cli.admin_pending_list().rows.size()));
    }

    // ---- 8. revoke, multi-use, cap ----
    owner.cli.admin_invite_create(2, 24, "two");
    until([&] { return owner.cli.admin_invites().has_created && owner.cli.admin_invites().created.note == "two"; }, 8);
    const AdminInvite two = owner.cli.admin_invites().created;
    report(two.uses == 2 && two.exp - two.created > 23 * H && two.exp - two.created < 25 * H, "8 create with options (2 uses, 24 h)", two.code);
    Node n6, n7; n6.start(server, "Newcomer6", work); n7.start(server, "Newcomer7", work); g_nodes.push_back(&n6); g_nodes.push_back(&n7);
    until([&] { return n6.lock() == hublock::Lock::Pending && n7.lock() == hublock::Lock::Pending; }, 15);
    n6.cli.redeem_invite(two.code);
    n7.cli.redeem_invite(two.code);
    const bool both = until([&] { return n6.cli.connected() && n6.lock() == hublock::Lock::None && n7.cli.connected() && n7.lock() == hublock::Lock::None; }, 20);
    report(both, "8 a 2-use code lets two accounts in", std::string("n6=") + lock_name(n6.lock()) + " n7=" + lock_name(n7.lock()));
    owner.cli.admin_invite_create(1, 24, "revoke me");
    until([&] { return owner.cli.admin_invites().has_created && owner.cli.admin_invites().created.note == "revoke me"; }, 8);
    const AdminInvite rv = owner.cli.admin_invites().created;
    owner.cli.admin_invite_revoke(rv.code);
    until([&] { for (const AdminInvite& a : owner.cli.admin_invites().list) if (a.code == rv.code) return false; return !owner.cli.admin_invites().has_created; }, 8);
    {
        bool gone = true;
        for (const AdminInvite& a : owner.cli.admin_invites().list) if (a.code == rv.code) gone = false;
        report(gone, "8 revoke removes it from the list", rv.code);
    }
    Node n8; n8.start(server, "Newcomer8", work); g_nodes.push_back(&n8);
    until([&] { return n8.lock() == hublock::Lock::Pending; }, 15);
    n8.cli.redeem_invite(rv.code);
    until([&] { return n8.count(Event::InviteRedeemed) >= 1; }, 8);
    const Event* r8 = n8.find(Event::InviteRedeemed);
    report(r8 && !r8->ok && r8->text == "invalid", "8 a revoked code -> invalid", r8 ? r8->text : "no answer");
    owner.events.clear();
    for (int i = 0; i < 70 && owner.count(Event::AdminResult) == 0; ++i) { owner.cli.admin_invite_create(1, 1, "cap"); settle(0.15); }
    const Event* cap = owner.find(Event::AdminResult);
    report(cap && !cap->ok && cap->detail == "invite-cap", "8 the active-invite cap -> invite-cap (admin_invites ok:false becomes a result)", cap ? cap->detail + " | \"" + cap->text + "\"" : "no cap reached in 70 creates");

    std::printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
