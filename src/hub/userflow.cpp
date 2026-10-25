/* HUB_USERFLOW: scripted username / account / friends flows for headless QA (qa\username_flows.ps1).
 *
 * Runs inside the real hub in screenshot mode (SDL dummy video, nothing visible) with HUB_SHOT_NET=1 against a LOCAL worker, and drives
 * the same functions the buttons call: typing into the username box (debounced check -> live server answer), un_claim_start(),
 * the account-code and redeem calls, FriendsClient::add_friend_username(), accept(). Every step logs to HUB_USERFLOW_LOG; the run ends
 * with "DONE pass=<n> fail=<n>". Refuses anything that is not a loopback server (shot_net()).
 *
 *   HUB_USERFLOW=claim      UF_PROBE=admin,qa_zed,a  UF_EXPECT=admin=NotAllowed,qa_zed=Taken,a=Invalid  UF_CLAIM=qa_alice
 *   HUB_USERFLOW=makecode   UF_OUT=<file>                (Settings > Account: account code, written to the file)
 *   HUB_USERFLOW=returning  UF_CODE=<code>  UF_FILE=<vault file that must appear>  [UF_OUT=<file>]
 *   HUB_USERFLOW=addfriend  UF_TARGET=<username>
 *   HUB_USERFLOW=accept
 *   HUB_USERFLOW=change     UF_CLAIM=<new name>
 *   HUB_USERFLOW=hold       UF_NAME=<old name>  UF_SLEEP_MS=4800  UF_WAIT_FILE=<signal file>   (held right after another PC's change, free after the hold)
 *   HUB_USERFLOW=prompt     UF_CLAIM=<name>              (needs_new_name prompt appears by itself, then pick + claim)
 *   HUB_USERFLOW=cloudpush  (waits until the cloud engine has nothing queued; used after seeding the vault) */
#include "app.h"
#include "cloud_internal.h"

#include <fstream>
#include <functional>

