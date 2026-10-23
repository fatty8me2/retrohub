/* json: parser + writer. Console test. */
#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    using json::Value;
    Value v;

    /* basics */
    CHECK(json::parse("{\"a\":1,\"b\":[true,false,null,\"x\"],\"c\":{\"d\":-2.5e1}}", &v));
    CHECK(v.is_object() && v.num("a") == 1 && v.i64("a") == 1);
    CHECK(v.get("b") && v.get("b")->a.size() == 4 && v.get("b")->a[0].b && !v.get("b")->a[1].b && v.get("b")->a[2].is_null());
    CHECK(v.get("c")->num("d") == -25.0 && !v.get("c")->get("d")->is_int);
    CHECK(v.str("zz", "def") == "def" && v.str("a", "def") == "def" && !v.get("nope"));
    CHECK(json::parse("  [ ]  ", &v) && v.is_array() && v.a.empty());
    CHECK(json::parse("{}", &v) && v.is_object() && v.o.empty());
    CHECK(json::parse("\xEF\xBB\xBF{\"k\":1}", &v) && v.num("k") == 1);   // BOM

    /* escapes + surrogates */
    CHECK(json::parse("\"a\\\"b\\n\\u00e9\\ud83d\\ude00\\\\\\/\"", &v) && v.s == std::string("a\"b\n\xC3\xA9\xF0\x9F\x98\x80\\/"));
    CHECK(json::parse("\"\\ud83dX\"", &v) && v.s == std::string("\xEF\xBF\xBD" "X"));        // lone high surrogate
    CHECK(json::parse("\"\\ude00\"", &v) && v.s == "\xEF\xBF\xBD");                           // lone low surrogate
    CHECK(json::parse("\"\\ud83d\\u0041\"", &v) && v.s == std::string("\xEF\xBF\xBD" "A"));   // high + non-low
    CHECK(json::parse("\"\\u0000\"", &v) && v.s.size() == 1 && v.s[0] == '\0');
    CHECK(!json::parse("\"\\u12G4\"", &v));
    CHECK(!json::parse("\"\\u12", &v));

    /* round trip */
    {
        std::string src = "{\"s\":\"tab\\t nl\\n q\\\" bs\\\\ \\u0001 \xC3\xA9\",\"n\":[1,-7,2.5,12345678901234],\"t\":true,\"z\":null,\"e\":{}}";
        Value a, b;
        CHECK(json::parse(src, &a));
        std::string out = json::stringify(a);
        CHECK(json::parse(out, &b));
        CHECK(json::stringify(b) == out);
        CHECK(b.str("s") == a.str("s"));
        CHECK(json::escape("a\"b\\\n\x01") == "a\\\"b\\\\\\n\\u0001");
        CHECK(json::quote("x") == "\"x\"");
        Value o = json::make_object();
        o.o.emplace_back("k", json::make_string("v\n"));
        o.o.emplace_back("i", json::make_int(-5));
        o.o.emplace_back("d", json::make_number(0.5));
        o.o.emplace_back("l", json::make_array());
        o.o.emplace_back("b", json::make_bool(true));
        CHECK(json::stringify(o) == "{\"k\":\"v\\n\",\"i\":-5,\"d\":0.5,\"l\":[],\"b\":true}");
    }

    /* big numbers */
    CHECK(json::parse("[9223372036854775807,-9223372036854775808,18446744073709551615,1e400,12345678901234567890123]", &v));
    CHECK(v.a[0].is_int && v.a[0].i == INT64_MAX);
    CHECK(v.a[1].i == INT64_MIN);
    CHECK(v.a[2].i == INT64_MAX);          // saturates, never wraps
    CHECK(v.a[3].n == 0);                  // not finite -> 0, no inf leaks out
    CHECK(v.a[4].raw == "12345678901234567890123");
    CHECK(json::parse("[1.5e3,2E-1,-0]", &v) && v.a[0].n == 1500 && v.a[0].i == 1500 && v.a[1].n == 0.2);
    CHECK(json::stringify(json::make_number(1e300 * 1e300)) == "null");

    /* malformed input never crashes, returns false and leaves Null */
    const char* bad[] = {"", " ", "{", "}", "[", "[1,", "[1 2]", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{a:1}", "\"abc", "tru", "nul",
                         "-", "+1", ".5", "0x10", "nan", "inf", "[1,]x", "{\"a\":1} x", "\"\\", "{\"a\":[}", "]"};
    for (const char* b : bad) {
        Value x = json::make_bool(true);
        CHECK(!json::parse(b, &x));
        CHECK(x.is_null());
    }
    CHECK(!json::parse(nullptr, 0, &v));
    CHECK(json::parse("{\"a\":1} trailing", &v, json::ParseOptions{64, true}) && v.num("a") == 1);
    CHECK(!json::parse("1 2", &v) && json::parse("1 2", &v, json::ParseOptions{64, true}) && v.num("x", 9) == 9 && v.n == 1);
    {   // truncations of a valid document all fail cleanly
        std::string doc = "{\"a\":[1,2,{\"b\":\"c\\u00e9\"}],\"d\":true}";
        for (size_t k = 0; k < doc.size(); ++k) { Value x; CHECK(!json::parse(doc.substr(0, k), &x)); }
        CHECK(json::parse(doc, &v));
    }

    /* nesting limit */
    {
        std::string deep(1000, '['), close(1000, ']');
        CHECK(!json::parse(deep + close, &v));
        std::string deep_o;
        for (int k = 0; k < 500; ++k) deep_o += "{\"a\":";
        CHECK(!json::parse(deep_o + "1" + std::string(500, '}'), &v));
        std::string ok(64, '[');
        CHECK(json::parse(ok + std::string(64, ']'), &v));            // exactly the default limit
        CHECK(!json::parse(std::string(65, '[') + std::string(65, ']'), &v));
        CHECK(json::parse(std::string(65, '[') + std::string(65, ']'), &v, json::ParseOptions{100, false}));
        CHECK(!json::parse("[[1]]", &v, json::ParseOptions{1, false}));
        CHECK(json::parse("[1]", &v, json::ParseOptions{1, false}));
        Value d = json::make_array();   // writer survives absurd depth
        Value* cur = &d;
        for (int k = 0; k < 300; ++k) { cur->a.push_back(json::make_array()); cur = &cur->a[0]; }
        CHECK(!json::stringify(d).empty());
    }

    /* duplicate keys: first wins; typed lookups */
    CHECK(json::parse("{\"a\":1,\"a\":2,\"b\":\"s\",\"c\":false}", &v) && v.num("a") == 1 && v.num("b", 7) == 7 && !v.boolean("c", true) && v.boolean("zz", true));

    if (g_fail) { std::printf("json_test: %d FAILED\n", g_fail); return 1; }
    std::printf("json_test: all passed\n");
    return 0;
}
