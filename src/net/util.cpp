#include "net_internal.h"
#include "session.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>

namespace hubnet {

const char* default_server() { return "wss://smash-netplay.jumpcircuit.workers.dev"; }

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

std::string hub_url(const std::string& server, const std::string& room, bool host, const std::string& name, int slot)
{
    std::string base = server;
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string u = base + "/hub?room=" + url_encode(room) + "&role=" + (host ? "host" : "join") +
                    "&name=" + url_encode(name);
    if (!host && slot > 0) u += "&slot=" + std::to_string(slot);
    return u;
}

} // namespace detail
} // namespace hubnet