namespace {

using hubui::un::Avail;

const char* env(const char* k) { const char* v = std::getenv(k); return v && *v ? v : nullptr; }

struct Flow {
    bool init = false, done = false;
    int pass = 0, fail = 0;
    size_t step = 0;
    double step_t = 0, t0 = 0;
    std::vector<std::function<bool()>> steps;   // true = step finished
    std::string shot;
    std::string name;
    std::string log_path;
    std::vector<std::string> probes;
    std::map<std::string, std::string> expect;
    size_t probe_i = 0;
    bool typed = false;
    double wait_until = 0;
} F;

double now_s() { return (double)GetTickCount64() / 1000.0; }

void log(const std::string& line) {
    if (F.log_path.empty()) return;
    std::ofstream f(F.log_path, std::ios::app | std::ios::binary);
    char ts[24];
    std::snprintf(ts, sizeof ts, "[%6.1fs] ", now_s() - F.t0);
    f << ts << line << "\n";
}
void check(const std::string& what, bool ok, const std::string& detail = "") {
    (ok ? F.pass : F.fail)++;
    log(std::string(ok ? "PASS " : "FAIL ") + what + (detail.empty() ? "" : " (" + detail + ")"));
}

const char* avail_name(Avail a) {
    switch (a) {
        case Avail::Empty: return "Empty";
        case Avail::Invalid: return "Invalid";
        case Avail::Typing: return "Typing";
        case Avail::Checking: return "Checking";
        case Avail::Available: return "Available";
        case Avail::Yours: return "Yours";
        case Avail::Taken: return "Taken";
        case Avail::NotAllowed: return "NotAllowed";
        case Avail::Offline: return "Offline";
    }
    return "?";
}
bool settled(Avail a) { return a != Avail::Empty && a != Avail::Typing && a != Avail::Checking; }
bool connected() { return fr_connected() && FR.started && FR.cli.username_known(); }

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> v;
    std::string cur;
    for (char ch : s) { if (ch == c) { v.push_back(cur); cur.clear(); } else cur += ch; }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

/* ---- reusable steps ---- */
std::function<bool()> s_connect() {
    return []() {
        if (!connected()) return false;
        log("connected uid=" + FR.cli.my_uid().substr(0, 6) + "... username=[" + FR.cli.username() + "] needs_new=" + (FR.cli.needs_new_name() ? "1" : "0"));
        return true;
    };
}

/* types `name` and waits for the live answer; checks it against `want` ("" = just log) */
std::function<bool()> s_probe(const std::string& name, const std::string& want, const char* shot = nullptr) {
    auto typed = std::make_shared<bool>(false);
    auto t0 = std::make_shared<double>(0);
    return [=]() {
        if (!*typed) { un_qa_type(name); *typed = true; *t0 = now_s(); return false; }
        const Avail a = un_state();
        if (!settled(a)) return false;
        if (now_s() - *t0 < 0.3) return false;   // let the screen show it
        log("probe [" + name + "] -> " + avail_name(a) + " | \"" + un_qa_status() + "\"");
        if (!want.empty()) check("probe " + name + " is " + want, want == avail_name(a), avail_name(a));
        if (shot) F.shot = shot;
        return true;
    };
}

std::function<bool()> s_claim(const std::string& name, const char* shot = nullptr) {
    auto stage = std::make_shared<int>(0);
    return [=]() {
        if (*stage == 0) {
            un_qa_type(name);
            *stage = 1;
            return false;
        }
        if (*stage == 1) {
            if (un_state() != Avail::Available) return false;
            log("claiming [" + name + "] (state Available)");
            check("claim " + name + " started", un_claim_start());
            *stage = 2;
            return false;
        }
        if (!A.st.username_claimed || A.st.name != name) return false;
        check("claimed " + name, FR.cli.username() == name, "server says [" + FR.cli.username() + "]");
        log("UI name now [" + A.st.name + "]");
        if (shot) F.shot = shot;
        return true;
    };
}

std::function<bool()> s_wait(double secs) {
    auto t = std::make_shared<double>(0);
    return [=]() { if (*t == 0) *t = now_s(); return now_s() - *t >= secs; };
}

void build() {
    const std::string flow = env("HUB_USERFLOW");
    if (flow == "claim") {
        F.steps.push_back(s_connect());
        for (auto& p : split(env("UF_PROBE") ? env("UF_PROBE") : "", ',')) {
            std::string want;
            if (const char* e = env("UF_EXPECT")) for (auto& kv : split(e, ',')) if (kv.rfind(p + "=", 0) == 0) want = kv.substr(p.size() + 1);
            F.steps.push_back(s_probe(p, want, want == "Taken" ? "taken" : want == "NotAllowed" ? "notallowed" : want == "Invalid" ? "invalid" : nullptr));
        }
        if (const char* c = env("UF_CLAIM")) {
            F.steps.push_back(s_probe(c, "Available", "available"));
            F.steps.push_back(s_claim(c, nullptr));
            F.steps.push_back(s_wait(1.5));
            F.steps.push_back([]() { check("wizard moved past the Name step", wizard_state() != 3, "wizard_state=" + std::to_string(wizard_state())); return true; });
        }
    } else if (flow == "makecode") {
        F.steps.push_back(s_connect());
        auto started = std::make_shared<bool>(false);
        F.steps.push_back([started]() {
            account_qa_poll();
            if (!*started) { account_qa_make_code(); *started = true; return false; }
            if (account_qa_busy()) return false;
            const std::string code = account_qa_code();
            check("account code created", code.size() >= 14, code.empty() ? account_qa_err() : "len=" + std::to_string(code.size()));
            if (const char* o = env("UF_OUT")) { std::ofstream f(o, std::ios::trunc); f << code; }
            log("code " + std::to_string(code.size()) + " chars");
            return true;
        });
    } else if (flow == "returning") {
        auto started = std::make_shared<bool>(false);
        F.steps.push_back([started]() {
            account_qa_poll();
            if (!*started) {
                std::string code;
                if (const char* c = env("UF_CODE")) code = c;
                else if (const char* f = env("UF_CODE_FILE")) { std::ifstream in(f); std::getline(in, code); }
                if (code.empty()) { check("code available", false); F.done = true; return true; }
                log("redeeming a " + std::to_string(code.size()) + "-char code");
                account_qa_redeem(code);
                *started = true;
                return false;
            }
            if (account_qa_busy()) return false;
            if (!account_qa_err().empty()) { check("redeem", false, account_qa_err()); F.done = true; return true; }
            return A.st.username_claimed && connected();
        });
        F.steps.push_back([]() {
            log("signed in: UI name [" + A.st.name + "] server username [" + FR.cli.username() + "] uid " + FR.cli.my_uid().substr(0, 6) + "...");
            check("returning player got the account's username", A.st.username_claimed && !A.st.name.empty() && FR.cli.username() == A.st.name);
            if (const char* o = env("UF_OUT")) { std::ofstream f(o, std::ios::trunc); f << A.st.name; }
            return true;
        });
        F.steps.push_back([]() {
            const char* want = env("UF_FILE");
            if (!want) return true;
            std::error_code ec;
            if (!std::filesystem::exists(want, ec)) return false;
            std::ifstream in(want, std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const char* exp = env("UF_FILE_PREFIX");
            check("cloud save pulled onto the new PC", !body.empty() && (!exp || body.rfind(exp, 0) == 0), "bytes=" + std::to_string(body.size()));
            F.shot = "pulled";
            return true;
        });
    } else if (flow == "addfriend") {
        F.steps.push_back(s_connect());
        auto sent = std::make_shared<bool>(false);
        F.steps.push_back([sent]() {
            const char* t = env("UF_TARGET");
            if (!*sent) { FR.cli.add_friend_username(t ? t : ""); *sent = true; log(std::string("add_friend_username [") + (t ? t : "") + "]"); return false; }
            const bool pend = !FR.cli.outgoing_requests().empty();
            const bool fr = !FR.cli.friends().empty();
            if (!pend && !fr) return false;
            check("friend request to the username went out", true, fr ? "already friends" : "pending: " + FR.cli.outgoing_requests()[0].name);
            return true;
        });
        F.steps.push_back(s_wait(2.0));
    } else if (flow == "accept") {
        F.steps.push_back(s_connect());
        auto asked = std::make_shared<bool>(false);
        F.steps.push_back([asked]() {
            if (!*asked) {
                if (FR.cli.incoming_requests().empty()) return false;
                const auto r = FR.cli.incoming_requests()[0];
                log("incoming request from [" + r.name + "]");
                if (const char* w = env("UF_TARGET")) check("request came from " + std::string(w), r.name == w, r.name);
                FR.cli.accept(r.uid);
                *asked = true;
                return false;
            }
            if (FR.cli.friends().empty()) return false;
            check("friend list has the new friend", true, FR.cli.friends()[0].name);
            return true;
        });
        F.steps.push_back(s_wait(1.0));
    } else if (flow == "change") {
        F.steps.push_back(s_connect());
        F.steps.push_back([]() { check("starts with a claimed username", A.st.username_claimed, A.st.name); un_open_change(); return true; });
        F.steps.push_back(s_probe(env("UF_CLAIM") ? env("UF_CLAIM") : "", "Available"));
        F.steps.push_back(s_claim(env("UF_CLAIM") ? env("UF_CLAIM") : ""));
        F.steps.push_back([]() {   // tell a concurrently running PC that the old name was just released (it starts its hold probe now)
            if (const char* sg = env("UF_SIGNAL")) { std::ofstream f(sg, std::ios::trunc); f << "changed"; }
            return true;
        });
        F.steps.push_back(s_wait(1.0));
    } else if (flow == "hold") {
        F.steps.push_back(s_connect());
        F.steps.push_back([]() {
            const char* w = env("UF_WAIT_FILE");
            std::error_code ec;
            return !w || std::filesystem::exists(w, ec);
        });
        const std::string n = env("UF_NAME") ? env("UF_NAME") : "";
        const double ms = env("UF_SLEEP_MS") ? std::atof(env("UF_SLEEP_MS")) : 4800;
        F.steps.push_back(s_probe(n, env("UF_EXPECT_NOW") ? env("UF_EXPECT_NOW") : "Taken", "held"));
        F.steps.push_back(s_probe("x_reset_" + std::to_string(GetCurrentProcessId() % 1000), ""));
        F.steps.push_back(s_wait(ms / 1000.0));
        F.steps.push_back(s_probe(n, "Available", "released"));
    } else if (flow == "prompt") {
        F.steps.push_back(s_connect());
        F.steps.push_back([]() {
            if (!un_qa_prompt_open()) return false;
            check("needs_new_name prompt is open", FR.cli.needs_new_name() && !A.st.username_claimed, "needs_new=" + std::to_string(FR.cli.needs_new_name()));
            F.shot = "prompt";
            return true;
        });
        F.steps.push_back(s_probe(env("UF_CLAIM") ? env("UF_CLAIM") : "", "Available", "prompt-available"));
        F.steps.push_back(s_claim(env("UF_CLAIM") ? env("UF_CLAIM") : ""));
        F.steps.push_back(s_wait(1.0));
        F.steps.push_back([]() { check("prompt closed after claiming", !un_qa_prompt_open()); return true; });
    } else if (flow == "cloudpush") {
        F.steps.push_back(s_connect());
        F.steps.push_back([]() { return hub::saves::cloud_engine_idle() && hub::saves::cloud_account().known; });
        F.steps.push_back(s_wait(2.0));
    } else {
        log("unknown flow " + flow);
        F.done = true;
    }
}

}  // namespace

bool userflow_active() { return env("HUB_USERFLOW") != nullptr && shot_net(); }
bool userflow_done() { return F.done; }
std::string userflow_take_shot() { std::string s; s.swap(F.shot); return s; }

void userflow_tick() {
    if (!userflow_active() || F.done) return;
    if (!F.init) {
        F.init = true;
        F.t0 = now_s();
        F.step_t = F.t0;
        if (const char* l = env("HUB_USERFLOW_LOG")) F.log_path = l;
        log(std::string("flow ") + env("HUB_USERFLOW") + " server=" + server_url());
        build();
    }
    if (F.done) return;
    if (F.step >= F.steps.size()) {
        log("DONE pass=" + std::to_string(F.pass) + " fail=" + std::to_string(F.fail));
        F.done = true;
        return;
    }
    if (!A.scanned_once && now_s() - F.t0 < 25.0) return;   // let the library scan finish first (same as the other shot drivers)
    const double now = now_s();
    static double diag_t = 0;
    if (now - F.step_t > 8.0 && now - diag_t > 8.0) { diag_t = now; log(std::string("waiting: step ") + std::to_string(F.step) + " un_state=" + avail_name(un_state()) + " connected=" + (fr_connected() ? "1" : "0") + " status=\"" + un_qa_status() + "\" claimed=" + (A.st.username_claimed ? "1" : "0") + " claiming=" + (un_claiming() ? "1" : "0")); }
    if (now - F.step_t > 45.0) {
        check("step " + std::to_string(F.step) + " finished in time", false, "timeout");
        log("DONE pass=" + std::to_string(F.pass) + " fail=" + std::to_string(F.fail));
        F.done = true;
        return;
    }
    if (F.steps[F.step]()) { ++F.step; F.step_t = now_s(); }
}
