#include "netplay_3ds.h"
#include <fstream>
#include <iterator>

namespace hubnp3ds {

std::string plan_json_fragment(const Plan& p) {
    return ",\"np_3ds\":{\"port\":" + std::to_string(p.port) + ",\"pw\":\"" + json::escape(p.password) +
           "\",\"n\":" + std::to_string(p.expected) + "}";
}

bool parse_plan(const hubui::Json& m, Plan* out) {
    const hubui::Json* o = m.get("np_3ds");
    if (!o || o->type != hubui::Json::Object) return false;
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

namespace hubnp3ds {

std::string mute_config(const std::string& in) {
    std::string t = in;
    auto set = [&](const char* sec, const char* key, const char* v) {
        t = hubui::ini_set(t, sec, std::string(key) + "\\default", "false");
        t = hubui::ini_set(t, sec, key, v);
    };
    set("Audio", "output_type", "1");
    set("Audio", "volume", "0");
    set("UI", "confirmClose", "false");
    set("UI", "check_for_update_on_start", "false");
    set("UI", "firstStart", "false");
    set("UI", "showConsole", "false");
    return t;
}

}  // namespace hubnp3ds

namespace hubnp3ds {

/* The 3DS username lives in the config savegame (NAND sysdata 00010017), block 0x000A0000: char16_t[10] + 8 bytes.
 * Layout: u16 entry count, u16 data start, then {u32 id, u32 offset-or-data, u16 size, u16 flags} entries. */
std::string console_username(const std::string& player, int slot) {
    std::string s;
    for (unsigned char c : player)
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-') s += (char)c;
        else if (c == ' ') s += '_';
    if (s.size() > 8) s.resize(8);
    std::string suf = "_" + std::to_string(slot < 0 ? 0 : slot % 10);
    s += suf;
    while (s.size() < 4) s.insert(s.size() - suf.size(), "x");
    return s;
}

static uint32_t rd32(const std::vector<uint8_t>& b, size_t o) { return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24); }

bool set_console_username(std::vector<uint8_t>* cfg, const std::string& name) {
    std::vector<uint8_t>& b = *cfg;
    if (b.size() < 4 || name.empty() || name.size() > 10) return false;
    size_t n = b[0] | (b[1] << 8);
    if (4 + n * 12 > b.size()) return false;
    for (size_t i = 0; i < n; ++i) {
        size_t e = 4 + i * 12;
        if (rd32(b, e) != 0x000A0000) continue;
        size_t off = rd32(b, e + 4), size = b[e + 8] | (b[e + 9] << 8);
        if (size < 20 || off + size > b.size()) return false;
        for (size_t k = 0; k < 10; ++k) {
            uint8_t c = k < name.size() ? (uint8_t)name[k] : 0;
            b[off + k * 2] = c;
            b[off + k * 2 + 1] = 0;
        }
        return true;
    }
    return false;
}

std::string console_config_path(const std::string& install_dir) {
    return install_dir + "\\user\\nand\\data\\00000000000000000000000000000000\\sysdata\\00010017\\00000000\\config";
}

bool set_console_username_file(const std::string& install_dir, const std::string& name) {
    std::string p = console_config_path(install_dir);
    std::vector<uint8_t> b;
    {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (!set_console_username(&b, name)) return false;
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write((const char*)b.data(), (std::streamsize)b.size());
    return (bool)o;
}

}  // namespace hubnp3ds
