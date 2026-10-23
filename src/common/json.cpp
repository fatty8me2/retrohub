#include "json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {

const Value* Value::get(const std::string& key) const {
    if (type != Object) return nullptr;
    for (const auto& kv : o)
        if (kv.first == key) return &kv.second;
    return nullptr;
}
std::string Value::str(const std::string& key, const std::string& def) const {
    const Value* v = get(key);
    return (v && v->type == String) ? v->s : def;
}
double Value::num(const std::string& key, double def) const {
    const Value* v = get(key);
    return (v && v->type == Number) ? v->n : def;
}
int64_t Value::i64(const std::string& key, int64_t def) const {
    const Value* v = get(key);
    return (v && v->type == Number) ? v->i : def;
}
bool Value::boolean(const std::string& key, bool def) const {
    const Value* v = get(key);
    return (v && v->type == Bool) ? v->b : def;
}

namespace {

void put_utf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) s.push_back((char)cp);
    else if (cp < 0x800) {
        s.push_back((char)(0xC0 | (cp >> 6)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back((char)(0xE0 | (cp >> 12)));
        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        s.push_back((char)(0xF0 | (cp >> 18)));
        s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

struct Parser {
    const char* p;
    const char* e;
    int max_depth;

    void ws() {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool hex4(uint32_t* v) {
        if (e - p < 4) return false;
        uint32_t r = 0;
        for (int k = 0; k < 4; ++k) {
            char c = *p++;
            r <<= 4;
            if (c >= '0' && c <= '9') r |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') r |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') r |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        *v = r;
        return true;
    }
    bool string(std::string* out) {
        if (p >= e || *p != '"') return false;
        ++p;
        out->clear();
        while (p < e) {
            char c = *p++;
            if (c == '"') return true;
            if (c != '\\') { out->push_back(c); continue; }
            if (p >= e) return false;
            char x = *p++;
            switch (x) {
            case 'n': out->push_back('\n'); break;
            case 't': out->push_back('\t'); break;
            case 'r': out->push_back('\r'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'u': {
                uint32_t cp;
                if (!hex4(&cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00) {
                    uint32_t lo = 0;
                    if (e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        const char* save = p;
                        p += 2;
                        if (hex4(&lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else { p = save; cp = 0xFFFD; }   // keep the following escape for the next turn
                    } else cp = 0xFFFD;
                } else if (cp >= 0xDC00 && cp < 0xE000) cp = 0xFFFD;
                put_utf8(*out, cp);
                break;
            }
            default: out->push_back(x); break;   // " \ / and unknown escapes pass through
            }
        }
        return false;   // unterminated
    }
    bool number(Value* v) {
        const char* st = p;
        if (p < e && *p == '-') ++p;
        if (p >= e || *p < '0' || *p > '9') return false;
        bool isf = false;
        while (p < e && *p >= '0' && *p <= '9') ++p;
        if (p < e && *p == '.') {
            isf = true;
            ++p;
            while (p < e && *p >= '0' && *p <= '9') ++p;
        }
        if (p < e && (*p == 'e' || *p == 'E')) {
            isf = true;
            ++p;
            if (p < e && (*p == '+' || *p == '-')) ++p;
            while (p < e && *p >= '0' && *p <= '9') ++p;
        }
        v->raw.assign(st, p);
        v->type = Value::Number;
        v->n = std::strtod(v->raw.c_str(), nullptr);
        if (!std::isfinite(v->n)) v->n = 0;
        v->is_int = !isf;
        if (!isf) {
            errno = 0;
            v->i = (int64_t)std::strtoll(v->raw.c_str(), nullptr, 10);   // saturates on overflow
        } else {
            v->i = (v->n >= 9.2e18) ? INT64_MAX : (v->n <= -9.2e18) ? INT64_MIN : (int64_t)v->n;
        }
        return true;
    }
    bool lit(const char* w, size_t n) {
        if ((size_t)(e - p) >= n && !std::memcmp(p, w, n)) { p += n; return true; }
        return false;
    }
    bool value(Value* v, int depth) {
        ws();
        if (p >= e) return false;
        char c = *p;
        if ((c == '{' || c == '[') && depth >= max_depth) return false;
        if (c == '{') {
            ++p;
            v->type = Value::Object;
            ws();
            if (p < e && *p == '}') { ++p; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!string(&k)) return false;
                ws();
                if (p >= e || *p++ != ':') return false;
                Value child;
                if (!value(&child, depth + 1)) return false;
                v->o.emplace_back(std::move(k), std::move(child));
                ws();
                if (p >= e) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == '}') { ++p; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++p;
            v->type = Value::Array;
            ws();
            if (p < e && *p == ']') { ++p; return true; }
            for (;;) {
                Value child;
                if (!value(&child, depth + 1)) return false;
                v->a.push_back(std::move(child));
                ws();
                if (p >= e) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == ']') { ++p; return true; }
                return false;
            }
        }
        if (c == '"') { v->type = Value::String; return string(&v->s); }
        if (lit("true", 4)) { v->type = Value::Bool; v->b = true; return true; }
        if (lit("false", 5)) { v->type = Value::Bool; v->b = false; return true; }
        if (lit("null", 4)) { v->type = Value::Null; return true; }
        return number(v);
    }
};

}  // namespace

bool parse(const char* data, size_t len, Value* out, const ParseOptions& opt) {
    if (!out) return false;
    *out = Value();
    try {
        if (!data) return false;
        const char* b = data;
        const char* e = data + len;
        if (len >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) b += 3;
        Parser ps{b, e, opt.max_depth};
        Value v;
        if (!ps.value(&v, 0)) return false;
        if (!opt.allow_trailing) {
            ps.ws();
            if (ps.p != e) return false;
        }
        *out = std::move(v);
        return true;
    } catch (...) {
        *out = Value();
        return false;
    }
}

bool parse(const std::string& text, Value* out, const ParseOptions& opt) {
    return parse(text.data(), text.size(), out, opt);
}

std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o.push_back((char)c);
        }
    }
    return o;
}

std::string quote(const std::string& s) { return "\"" + escape(s) + "\""; }

std::string number_to_string(double d) {
    if (!std::isfinite(d)) return "null";
    char b[40];
    if (d == std::floor(d) && std::fabs(d) < 9e15) std::snprintf(b, sizeof b, "%.0f", d);
    else std::snprintf(b, sizeof b, "%.17g", d);
    return b;
}

static void write_value(const Value& v, std::string& o, int depth) {
    if (depth > 256) { o += "null"; return; }
    switch (v.type) {
    case Value::Null: o += "null"; break;
    case Value::Bool: o += v.b ? "true" : "false"; break;
    case Value::Number:
        if (v.is_int) o += std::to_string(v.i);
        else o += number_to_string(v.n);
        break;
    case Value::String: o += quote(v.s); break;
    case Value::Array:
        o += '[';
        for (size_t k = 0; k < v.a.size(); ++k) { if (k) o += ','; write_value(v.a[k], o, depth + 1); }
        o += ']';
        break;
    case Value::Object:
        o += '{';
        for (size_t k = 0; k < v.o.size(); ++k) {
            if (k) o += ',';
            o += quote(v.o[k].first);
            o += ':';
            write_value(v.o[k].second, o, depth + 1);
        }
        o += '}';
        break;
    }
}

std::string stringify(const Value& v) {
    std::string o;
    try { write_value(v, o, 0); } catch (...) { return "null"; }
    return o;
}

Value make_null() { return Value(); }
Value make_bool(bool b) { Value v; v.type = Value::Bool; v.b = b; return v; }
Value make_number(double d) { Value v; v.type = Value::Number; v.n = d; v.i = (int64_t)d; return v; }
Value make_int(int64_t i) { Value v; v.type = Value::Number; v.i = i; v.n = (double)i; v.is_int = true; return v; }
Value make_string(const std::string& s) { Value v; v.type = Value::String; v.s = s; return v; }
Value make_array() { Value v; v.type = Value::Array; return v; }
Value make_object() { Value v; v.type = Value::Object; return v; }

}  // namespace json
