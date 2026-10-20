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

std::string json_escape(const std::string& s)
{
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else o += (char)c;
        }
    }
    return o;
}

const Json* Json::get(const char* key) const
{
    if (type != Object) return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &it->second;
}
std::string Json::s(const char* key, const std::string& def) const
{
    const Json* j = get(key);
    return (j && j->type == String) ? j->str : def;
}
int Json::i(const char* key, int def) const
{
    const Json* j = get(key);
    return (j && j->type == Number) ? (int)j->num : def;
}

namespace {
struct Parser {
    const std::string& t;
    size_t p = 0;
    int depth = 0;
    explicit Parser(const std::string& s) : t(s) {}
    void ws() { while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) p++; }
    bool value(Json* out)
    {
        ws();
        if (p >= t.size() || ++depth > 32) return false;
        size_t start = p;
        bool ok = false;
        char c = t[p];
        if (c == '{') {
            out->type = Json::Object;
            p++;
            ws();
            if (p < t.size() && t[p] == '}') { p++; ok = true; }
            else {
                for (;;) {
                    ws();
                    Json k;
                    if (p >= t.size() || t[p] != '"' || !string(&k.str)) break;
                    ws();
                    if (p >= t.size() || t[p] != ':') break;
                    p++;
                    Json v;
                    if (!value(&v)) break;
                    out->obj[k.str] = std::move(v);
                    ws();
                    if (p < t.size() && t[p] == ',') { p++; continue; }
                    if (p < t.size() && t[p] == '}') { p++; ok = true; }
                    break;
                }
            }
        } else if (c == '[') {
            out->type = Json::Array;
            p++;
            ws();
            if (p < t.size() && t[p] == ']') { p++; ok = true; }
            else {
                for (;;) {
                    Json v;
                    if (!value(&v)) break;
                    out->arr.push_back(std::move(v));
                    ws();
                    if (p < t.size() && t[p] == ',') { p++; continue; }
                    if (p < t.size() && t[p] == ']') { p++; ok = true; }
                    break;
                }
            }
        } else if (c == '"') {
            out->type = Json::String;
            ok = string(&out->str);
        } else if (t.compare(p, 4, "true") == 0) { out->type = Json::Bool; out->b = true; p += 4; ok = true; }
        else if (t.compare(p, 5, "false") == 0) { out->type = Json::Bool; p += 5; ok = true; }
        else if (t.compare(p, 4, "null") == 0) { out->type = Json::Null; p += 4; ok = true; }
        else {
            char* end = nullptr;
            out->num = strtod(t.c_str() + p, &end);
            if (end && end > t.c_str() + p) { out->type = Json::Number; p = end - t.c_str(); ok = true; }
        }
        depth--;
        if (ok) out->raw = t.substr(start, p - start);
        return ok;
    }
    bool string(std::string* out)
    {
        p++;   // opening quote
        out->clear();
        while (p < t.size()) {
            char c = t[p++];
            if (c == '"') return true;
            if (c != '\\') { *out += c; continue; }
            if (p >= t.size()) return false;
            char e = t[p++];
            switch (e) {
            case 'n': *out += '\n'; break;
            case 'r': *out += '\r'; break;
            case 't': *out += '\t'; break;
            case 'b': *out += '\b'; break;
            case 'f': *out += '\f'; break;
            case 'u': {
                if (p + 4 > t.size()) return false;
                unsigned cp = (unsigned)strtoul(t.substr(p, 4).c_str(), nullptr, 16);
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && t.compare(p, 2, "\\u") == 0 && p + 6 <= t.size()) {
                    unsigned lo = (unsigned)strtoul(t.substr(p + 2, 4).c_str(), nullptr, 16);
                    p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                if (cp < 0x80) *out += (char)cp;
                else if (cp < 0x800) { *out += (char)(0xC0 | (cp >> 6)); *out += (char)(0x80 | (cp & 63)); }
                else if (cp < 0x10000) { *out += (char)(0xE0 | (cp >> 12)); *out += (char)(0x80 | ((cp >> 6) & 63)); *out += (char)(0x80 | (cp & 63)); }
                else { *out += (char)(0xF0 | (cp >> 18)); *out += (char)(0x80 | ((cp >> 12) & 63)); *out += (char)(0x80 | ((cp >> 6) & 63)); *out += (char)(0x80 | (cp & 63)); }
                break;
            }
            default: *out += e;   // " \ /
            }
        }
        return false;
    }
};
} // namespace

bool json_parse(const std::string& text, Json* out)
{
    Parser ps(text);
    *out = Json();
    return ps.value(out);
}

} // namespace detail
} // namespace hubnet
