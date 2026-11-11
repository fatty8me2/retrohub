// Fuzz-style hostile-input test for the cloud wire parsers (cloud_wire.cpp): malformed, truncated, mistyped, huge and NUL-carrying server
// replies must be rejected or tolerated WITHOUT crashing, over-reading, or letting an unsafe path through. Deterministic (fixed seeds), offline.
#include "cloud_internal.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
using namespace hub::saves;
using str = std::string;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static str hex64() { return str(64, 'a'); }

static str valid_manifest() {
    return "{\"ok\":true,\"rev\":412,\"full\":false,\"account\":{\"used\":8123456,\"protUsed\":1204433,\"cap\":52428800,\"protCap\":20971520,"
           "\"devices\":2,\"pool\":\"tight\",\"paused\":false},\"games\":{\"n64/smash-64\":{\"playedAt\":1760000000,\"device\":\"PC-DESK\","
           "\"pin\":true,\"rev\":411,\"lease\":{\"dev\":\"d-ab12cd34\",\"name\":\"PC-LIVING\",\"until\":1760000900},\"files\":{"
           "\"battery/ssb64.sra\":{\"cur\":7,\"cloud\":\"synced\",\"vers\":[{\"ver\":7,\"sha\":\"" + hex64() + "\",\"size\":32768,\"stored\":1210,"
           "\"enc\":\"deflate\",\"mtime\":1760000000,\"dev\":\"d-ab12cd34\",\"devName\":\"PC-DESK\",\"ts\":1760000010,\"class\":\"battery\"}]},"
           "\"states/s0.state\":{\"cur\":2,\"cloud\":\"evicted\",\"vers\":[]}}}}}";
}
static str valid_plan() {
    return "{\"ok\":true,\"ticket\":\"T1\",\"files\":[{\"path\":\"a\",\"action\":\"upload\"},{\"path\":\"b\",\"action\":\"skip\",\"reason\":\"over-cap\"},"
           "{\"path\":\"c\",\"action\":\"conflict\",\"current\":{\"ver\":4,\"sha\":\"" + hex64() + "\",\"mtime\":7}}]}";
}
static str valid_commit() {
    return "{\"ok\":true,\"rev\":9,\"results\":[{\"path\":\"a\",\"ver\":2,\"cloud\":\"synced\"},{\"path\":\"b\",\"conflict\":true}],"
           "\"evicted\":[{\"game\":\"n64/y\",\"path\":\"states/s\"}]}";
}
static str valid_lease() { return "{\"ok\":true,\"held\":{\"dev\":\"d1\",\"name\":\"PC\",\"since\":5,\"until\":99}}"; }
static str valid_error() { return "{\"error\":\"rate-limited\",\"retryAfter\":30}"; }

/* A path that came out of parse_manifest must be safe to join under the vault. */
static bool safe_rel(const str& p) {
    if (p.empty() || p.size() > 300 || p[0] == '/' || p[0] == '\\') return false;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == str::npos) j = p.size();
        str seg = p.substr(i, j - i);
        if (seg.empty() || seg == "." || seg == "..") return false;
        i = j + 1;
    }
    for (unsigned char c : p) if (c < 0x20 || c == 0x7f || c == ':' || c == '\\') return false;
    str h = p.substr(0, 9);
    for (auto& c : h) c = (char)tolower((unsigned char)c);
    if (h == "_identity" && (p.size() == 9 || p[9] == '/')) return false;
    return true;
}
static bool manifest_safe(const WManifest& m) {
    for (auto& g : m.games) {
        if (!safe_rel(g.gkey)) return false;
        for (auto& f : g.files) {
            if (!safe_rel(f.path)) return false;
            for (auto& v : f.vers) {
                if (v.ver <= 0) return false;
                if (!f.evicted() && v.sha.size() != 64) return false;
            }
        }
    }
    return true;
}

/* Runs every parser on one input; returns whether the manifest parser accepted it. Only crashes / unsafe output are failures. */
static bool feed_all(const str& in) {
    WManifest m; PlanReply p; CommitReply c; LeaseReply l; str err;
    bool ok = parse_manifest(in, &m, &err);
    if (ok && !manifest_safe(m)) { std::printf("UNSAFE manifest accepted (%zu bytes)\n", in.size()); ++g_fail; }
    parse_plan(in, &p, &err);
    parse_commit(in, &c, &err);
    parse_lease(in, &l);
    ServerError e = parse_error(in);
    (void)e;
    return ok;
}

static uint32_t g_seed = 1;
static uint32_t rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }

