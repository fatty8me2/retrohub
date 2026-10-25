/* Parsers/builders for the cloud saves v2 wire format (design doc section 3). Tolerant readers: unknown fields are ignored. */
#include "cloud_internal.h"
#include "json.h"

namespace hub::saves {

typedef json::Value JV;
using str = std::string;

const WVer* WFile::current() const {
    for (auto& v : vers) if (v.ver == cur) return &v;
    return vers.empty() ? nullptr : &vers[0];
}
const WFile* WGame::find(const std::string& path) const {
    for (auto& f : files) if (f.path == path) return &f;
    return nullptr;
}

namespace {
bool jparse(const str& s, JV* v) { return json::parse(s, v, json::ParseOptions{64, true}); }

str sval(const JV& o, const char* k) {
    const JV* v = o.get(k);
    return v && v->type == JV::String ? v->s : v && v->type == JV::Number ? v->raw : str();
}
WVer read_ver(const JV& o) {
    WVer v;
    v.ver = (int)o.i64("ver", 0);
    v.sha = sval(o, "sha");
    v.size = o.i64("size", 0);
    v.stored = o.i64("stored", v.size);
    v.enc = sval(o, "enc");
    if (v.enc.empty()) v.enc = "none";
    v.mtime = o.i64("mtime", 0);
    v.dev = sval(o, "dev");
    v.dev_name = sval(o, "devName");
    v.ts = o.i64("ts", 0);
    v.cls = sval(o, "class");
    return v;
}
bool sha_ok(const str& s) {
    if (s.size() != 64) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
bool path_ok(const str& p) {   // never trust a server-supplied path
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
    return true;
}
}  // namespace

bool parse_manifest(const std::string& json_text, WManifest* out, std::string* err) {
    JV root;
    if (!jparse(json_text, &root) || root.type != JV::Object) { if (err) *err = "bad manifest"; return false; }
    if (const JV* ok = root.get("ok")) if (ok->type == JV::Bool && !ok->b) { if (err) *err = sval(root, "error"); return false; }
    WManifest m;
    m.rev = root.i64("rev", 0);
    m.full = root.boolean("full", false);
    if (const JV* a = root.get("account")) {
        m.account.used = a->i64("used", 0);
        m.account.prot_used = a->i64("protUsed", 0);
        m.account.cap = a->i64("cap", 0);
        m.account.prot_cap = a->i64("protCap", 0);
        m.account.devices = (int)a->i64("devices", 0);
        m.account.pool = a->str("pool", "ok");
        m.account.paused = a->boolean("paused", false);
    }
    if (const JV* g = root.get("games")) {
        if (g->type != JV::Object) { if (err) *err = "bad manifest games"; return false; }
        for (auto& kv : g->o) {
            if (kv.second.type != JV::Object) continue;
            const JV& gj = kv.second;
            WGame wg;
            wg.gkey = kv.first;
            if (!path_ok(wg.gkey)) continue;
            wg.played_at = gj.i64("playedAt", 0);
            wg.device = sval(gj, "device");
            wg.pin = gj.boolean("pin", false);
            wg.rev = gj.i64("rev", 0);
            if (const JV* l = gj.get("lease")) if (l->type == JV::Object) {
                wg.lease.present = true;
                wg.lease.dev = sval(*l, "dev");
                wg.lease.name = sval(*l, "name");
                wg.lease.since = l->i64("since", 0);
                wg.lease.until = l->i64("until", 0);
            }
            if (const JV* fs = gj.get("files")) if (fs->type == JV::Object) {
                for (auto& fk : fs->o) {
                    if (fk.second.type != JV::Object || !path_ok(fk.first)) continue;
                    WFile wf;
                    wf.path = fk.first;
                    wf.cur = (int)fk.second.i64("cur", 0);
                    wf.cloud = fk.second.str("cloud", "synced");
                    if (const JV* vs = fk.second.get("vers")) if (vs->type == JV::Array)
                        for (auto& vj : vs->a) if (vj.type == JV::Object) {
                            WVer v = read_ver(vj);
                            if (v.ver > 0 && (sha_ok(v.sha) || wf.evicted())) wf.vers.push_back(v);
                        }
                    wg.files.push_back(std::move(wf));
                }
            }
            m.games.push_back(std::move(wg));
        }
    }
    *out = std::move(m);
    return true;
}

std::string build_plan_body(const std::string& gkey, int64_t played_at, const std::string& mode, const std::vector<PlanFile>& files) {
    str b = "{\"game\":" + json::quote(gkey) + ",\"played_at\":" + std::to_string(played_at) + ",\"mode\":" + json::quote(mode) + ",\"files\":[";
    bool first = true;
    for (auto& f : files) {
        if (!first) b += ',';
        first = false;
        b += "{\"path\":" + json::quote(f.path) + ",\"sha\":" + json::quote(f.sha) + ",\"size\":" + std::to_string(f.size) +
             ",\"stored\":" + std::to_string(f.stored) + ",\"enc\":" + json::quote(f.enc) + ",\"mtime\":" + std::to_string(f.mtime) +
             ",\"class\":" + json::quote(f.cls) + ",\"base\":" + std::to_string(f.base) + "}";
    }
    return b + "]}";
}

bool parse_plan(const std::string& json_text, PlanReply* out, std::string* err) {
    JV root;
    if (!jparse(json_text, &root) || root.type != JV::Object) { if (err) *err = "bad plan"; return false; }
    PlanReply r;
    r.ticket = sval(root, "ticket");
    const JV* fs = root.get("files");
    if (!fs || fs->type != JV::Array) { if (err) *err = "bad plan files"; return false; }
    for (auto& f : fs->a) {
        if (f.type != JV::Object) continue;
        PlanAnswer a;
        a.path = sval(f, "path");
        a.action = sval(f, "action");
        if (a.action.empty()) a.action = sval(f, "result");
        if (a.action.empty()) a.action = sval(f, "status");
        a.reason = sval(f, "reason");
        if (const JV* c = f.get("current")) if (c->type == JV::Object) { a.current = read_ver(*c); a.has_current = true; }
        if (a.path.empty() || a.action.empty()) continue;
        r.files.push_back(std::move(a));
    }
    *out = std::move(r);
    return true;
}

bool parse_commit(const std::string& json_text, CommitReply* out, std::string* err) {
    JV root;
    if (!jparse(json_text, &root) || root.type != JV::Object) { if (err) *err = "bad commit"; return false; }
    CommitReply r;
    r.rev = root.i64("rev", 0);
    if (const JV* rs = root.get("results")) if (rs->type == JV::Array)
        for (auto& j : rs->a) {
            if (j.type != JV::Object) continue;
            CommitResult c;
            c.path = sval(j, "path");
            c.ver = (int)j.i64("ver", 0);
            c.cloud = j.str("cloud", "synced");
            str res = sval(j, "result");
            if (res.empty()) res = sval(j, "action");
            c.conflict = j.boolean("conflict", false) || res == "conflict" || c.cloud == "conflict";
            if (const JV* cu = j.get("current")) if (cu->type == JV::Object) { c.current = read_ver(*cu); c.has_current = true; }
            if (!c.path.empty()) r.results.push_back(std::move(c));
        }
    if (const JV* ev = root.get("evicted")) if (ev->type == JV::Array)
        for (auto& j : ev->a) if (j.type == JV::Object) r.evicted.push_back({sval(j, "game"), sval(j, "path")});
    *out = std::move(r);
    return true;
}

bool parse_lease(const std::string& json_text, LeaseReply* out) {
    JV root;
    if (!jparse(json_text, &root) || root.type != JV::Object) return false;
    LeaseReply l;
    l.ok = root.boolean("ok", false);
    if (const JV* h = root.get("held")) if (h->type == JV::Object) {
        l.held = true;
        l.dev = sval(*h, "dev");
        l.name = sval(*h, "name");
        l.since = h->i64("since", 0);
        l.until = h->i64("until", 0);
    }
    *out = l;
    return true;
}

ServerError parse_error(const std::string& json_text) {
    ServerError e;
    JV root;
    if (!jparse(json_text, &root) || root.type != JV::Object) return e;
    e.code = sval(root, "error");
    e.retry_after = root.i64("retryAfter", 0);
    return e;
}

}  // namespace hub::saves
