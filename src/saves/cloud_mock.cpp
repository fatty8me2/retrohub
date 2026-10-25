/* Mock of the cloud saves v2 Worker. See cloud_mock.h. Semantics follow docs/CLOUD_SAVES_V2.md section 3 closely enough for the
 * engine tests; it is not a second implementation to keep in sync with the Worker, only a contract fixture. */
#include "cloud_mock.h"
#include "cloud.h"
#include "cloud_internal.h"
#include "json.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>

namespace hub::saves {

using str = std::string;
typedef json::Value JV;

namespace {
int64_t now_s() { return (int64_t)time(nullptr); }
int64_t now_ms() { return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

struct Ver {
    int ver = 0;
    str sha, enc = "none", dev, dev_name, cls;
    int64_t size = 0, stored = 0, mtime = 0, ts = 0;
};
struct FileRec { int cur = 0; std::vector<Ver> vers; bool evicted = false; };   // vers newest first
struct Game {
    int64_t played_at = 0, rev = 0, lease_until = 0, lease_since = 0;
    str device, lease_dev, lease_name;
    bool pin = false;
    std::map<str, FileRec> files;
};
struct Blob { str bytes, stored_sha, enc; };
struct Ticket { str gkey; std::vector<PlanFile> files; int64_t expires = 0; };
struct Acct {
    str secret;
    std::map<str, Game> games;
    std::map<str, Blob> blobs;            // plain sha -> stored bytes
    std::map<str, str> devices;
    std::map<str, Ticket> tickets;
    int64_t rev = 0;
    int puts = 0;
    int ticket_seq = 0;
};

bool is_prot(const str& cls, int64_t size) {
    if (cls == "settings" || cls == "battery") return true;
    if (cls == "card") return size <= 512 * 1024;
    return false;
}
bool is_prot_ver(const Ver& v) { return is_prot(v.cls, v.size); }

str lower(str s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

struct Req {
    str method, path, query, target;
    std::map<str, str> h;   // lowercase names
    str body;
    std::map<str, str> q;
};
struct Res {
    int status = 200;
    str body, ctype = "application/json";
    std::map<str, str> h;
    int truncate = 0;
};

str url_dec(const str& s) {
    str o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}
Res jerr(int status, const str& code, const str& extra = str()) {
    Res r;
    r.status = status;
    r.body = "{\"ok\":false,\"error\":" + json::quote(code) + extra + "}";
    return r;
}
Res jok(const str& inner) {
    Res r;
    r.body = "{\"ok\":true" + (inner.empty() ? str() : "," + inner) + "}";
    return r;
}
}  // namespace

struct MockServer::Impl {
    MockServer* self = nullptr;
    std::mutex mu;
    std::map<str, Acct> accts;
    SOCKET ls = INVALID_SOCKET;
    std::thread th;
    std::atomic<bool> stop{false};
    std::mutex tmu;
    std::vector<std::thread> conns;

    int64_t used_of(const Acct& a, int64_t* prot = nullptr) {
        int64_t u = 0, p = 0;
        for (auto& g : a.games) for (auto& f : g.second.files) for (auto& v : f.second.vers) {
            u += v.stored;
            if (is_prot_ver(v)) p += v.stored;
        }
        if (prot) *prot = p;
        return u;
    }

    str ver_json(const Ver& v) {
        return "{\"ver\":" + std::to_string(v.ver) + ",\"sha\":" + json::quote(v.sha) + ",\"size\":" + std::to_string(v.size) +
               ",\"stored\":" + std::to_string(v.stored) + ",\"enc\":" + json::quote(v.enc) + ",\"mtime\":" + std::to_string(v.mtime) +
               ",\"dev\":" + json::quote(v.dev) + ",\"devName\":" + json::quote(v.dev_name) + ",\"ts\":" + std::to_string(v.ts) +
               ",\"class\":" + json::quote(v.cls) + "}";
    }

    void drop_unreferenced(Acct& a) {
        std::set<str> ref;
        for (auto& g : a.games) for (auto& f : g.second.files) for (auto& v : f.second.vers) ref.insert(v.sha);
        for (auto it = a.blobs.begin(); it != a.blobs.end();) it = ref.count(it->first) ? std::next(it) : a.blobs.erase(it);
    }

    /* evict large items of other games, least recently played first, until `need` bytes are free; returns evicted (game,path) */
    bool evict_for(Acct& a, const str& keep_game, int64_t need, std::vector<std::pair<str, str>>* ev, bool dry) {
        int64_t u = used_of(a);
        int64_t cap = self->account_cap.load();
        if (u + need <= cap) return true;
        std::vector<std::pair<int64_t, str>> order;
        for (auto& g : a.games) if (g.first != keep_game && !g.second.pin) order.push_back({g.second.played_at, g.first});
        std::sort(order.begin(), order.end());
        for (auto& o : order) {
            Game& g = a.games[o.second];
            for (auto& f : g.files) {
                if (f.second.evicted || f.second.vers.empty()) continue;
                if (is_prot_ver(f.second.vers[0])) continue;
                int64_t freed = 0;
                for (auto& v : f.second.vers) freed += v.stored;
                if (!dry) { f.second.vers.clear(); f.second.evicted = true; g.rev = ++a.rev; }
                if (ev) ev->push_back({o.second, f.first});
                u -= freed;
                if (u + need <= cap) { if (!dry) drop_unreferenced(a); return true; }
            }
        }
        if (!dry) drop_unreferenced(a);
        return u + need <= cap;
    }

    Res handle(const Req& rq, SOCKET s) {
        (void)s;
        if (rq.path == "/__ctl") {   // test control (unauthenticated)
            for (auto& kv : rq.q) {
                int v = atoi(kv.second.c_str());
                if (kv.first == "delay") self->delay_ms = v;
                else if (kv.first == "fail") self->fail_status = v;
                else if (kv.first == "failn") self->fail_count = v;
                else if (kv.first == "truncate") self->truncate_get = v;
                else if (kv.first == "putlimit") self->put_limit = v;
                else if (kv.first == "cap") self->account_cap = atoll(kv.second.c_str());
                else if (kv.first == "leasesecs") self->lease_secs = v;
            }
            return jok("");
        }
        if (self->fail_count > 0) {
            int left = --self->fail_count;
            (void)left;
            if (self->fail_status > 0) return jerr(self->fail_status, self->fail_status == 429 ? "rate-limited" : "cloud-saves-error", ",\"retryAfter\":1");
        }
        /* auth */
        auto ah = rq.h.find("authorization");
        str uid, ts, sig;
        if (ah == rq.h.end() || ah->second.rfind("RHSIG ", 0) != 0) return jerr(401, "unauthorized");
        {
            str v = ah->second.substr(6);
            size_t a = v.find("uid="), b = v.find(",ts="), c = v.find(",sig=");
            if (a == str::npos || b == str::npos || c == str::npos) return jerr(401, "unauthorized");
            uid = v.substr(a + 4, b - a - 4); ts = v.substr(b + 4, c - b - 4); sig = v.substr(c + 5);
        }
        auto ai = accts.find(uid);
        if (ai == accts.end()) return jerr(401, "unauthorized");
        Acct& A = ai->second;
        long long tsv = atoll(ts.c_str());
        if (std::llabs(now_ms() - tsv) > 5 * 60 * 1000) return jerr(401, "stale-timestamp", ",\"serverTime\":" + std::to_string(now_ms()));
        auto hv = [&](const char* n) { auto it = rq.h.find(lower(n)); return it == rq.h.end() ? str() : it->second; };
        str want = cloud_sign(A.secret, rq.method, rq.target, tsv, hv("X-RH-Sha"), hv("If-Match"), hv("If-None-Match"),
                              rq.method == "PUT" ? hv("Content-Length") : str(), hv("X-RH-Mtime"));
        if (want != sig) { self->sig_checks_failed++; return jerr(401, "unauthorized"); }
        if (rq.method == "POST" && !rq.body.empty() && sha256_hex_of(rq.body) != hv("X-RH-Sha")) return jerr(400, "bad-sha");
        str dev = hv("X-RH-Device");
        str devname = A.devices.count(dev) ? A.devices[dev] : dev;

        const str& P = rq.path;
        auto qget = [&](const char* k) { auto it = rq.q.find(k); return it == rq.q.end() ? str() : it->second; };
        JV body;
        if (rq.method == "POST" && !rq.body.empty() && !json::parse(rq.body, &body)) return jerr(400, "bad-body");

        auto account_json = [&]() {
            int64_t prot = 0;
            int64_t u = used_of(A, &prot);
            return "\"account\":{\"used\":" + std::to_string(u) + ",\"protUsed\":" + std::to_string(prot) + ",\"cap\":" +
                   std::to_string(self->account_cap.load()) + ",\"protCap\":" + std::to_string(self->prot_cap.load()) + ",\"devices\":" +
                   std::to_string(A.devices.size()) + ",\"pool\":\"ok\",\"paused\":false}";
        };

        if (P == "/saves/v2/status") return jok(account_json());
        if (P == "/saves/v2/device" && rq.method == "POST") {
            str d = body.str("dev"), n = body.str("name");
            if (d.size() < 8) return jerr(400, "bad-body");
            if (!A.devices.count(d) && A.devices.size() >= 8) return jerr(403, "device-limit");
            A.devices[d] = n;
            return jok("\"devices\":" + std::to_string(A.devices.size()));
        }
        if (P == "/saves/v2/manifest") {
            self->n_manifest++;
            str only = qget("game");
            int64_t since = qget("since").empty() ? -1 : atoll(qget("since").c_str());
            str gs;
            for (auto& g : A.games) {
                if (!only.empty() && g.first != only) continue;
                if (since >= 0 && g.second.rev <= since) continue;
                const Game& G = g.second;
                str fs;
                for (auto& f : G.files) {
                    str vs;
                    for (auto& v : f.second.vers) vs += (vs.empty() ? "" : ",") + ver_json(v);
                    fs += (fs.empty() ? "" : ",") + json::quote(f.first) + ":{\"cur\":" + std::to_string(f.second.cur) + ",\"cloud\":" +
                          (f.second.evicted ? "\"evicted\"" : "\"synced\"") + ",\"vers\":[" + vs + "]}";
                }
                str lease;
                if (G.lease_until > now_s()) lease = ",\"lease\":{\"dev\":" + json::quote(G.lease_dev) + ",\"name\":" + json::quote(G.lease_name) +
                                                     ",\"since\":" + std::to_string(G.lease_since) + ",\"until\":" + std::to_string(G.lease_until) + "}";
                gs += (gs.empty() ? "" : ",") + json::quote(g.first) + ":{\"playedAt\":" + std::to_string(G.played_at) + ",\"device\":" +
                      json::quote(G.device) + ",\"pin\":" + (G.pin ? "true" : "false") + ",\"rev\":" + std::to_string(G.rev) + lease +
                      ",\"files\":{" + fs + "}}";
            }
            return jok("\"rev\":" + std::to_string(A.rev) + ",\"full\":" + (since < 0 ? "true" : "false") + "," + account_json() + ",\"games\":{" + gs + "}");
        }
        if (P == "/saves/v2/plan" && rq.method == "POST") {
            self->n_plan++;
            str gkey = body.str("game");
            const JV* fl = body.get("files");
            if (gkey.empty() || !fl || fl->type != JV::Array || fl->a.size() > 200) return jerr(400, "bad-plan");
            Game& G = A.games[gkey];
            Ticket t;
            t.gkey = gkey;
            t.expires = now_s() + 600;
            str answers;
            int64_t extra_used = 0, extra_prot = 0;
            auto add = [&](const str& path, const str& act, const str& more = str()) {
                answers += (answers.empty() ? "" : ",") + str("{\"path\":") + json::quote(path) + ",\"action\":" + json::quote(act) + more + "}";
            };
            for (auto& f : fl->a) {
                PlanFile pf;
                pf.path = f.str("path"); pf.sha = f.str("sha"); pf.enc = f.str("enc", "none"); pf.cls = f.str("class");
                pf.size = f.i64("size"); pf.stored = f.i64("stored"); pf.mtime = f.i64("mtime"); pf.base = (int)f.i64("base");
                FileRec* fr = G.files.count(pf.path) ? &G.files[pf.path] : nullptr;
                int cur = fr && !fr->evicted ? fr->cur : 0;
                if (fr && fr->evicted) cur = 0;
                const Ver* cv = fr && !fr->vers.empty() ? &fr->vers[0] : nullptr;
                if (pf.stored > 16 * 1024 * 1024) { add(pf.path, "skip", ",\"reason\":\"too-large\""); continue; }
                if (cv && cv->sha == pf.sha && !fr->evicted) { add(pf.path, "same", ",\"current\":" + ver_json(*cv)); continue; }
                if (cur != pf.base && !(fr && fr->evicted)) { add(pf.path, "conflict", cv ? ",\"current\":" + ver_json(*cv) : str()); continue; }
                bool prot = is_prot(pf.cls, pf.size);
                if (A.blobs.count(pf.sha)) { t.files.push_back(pf); add(pf.path, "have"); continue; }
                int64_t ap = 0;
                int64_t u = used_of(A, &ap);
                if (prot) {
                    if (ap + extra_prot + pf.stored > self->prot_cap.load()) { add(pf.path, "skip", ",\"reason\":\"too-large\""); continue; }
                } else if (!evict_for(A, gkey, extra_used + pf.stored, nullptr, true)) { add(pf.path, "skip", ",\"reason\":\"over-cap\""); continue; }
                (void)u;
                extra_used += pf.stored;
                if (prot) extra_prot += pf.stored;
                t.files.push_back(pf);
                add(pf.path, "upload");
            }
            str tid = "T" + std::to_string(++A.ticket_seq) + "-" + std::to_string(now_ms());
            A.tickets[tid] = t;
            G.played_at = std::max<int64_t>(G.played_at, body.i64("played_at"));
            return jok("\"ticket\":" + json::quote(tid) + ",\"files\":[" + answers + "]");
        }
        if (P == "/saves/v2/blob" && rq.method == "PUT") {
            self->n_put++;
            if (self->put_limit > 0 && ++A.puts > self->put_limit) return jerr(429, "account-daily-limit", ",\"retryAfter\":3600");
            auto ti = A.tickets.find(qget("ticket"));
            if (ti == A.tickets.end() || ti->second.expires < now_s()) return jerr(409, "ticket-expired");
            str sha = qget("sha");
            if (sha.size() != 64) return jerr(400, "bad-sha");
            if (sha256_hex_of(rq.body) != hv("X-RH-Sha")) return jerr(400, "bad-sha");
            Blob b;
            b.bytes = rq.body; b.stored_sha = hv("X-RH-Sha");
            for (auto& f : ti->second.files) if (f.sha == sha) b.enc = f.enc;
            if (A.blobs.count(sha)) return jok("\"unchanged\":true");
            A.blobs[sha] = b;
            return jok("");
        }
        if (P == "/saves/v2/commit" && rq.method == "POST") {
            self->n_commit++;
            auto ti = A.tickets.find(body.str("ticket"));
            if (ti == A.tickets.end() || ti->second.expires < now_s()) return jerr(409, "ticket-expired");
            Ticket t = ti->second;
            Game& G = A.games[t.gkey];
            G.device = devname;
            str results;
            std::vector<std::pair<str, str>> ev;
            for (auto& pf : t.files) {
                FileRec& fr = G.files[pf.path];
                int cur = fr.evicted ? 0 : fr.cur;
                if (cur != pf.base && !(fr.evicted && pf.base == 0)) {
                    str cj = fr.vers.empty() ? str() : ",\"current\":" + ver_json(fr.vers[0]);
                    results += (results.empty() ? "" : ",") + str("{\"path\":") + json::quote(pf.path) + ",\"cloud\":\"synced\",\"conflict\":true" + cj + "}";
                    continue;
                }
                if (!A.blobs.count(pf.sha)) { return jerr(409, "missing-blob"); }
                if (!is_prot(pf.cls, pf.size)) evict_for(A, t.gkey, pf.stored, &ev, false);
                Ver v;
                v.ver = std::max(fr.cur, fr.vers.empty() ? 0 : fr.vers[0].ver) + 1;
                v.sha = pf.sha; v.enc = pf.enc; v.size = pf.size; v.stored = pf.stored; v.mtime = pf.mtime; v.dev = dev; v.dev_name = devname;
                v.ts = now_s(); v.cls = pf.cls;
                fr.vers.insert(fr.vers.begin(), v);
                fr.cur = v.ver;
                fr.evicted = false;
                size_t keep = is_prot(pf.cls, pf.size) ? 3 : 1;
                if (fr.vers.size() > keep) fr.vers.resize(keep);
                results += (results.empty() ? "" : ",") + str("{\"path\":") + json::quote(pf.path) + ",\"ver\":" + std::to_string(v.ver) + ",\"cloud\":\"synced\"}";
            }
            G.rev = ++A.rev;
            drop_unreferenced(A);
            A.tickets.erase(body.str("ticket"));
            str evs;
            for (auto& e : ev) evs += (evs.empty() ? "" : ",") + str("{\"game\":") + json::quote(e.first) + ",\"path\":" + json::quote(e.second) + "}";
            return jok("\"rev\":" + std::to_string(A.rev) + ",\"results\":[" + results + "],\"evicted\":[" + evs + "]");
        }
        if (P == "/saves/v2/blob" && rq.method == "GET") {
            self->n_get++;
            auto gi = A.games.find(qget("game"));
            if (gi == A.games.end()) return jerr(404, "not-found");
            auto fi = gi->second.files.find(qget("path"));
            if (fi == gi->second.files.end() || fi->second.evicted) return jerr(404, "not-found");
            int want_ver = qget("ver").empty() ? fi->second.cur : atoi(qget("ver").c_str());
            const Ver* v = nullptr;
            for (auto& x : fi->second.vers) if (x.ver == want_ver) v = &x;
            if (!v || !A.blobs.count(v->sha)) return jerr(404, "not-found");
            const Blob& b = A.blobs[v->sha];
            Res r;
            r.ctype = "application/octet-stream";
            size_t off = 0;
            auto rg = rq.h.find("range");
            if (rg != rq.h.end() && rg->second.rfind("bytes=", 0) == 0) {
                off = (size_t)atoll(rg->second.c_str() + 6);
                if (off >= b.bytes.size() && !b.bytes.empty()) return jerr(416, "bad-range");
                r.status = 206;
            }
            r.body = b.bytes.substr(off);
            r.h["X-RH-Sha"] = b.stored_sha;
            r.h["X-RH-Plain-Sha"] = v->sha;
            r.h["X-RH-Enc"] = v->enc;
            r.h["X-RH-Size"] = std::to_string(v->size);
            int tr = self->truncate_get.exchange(0);
            if (tr > 0 && (size_t)tr < r.body.size()) r.truncate = tr;
            return r;
        }
        if (P == "/saves/v2/restore" && rq.method == "POST") {
            auto gi = A.games.find(body.str("game"));
            if (gi == A.games.end()) return jerr(404, "not-found");
            auto fi = gi->second.files.find(body.str("path"));
            if (fi == gi->second.files.end()) return jerr(404, "not-found");
            if (fi->second.cur != (int)body.i64("base")) return jerr(412, "conflict");
            const Ver* src = nullptr;
            for (auto& x : fi->second.vers) if (x.ver == (int)body.i64("ver")) src = &x;
            if (!src) return jerr(404, "not-found");
            Ver v = *src;
            v.ver = fi->second.vers[0].ver + 1;
            v.ts = now_s();
            fi->second.vers.insert(fi->second.vers.begin(), v);
            fi->second.cur = v.ver;
            gi->second.rev = ++A.rev;
            return jok("\"ver\":" + std::to_string(v.ver));
        }
        if (P == "/saves/v2/game" && rq.method == "POST") {
            auto gi = A.games.find(body.str("game"));
            if (gi == A.games.end()) return jok("");
            if (body.get("pin")) gi->second.pin = body.boolean("pin");
            if (body.boolean("remove")) { A.games.erase(gi); drop_unreferenced(A); A.rev++; }
            else gi->second.rev = ++A.rev;
            return jok("");
        }
        if (P == "/saves/v2/lease" && rq.method == "POST") {
            self->n_lease++;
            Game& G = A.games[body.str("game")];
            if (body.str("mode") == "readonly") return jok("");
            bool held = G.lease_until > now_s() && G.lease_dev != dev;
            if (held && !body.boolean("take")) {
                return jerr(423, "lease-held", ",\"held\":{\"dev\":" + json::quote(G.lease_dev) + ",\"name\":" + json::quote(G.lease_name) +
                            ",\"since\":" + std::to_string(G.lease_since) + ",\"until\":" + std::to_string(G.lease_until) + "}");
            }
            if (G.lease_dev != dev || G.lease_until <= now_s()) G.lease_since = now_s();
            G.lease_dev = dev; G.lease_name = devname; G.lease_until = now_s() + self->lease_secs.load();
            return jok("");
        }
        if (P == "/saves/v2/lease/beat" && rq.method == "POST") {
            Game& G = A.games[body.str("game")];
            if (G.lease_dev == dev) G.lease_until = now_s() + self->lease_secs.load();
            return jok("");
        }
        if (P == "/saves/v2/lease/release" && rq.method == "POST") {
            Game& G = A.games[body.str("game")];
            if (G.lease_dev == dev) G.lease_until = 0;
            return jok("");
        }
        return jerr(404, "not-found");
    }

    static bool send_all(SOCKET s, const char* d, size_t n) {
        while (n) {
            int w = send(s, d, (int)std::min<size_t>(n, 1 << 20), 0);
            if (w <= 0) return false;
            d += w; n -= (size_t)w;
        }
        return true;
    }

    void serve(SOCKET s) {
        DWORD to = 15000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
        str buf;
        char tmp[8192];
        size_t hend = str::npos;
        while ((hend = buf.find("\r\n\r\n")) == str::npos) {
            int n = recv(s, tmp, sizeof tmp, 0);
            if (n <= 0) { closesocket(s); return; }
            buf.append(tmp, (size_t)n);
            if (buf.size() > (1u << 20)) { closesocket(s); return; }
        }
        Req rq;
        {
            str head = buf.substr(0, hend);
            size_t le = head.find("\r\n");
            str line = head.substr(0, le);
            size_t a = line.find(' '), b = line.find(' ', a + 1);
            rq.method = line.substr(0, a);
            rq.target = line.substr(a + 1, b - a - 1);
            size_t qm = rq.target.find('?');
            rq.path = rq.target.substr(0, qm);
            if (qm != str::npos) {
                str qs = rq.target.substr(qm + 1);
                size_t i = 0;
                while (i < qs.size()) {
                    size_t j = qs.find('&', i);
                    if (j == str::npos) j = qs.size();
                    str kv = qs.substr(i, j - i);
                    size_t e = kv.find('=');
                    rq.q[url_dec(kv.substr(0, e))] = e == str::npos ? str() : url_dec(kv.substr(e + 1));
                    i = j + 1;
                }
            }
            size_t p = le == str::npos ? head.size() : le + 2;
            while (p < head.size()) {
                size_t e = head.find("\r\n", p);
                if (e == str::npos) e = head.size();
                str hl = head.substr(p, e - p);
                size_t c = hl.find(':');
                if (c != str::npos) {
                    str v = hl.substr(c + 1);
                    while (!v.empty() && v[0] == ' ') v.erase(0, 1);
                    rq.h[lower(hl.substr(0, c))] = v;
                }
                p = e + 2;
            }
        }
        size_t clen = rq.h.count("content-length") ? (size_t)atoll(rq.h["content-length"].c_str()) : 0;
        rq.body = buf.substr(hend + 4);
        while (rq.body.size() < clen) {
            int n = recv(s, tmp, sizeof tmp, 0);
            if (n <= 0) { closesocket(s); return; }
            rq.body.append(tmp, (size_t)n);
        }
        Res rs;
        if (rq.path != "/__ctl" && self->delay_ms > 0) Sleep((DWORD)self->delay_ms.load());
        {
            std::lock_guard<std::mutex> lk(mu);
            rs = handle(rq, s);
        }
        const char* reason = rs.status == 200 ? "OK" : rs.status == 206 ? "Partial Content" : "Error";
        str out = "HTTP/1.1 " + std::to_string(rs.status) + " " + reason + "\r\nContent-Type: " + rs.ctype + "\r\nContent-Length: " +
                  std::to_string(rs.body.size()) + "\r\nConnection: close\r\n";
        for (auto& kv : rs.h) out += kv.first + ": " + kv.second + "\r\n";
        out += "\r\n";
        if (rs.truncate > 0) {
            send_all(s, out.data(), out.size());
            send_all(s, rs.body.data(), (size_t)rs.truncate);
            shutdown(s, SD_SEND);      // clean close before Content-Length bytes arrived: the client sees a broken transfer
            Sleep(100);
            closesocket(s);
            return;
        }
        send_all(s, out.data(), out.size());
        send_all(s, rs.body.data(), rs.body.size());
        shutdown(s, SD_SEND);
        Sleep(5);
        closesocket(s);
    }

    void accept_loop() {
        while (!stop) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ls, &fds);
            timeval tv = {0, 100000};
            if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET c = accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            std::lock_guard<std::mutex> lk(tmu);
            conns.emplace_back([this, c]() { serve(c); });
        }
    }
};

MockServer::MockServer() : p_(new Impl) { p_->self = this; }
MockServer::~MockServer() { stop(); }

bool MockServer::start(int port) {
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(s, (sockaddr*)&a, sizeof a) != 0 || listen(s, 32) != 0) { closesocket(s); return false; }
    int len = sizeof a;
    getsockname(s, (sockaddr*)&a, &len);
    port_ = ntohs(a.sin_port);
    p_->ls = s;
    p_->stop = false;
    p_->th = std::thread([this]() { p_->accept_loop(); });
    return true;
}