int main() {
    const str man = valid_manifest(), plan = valid_plan(), commit = valid_commit(), lease = valid_lease(), error = valid_error();
    {   // sanity: the valid documents parse (so the fuzz below starts from a real shape)
        WManifest m; PlanReply p; CommitReply c; LeaseReply l; str err;
        CHECK(parse_manifest(man, &m, &err) && manifest_safe(m) && m.games.size() == 1);
        CHECK(parse_plan(plan, &p, &err) && p.files.size() == 3);
        CHECK(parse_commit(commit, &c, &err) && c.results.size() == 2);
        CHECK(parse_lease(lease, &l) && l.held && l.name == "PC");
        CHECK(parse_error(error).code == "rate-limited" && parse_error(error).retry_after == 30);
    }
    {   // every proper prefix of a valid document is rejected by its parser (truncated reply), and none crashes
        for (size_t n = 0; n < man.size(); ++n) { WManifest m; str err; CHECK(!parse_manifest(man.substr(0, n), &m, &err)); feed_all(man.substr(0, n)); }
        for (size_t n = 0; n < plan.size(); ++n) { PlanReply p; str err; CHECK(!parse_plan(plan.substr(0, n), &p, &err)); feed_all(plan.substr(0, n)); }
        for (size_t n = 0; n < commit.size(); ++n) { CommitReply c; str err; CHECK(!parse_commit(commit.substr(0, n), &c, &err)); feed_all(commit.substr(0, n)); }
        for (size_t n = 0; n < lease.size(); ++n) { LeaseReply l; CHECK(!parse_lease(lease.substr(0, n), &l)); feed_all(lease.substr(0, n)); }
        for (size_t n = 0; n < error.size(); ++n) { CHECK(parse_error(error.substr(0, n)).code.empty()); }
    }
    {   // wrong root types
        const char* roots[] = {"", " ", "null", "true", "false", "0", "-1", "1e999", "\"str\"", "[]", "[1,2,3]", "[{}]", "{}", "{", "}", "{\"a\"", "{\"a\":", "{\"a\":}",
                               "{,}", "[,]", "\x01", "\xff\xfe\xfd", "{\"games\":[]}", "{\"games\":5}", "{\"games\":null}", "{\"games\":\"x\"}"};
        for (const char* r : roots) feed_all(r);
        WManifest m; str err;
        CHECK(!parse_manifest("[]", &m, &err) && !parse_manifest("null", &m, &err) && !parse_manifest("\"x\"", &m, &err) && !parse_manifest("", &m, &err));
        CHECK(!parse_manifest("{\"games\":[]}", &m, &err));     // games of the wrong type is a bad manifest, not an empty one
        CHECK(!parse_manifest("{\"games\":5}", &m, &err));
        PlanReply p; CommitReply c; LeaseReply l;
        CHECK(!parse_plan("[]", &p, &err) && !parse_plan("{}", &p, &err) && !parse_plan("{\"files\":{}}", &p, &err) && !parse_plan("{\"files\":\"x\"}", &p, &err));
        CHECK(!parse_commit("[]", &c, &err) && !parse_commit("null", &c, &err));
        CHECK(!parse_lease("[]", &l) && !parse_lease("", &l));
        CHECK(parse_error("[]").code.empty() && parse_error("").code.empty() && parse_error("{\"error\":5}").code == "5");
    }
    {   // mistyped fields inside a good shape: tolerated or skipped, never a crash, never an unsafe path
        const char* docs[] = {
            "{\"rev\":\"x\",\"full\":\"yes\",\"account\":5,\"games\":{}}",
            "{\"account\":[],\"games\":{\"g\":5}}",
            "{\"games\":{\"g\":{\"files\":5}}}",
            "{\"games\":{\"g\":{\"files\":[]}}}",
            "{\"games\":{\"g\":{\"files\":{\"f\":5}}}}",
            "{\"games\":{\"g\":{\"files\":{\"f\":{\"vers\":{}}}}}}",
            "{\"games\":{\"g\":{\"files\":{\"f\":{\"vers\":[5,null,\"x\",[]]}}}}}",
            "{\"games\":{\"g\":{\"files\":{\"f\":{\"cur\":\"7\",\"cloud\":5,\"vers\":[{\"ver\":\"1\",\"sha\":5}]}}}}}",
            "{\"games\":{\"g\":{\"lease\":5,\"playedAt\":\"now\",\"pin\":\"1\",\"device\":[]}}}",
            "{\"games\":{\"g\":{\"lease\":{\"dev\":{},\"name\":[],\"since\":\"x\"}}}}",
            "{\"games\":{\"g\":{\"files\":{\"f\":{\"vers\":[{\"ver\":1,\"sha\":null,\"size\":\"big\",\"enc\":7,\"mtime\":{}}]}}}}}",
            "{\"files\":[5,null,\"x\",[],{\"path\":5,\"action\":[]},{\"path\":\"a\",\"action\":\"x\",\"current\":5}],\"ticket\":[]}",
            "{\"results\":[5,null,{\"path\":[],\"ver\":\"x\",\"cloud\":5,\"current\":[]},{\"path\":\"a\",\"conflict\":\"true\"}],\"evicted\":[5,{\"game\":5},{}]}",
            "{\"held\":5}", "{\"held\":[]}", "{\"held\":{\"until\":\"x\"},\"ok\":\"yes\"}", "{\"retryAfter\":\"soon\",\"error\":[]}",
        };
        for (const char* d : docs) feed_all(d);
        // mistyped lease/held: "held":5 is not a held lease
        LeaseReply l;
        CHECK(parse_lease("{\"held\":5}", &l) && !l.held);
    }
    {   // numbers far outside range: no overflow trap, no crash
        const char* nums[] = {"99999999999999999999", "-99999999999999999999", "9223372036854775807", "9223372036854775808", "-9223372036854775808", "1e999", "-1e999",
                              "0.5", "1e-999", "1E5", "0x10", "-", "+1", "00012", "1.", ".5", "NaN", "Infinity"};
        for (const char* n : nums) {
            str d = str("{\"rev\":") + n + ",\"account\":{\"used\":" + n + ",\"cap\":" + n + ",\"devices\":" + n + "},\"games\":{\"g\":{\"playedAt\":" + n +
                    ",\"files\":{\"f\":{\"cur\":" + n + ",\"vers\":[{\"ver\":" + n + ",\"sha\":\"" + hex64() + "\",\"size\":" + n + ",\"stored\":" + n + ",\"mtime\":" + n + "}]}}}}}";
            feed_all(d);
            feed_all(str("{\"rev\":") + n + ",\"results\":[{\"path\":\"a\",\"ver\":" + n + "}],\"files\":[{\"path\":\"a\",\"action\":\"same\",\"ver\":" + n + "}],\"retryAfter\":" + n + "}");
        }
    }
    {   // hostile paths: dropped from the manifest, the rest of the game survives
        const char* bad[] = {"../evil", "a/../b", "a/./b", "/abs", "\\\\srv\\share", "C:/x", "a\\\\b", "a//b", "a/", "/", ".", "..", "", "_identity", "_identity/key", "_IDENTITY/key",
                             "a/_identity/../..", "a:b", "a\\u0000b", "a\\u0001b", "a\\u007fb", "\\u0000"};
        for (const char* b : bad) {
            str d = "{\"games\":{\"ok/game\":{\"files\":{\"" + str(b) + "\":{\"cur\":1,\"cloud\":\"evicted\",\"vers\":[]},\"good.sav\":{\"cur\":1,\"cloud\":\"evicted\",\"vers\":[]}}},\"" + str(b) +
                    "\":{\"files\":{}}}}";
            WManifest m; str err;
            bool ok = parse_manifest(d, &m, &err);
            CHECK(ok || err == "bad manifest");   // a JSON error from the escape is fine, an accepted unsafe path is not
            if (ok) {
                CHECK(manifest_safe(m));
                CHECK(m.games.size() == 1 && m.games[0].gkey == "ok/game" && m.games[0].files.size() == 1 && m.games[0].files[0].path == "good.sav");
            }
        }
        // the same with raw NUL / control bytes in the text (not a JSON escape)
        str raw = valid_manifest();
        size_t at = raw.find("battery/ssb64.sra");
        for (int ch : {0, 1, 0x1f, 0x7f}) {
            str d = raw;
            d[at + 3] = (char)ch;
            WManifest m; str err;
            if (parse_manifest(d, &m, &err)) CHECK(manifest_safe(m));
            feed_all(d);
        }
        // a NUL in the middle of the text: std::string carries it, parsers must not stop reading at it as if it were the end
        str nul = valid_manifest();
        nul.insert(nul.size() / 2, 1, '\0');
        feed_all(nul);
        str tailnul = valid_manifest(); tailnul.push_back('\0'); tailnul += "garbage";
        feed_all(tailnul);
        WManifest m; str err;
        CHECK(!parse_manifest(str("\0{}", 3), &m, &err) || manifest_safe(m));
    }
    {   // bad hashes: a non-evicted version with a short / upper-case / non-hex sha is dropped (cannot be downloaded or trusted)
        const str shas[] = {"", "abc", str(63, 'a'), str(65, 'a'), str(64, 'G'), str(64, 'A'), str(63, 'a') + "z", str(64, '\0')};
        for (const str& s : shas) {
            str d = "{\"games\":{\"g\":{\"files\":{\"f\":{\"cur\":1,\"cloud\":\"synced\",\"vers\":[{\"ver\":1,\"sha\":\"" + s + "\"}]}}}}}";
            WManifest m; str err;
            bool ok = parse_manifest(d, &m, &err);
            if (ok) CHECK(manifest_safe(m) && m.games.size() == 1 && m.games[0].files.size() == 1 && m.games[0].files[0].vers.empty());
        }
    }
    {   // huge lengths and sizes
        str big_path(100000, 'p');
        WManifest m; str err;
        CHECK(parse_manifest("{\"games\":{\"" + big_path + "\":{\"files\":{}},\"g\":{\"files\":{\"" + big_path + "\":{\"cur\":1}}}}}", &m, &err));
        CHECK(manifest_safe(m) && m.games.size() == 1 && m.games[0].files.empty());       // over-long game key and path are dropped
        str long_name(2'000'000, 'n');                                                      // a 2 MB string value is fine to read
        feed_all("{\"games\":{\"g\":{\"device\":\"" + long_name + "\",\"files\":{}}}}");
        feed_all("{\"ticket\":\"" + long_name + "\",\"files\":[]}");
        // thousands of entries
        str many = "{\"games\":{\"g\":{\"files\":{";
        for (int i = 0; i < 5000; ++i) { if (i) many += ','; many += "\"f" + std::to_string(i) + "\":{\"cur\":1,\"cloud\":\"evicted\"}"; }
        many += "}}}}";
        CHECK(parse_manifest(many, &m, &err) && m.games.size() == 1 && m.games[0].files.size() == 5000);
        str many_p = "{\"files\":[";
        for (int i = 0; i < 20000; ++i) { if (i) many_p += ','; many_p += "{\"path\":\"p" + std::to_string(i) + "\",\"action\":\"upload\"}"; }
        many_p += "]}";
        PlanReply p;
        CHECK(parse_plan(many_p, &p, &err) && p.files.size() == 20000);
    }
    {   // nesting far deeper than any real reply: rejected (parser depth limit), no stack overflow
        for (int depth : {64, 65, 100, 1000, 100000}) {
            str a(depth, '['); a += str(depth, ']');
            feed_all(a);
            str o; for (int i = 0; i < depth; ++i) o += "{\"a\":"; o += "1"; o += str(depth, '}');
            feed_all(o);
            str g = "{\"games\":"; for (int i = 0; i < depth; ++i) g += "{\"g\":"; g += "1"; g += str(depth, '}'); g += "}";
            feed_all(g);
        }
        WManifest m; str err;
        str deep = "{\"x\":"; for (int i = 0; i < 1000; ++i) deep += "["; deep += str(1000, ']'); deep += "}";
        CHECK(!parse_manifest(deep, &m, &err));
    }
    {   // unterminated strings / escapes at the very end
        const char* docs[] = {"{\"a\":\"abc", "{\"a\":\"abc\\", "{\"a\":\"\\u", "{\"a\":\"\\u12", "{\"a\":\"\\u12zz\"}", "{\"a\":\"\\ud800\"}", "{\"a\":\"\\ud800\\u0041\"}", "{\"a\":\"\\udc00\"}",
                              "{\"a\":\"\\x\"}", "{\"a\":\"\xc3\"}", "{\"a\":\"\xe3\x82\"}", "{\"a\":\"\xf0\x9f\x98\"}", "{\"a\":\"\xc0\xaf\"}", "{\"games\":{\"\xff\":{}}}"};
        for (const char* d : docs) feed_all(d);
    }
    {   // random mutation fuzz of every valid document (fixed seeds): flip, zero, insert, delete, truncate, splice
        const str seeds[] = {man, plan, commit, lease, error};
        for (const str& base : seeds) {
            for (int iter = 0; iter < 3000; ++iter) {
                g_seed = 12345u + (uint32_t)iter * 7919u + (uint32_t)base.size();
                str d = base;
                int ops = 1 + (int)(rnd() % 4);
                for (int k = 0; k < ops && !d.empty(); ++k) {
                    size_t pos = rnd() % d.size();
                    switch (rnd() % 6) {
                        case 0: d[pos] = (char)(rnd() & 0xFF); break;
                        case 1: d[pos] = '\0'; break;
                        case 2: d.insert(pos, 1, (char)(rnd() & 0xFF)); break;
                        case 3: d.erase(pos, 1 + rnd() % 8); break;
                        case 4: d.resize(pos); break;
                        default: { size_t len = 1 + rnd() % 40; d.insert(pos, d.substr(rnd() % d.size(), len)); break; }
                    }
                }
                feed_all(d);
            }
        }
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hubsaves_wire_fuzz_test: all passed\n");
    return 0;
}
