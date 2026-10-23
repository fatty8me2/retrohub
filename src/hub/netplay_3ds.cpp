#include "netplay_3ds.h"

namespace hubnp3ds {

std::string plan_json_fragment(const Plan& p) {
    return ",\"np_3ds\":{\"port\":" + std::to_string(p.port) + ",\"pw\":\"" + hubui::json_escape(p.password) +
           "\",\"n\":" + std::to_string(p.expected) + "}";
}

bool parse_plan(const hubui::Json& m, Plan* out) {
    const hubui::Json* o = m.get("np_3ds");
    if (!o || o->type != hubui::Json::Obj) return false;
    double port = o->num("port", -1), n = o->num("n", -1);
    if (port < 1024 || port > 65535 || port != (double)(int)port) return false;
    if (n < 2 || n > kMaxMembers || n != (double)(int)n) return false;
    std::string pw = o->str("pw");
    if (pw.size() > 32) return false;
    for (char c : pw)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;   // would break "-m nick:pw@host"
    out->port = (int)port;
    out->password = pw;
    out->expected = (int)n;
    return true;
}

std::string random_password(unsigned seed) {
    static const char a[] = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    uint64_t x = seed * 6364136223846793005ull + 1442695040888963407ull;
    std::string s;
    for (int i = 0; i < 12; ++i) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        s += a[(x >> 33) % (sizeof a - 1)];
    }
    return s;
}

std::string nickname(const std::string& name, int slot) {
    std::string s;
    for (unsigned char c : name) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-') s += (char)c;
        else if (c == ' ') s += '_';
    }
    if (s.size() > 15) s.resize(15);
    std::string suf = "_" + std::to_string(slot + 1);
    s += suf;
    while (s.size() < 4) s.insert(s.size() - suf.size(), "x");
    return s;
}

static std::string q(const std::string& s) { return "\"" + s + "\""; }

std::string room_args(int port, const std::string& pw, int max_members, const std::string& log_file, const std::string& ban_file) {
    std::string a = "--port " + std::to_string(port) + " --room-name RetroHub --max_members " + std::to_string(max_members) +
                    " --preferred-app RetroHub --preferred-app-id 0";
    if (!pw.empty()) a += " --password " + pw;
    if (!log_file.empty()) a += " --log-file " + q(log_file);
    if (!ban_file.empty()) a += " --ban-list-file " + q(ban_file);
    return a;
}

std::string multiplayer_arg(const std::string& nick, const std::string& pw, const std::string& host, int port) {
    return "-m " + q(nick + (pw.empty() ? "" : ":" + pw) + "@" + host + ":" + std::to_string(port));
}

std::string session_room(const std::string& code, int launch_no) { return code + "-" + std::to_string(launch_no); }   // the Worker only accepts CODE or CODE-N

std::string config_patch(const std::string& in, const std::string& host, int port, const std::string& nick) {
    std::string t = in;
    auto set = [&](const std::string& key, const std::string& val) {
        t = hubui::ini_set(t, "UI", "Multiplayer\\" + key + "\\default", "false");
        t = hubui::ini_set(t, "UI", "Multiplayer\\" + key, val);
    };
    set("ip", host);
    set("port", std::to_string(port));
    set("nickname", nick);
    return t;
}

std::vector<std::string> fragment(uint16_t id, const uint8_t* d, size_t len) {
    std::vector<std::string> v;
    size_t cnt = len == 0 ? 1 : (len + kFragPayload - 1) / kFragPayload;
    if (cnt > 255) return v;
    for (size_t i = 0; i < cnt; ++i) {
        size_t off = i * kFragPayload, n = len - off < (size_t)kFragPayload ? len - off : (size_t)kFragPayload;
        std::string f;
        f += 'F';
        f += (char)(id & 0xff);
        f += (char)(id >> 8);
        f += (char)i;
        f += (char)cnt;
        f.append((const char*)d + off, len ? n : 0);
        v.push_back(std::move(f));
    }
    return v;
}

bool Reassembler::add(int from, const uint8_t* f, size_t len, uint32_t now, std::string* whole) {
    for (auto it = parts_.begin(); it != parts_.end();) it = (now - it->second.t > 2000) ? parts_.erase(it) : std::next(it);
    if (len < (size_t)kFragHeader || f[0] != 'F') return false;
    int id = f[1] | (f[2] << 8), idx = f[3], cnt = f[4];
    if (cnt < 1 || idx >= cnt) return false;
    if (cnt == 1) { whole->assign((const char*)f + kFragHeader, len - kFragHeader); return true; }
    uint64_t key = ((uint64_t)(unsigned)from << 16) | (uint64_t)id;
    Part& p = parts_[key];
    if (p.count != cnt) { p = Part(); p.count = cnt; p.piece.assign((size_t)cnt, std::string()); }
    p.t = now;
    if (p.piece[(size_t)idx].empty()) { p.piece[(size_t)idx].assign((const char*)f + kFragHeader, len - kFragHeader); ++p.have; }
    if (p.have < cnt) return false;
    whole->clear();
    for (auto& s : p.piece) *whole += s;
    parts_.erase(key);
    return true;
}

}  // namespace hubnp3ds