void MockServer::stop() {
    if (p_->ls == INVALID_SOCKET) return;
    p_->stop = true;
    if (p_->th.joinable()) p_->th.join();
    closesocket(p_->ls);
    p_->ls = INVALID_SOCKET;
    std::vector<std::thread> cs;
    { std::lock_guard<std::mutex> lk(p_->tmu); cs.swap(p_->conns); }
    for (auto& t : cs) if (t.joinable()) t.join();
}

void MockServer::add_account(const std::string& uid, const std::string& secret) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->accts[uid].secret = secret;
}
std::string MockServer::cur_sha(const std::string& uid, const std::string& gkey, const std::string& path) {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto& A = p_->accts[uid];
    auto g = A.games.find(gkey);
    if (g == A.games.end()) return str();
    auto f = g->second.files.find(path);
    return f == g->second.files.end() || f->second.vers.empty() ? str() : f->second.vers[0].sha;
}
int MockServer::cur_ver(const std::string& uid, const std::string& gkey, const std::string& path) {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto& A = p_->accts[uid];
    auto g = A.games.find(gkey);
    if (g == A.games.end()) return 0;
    auto f = g->second.files.find(path);
    return f == g->second.files.end() ? 0 : f->second.cur;
}
int MockServer::versions(const std::string& uid, const std::string& gkey, const std::string& path) {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto& A = p_->accts[uid];
    auto g = A.games.find(gkey);
    if (g == A.games.end()) return 0;
    auto f = g->second.files.find(path);
    return f == g->second.files.end() ? 0 : (int)f->second.vers.size();
}
bool MockServer::evicted(const std::string& uid, const std::string& gkey, const std::string& path) {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto& A = p_->accts[uid];
    auto g = A.games.find(gkey);
    if (g == A.games.end()) return false;
    auto f = g->second.files.find(path);
    return f != g->second.files.end() && f->second.evicted;
}
int64_t MockServer::used(const std::string& uid) { std::lock_guard<std::mutex> lk(p_->mu); return p_->used_of(p_->accts[uid]); }
int MockServer::devices(const std::string& uid) { std::lock_guard<std::mutex> lk(p_->mu); return (int)p_->accts[uid].devices.size(); }
void MockServer::seed_file(const std::string& uid, const std::string& gkey, const std::string& path, const std::string& plain, int64_t mtime,
                           const std::string& device_name, int64_t played_at) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Acct& A = p_->accts[uid];
    Game& G = A.games[gkey];
    FileRec& fr = G.files[path];
    Ver v;
    v.ver = (fr.vers.empty() ? 0 : fr.vers[0].ver) + 1;
    v.sha = sha256_hex_of(plain); v.size = v.stored = (int64_t)plain.size(); v.mtime = mtime; v.dev = "d-seeded00"; v.dev_name = device_name;
    v.ts = now_s(); v.cls = class_name(classify_path(path, v.size));
    A.blobs[v.sha] = Blob{plain, v.sha, "none"};
    fr.vers.insert(fr.vers.begin(), v);
    fr.cur = v.ver;
    fr.evicted = false;
    G.played_at = played_at;
    G.device = device_name;
    G.rev = ++A.rev;
}
void MockServer::set_played(const std::string& uid, const std::string& gkey, int64_t played_at) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->accts[uid].games[gkey].played_at = played_at;
}
void MockServer::expire_leases() {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& a : p_->accts) for (auto& g : a.second.games) g.second.lease_until = 0;
}

} // namespace hub::saves
