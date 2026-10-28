#include "net_internal.h"
#include "session.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace hubnet {

const char* default_server() { return "wss://smash-netplay.jumpcircuit.workers.dev"; }

bool is_production_server(const std::string& url)
{
    return url.find("smash-netplay.jumpcircuit.workers.dev") != std::string::npos;
}

bool is_qa_run() { return std::getenv("RETROHUB_TEST_SERVER") || std::getenv("RETROHUB_AUTOTEST"); }

std::string test_server_or_exit(int argc, char** argv)
{
    const char* env = std::getenv("RETROHUB_TEST_SERVER");
    std::string s = argc > 1 ? std::string(argv[1]) : std::string(env && *env ? env : "");
    if (s.empty()) {
        fprintf(stderr, "ERROR: online test needs a LOCAL server. Start `npx wrangler dev --ip 127.0.0.1 --port 8761 --persist-to <fresh dir>` "
                        "in BattleShip\\server\\cloudflare and set RETROHUB_TEST_SERVER=ws://127.0.0.1:8761 (never production).\n");
        exit(2);
    }
    const char* allow = std::getenv("RETROHUB_ALLOW_PROD_TESTS");
    if (is_production_server(s) && !(allow && allow[0] == '1')) {
        fprintf(stderr, "ERROR: refusing to run tests against production (%s): they create throwaway accounts auto-friended to the owner. "
                        "Use a local server, or set RETROHUB_ALLOW_PROD_TESTS=1 if you really mean it.\n", s.c_str());
        exit(2);
    }
    return s;
}

uint32_t now_ms()
{
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   // [A-HJ-NP-Z2-9]

std::string new_room_code()
{
    std::random_device rd;
    std::string s;
    for (int i = 0; i < 6; i++) s += kAlphabet[rd() % (sizeof(kAlphabet) - 1)];
    return s;
}

bool valid_room_code(const std::string& code)
{
    if (code.size() != 6) return false;
    for (char c : code)
        if (!memchr(kAlphabet, c, sizeof(kAlphabet) - 1)) return false;
    return true;
}

namespace detail {

std::string url_encode(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string hub_url(const std::string& server, const std::string& room, bool host, const std::string& name, int slot, bool watch)
{
    std::string base = server;
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string u = base + "/hub?room=" + url_encode(room) + "&role=" + (watch ? "watch" : host ? "host" : "join") +
                    "&name=" + url_encode(name);
    if (!host && !watch && slot > 0) u += "&slot=" + std::to_string(slot);
    return u;
}

} // namespace detail
} // namespace hubnet
