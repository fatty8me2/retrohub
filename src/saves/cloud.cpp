/* Cloud saves backend (see cloud.h). Windows only: WinHTTP + BCrypt. */
#include "winutil.h"
#include "json.h"
#include "cloud.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using str = std::string;

namespace hub::saves {
namespace {

const uint64_t kMaxObject = 16ull * 1024 * 1024;

/* ----------------------------------------------------------------- crypto */
str to_hex(const unsigned char* d, size_t n) {
    static const char* h = "0123456789abcdef";
    str o;
    for (size_t i = 0; i < n; i++) { o += h[d[i] >> 4]; o += h[d[i] & 15]; }
    return o;
}

class Sha {
public:
    explicit Sha(const str* hmac_key = nullptr) {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, hmac_key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) < 0) { alg_ = nullptr; return; }
        if (BCryptCreateHash(alg_, &h_, nullptr, 0, hmac_key ? (PUCHAR)hmac_key->data() : nullptr,
                             hmac_key ? (ULONG)hmac_key->size() : 0, 0) < 0) h_ = nullptr;
    }
    ~Sha() { if (h_) BCryptDestroyHash(h_); if (alg_) BCryptCloseAlgorithmProvider(alg_, 0); }
    void update(const void* d, size_t n) { if (h_) BCryptHashData(h_, (PUCHAR)d, (ULONG)n, 0); }
    str hex() {
        unsigned char out[32] = {};
        if (!h_ || BCryptFinishHash(h_, out, 32, 0) < 0) return str();
        return to_hex(out, 32);
    }
private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE h_ = nullptr;
};

str sha_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return str();
    Sha s;
    std::vector<char> buf(256 * 1024);
    while (f) { f.read(buf.data(), buf.size()); if (f.gcount() > 0) s.update(buf.data(), (size_t)f.gcount()); }
    return s.hex();
}

/* ----------------------------------------------------------------- JSON (shared reader, src/common/json.h) */
typedef json::Value JV;
bool jparse(const str& text, JV* out) { return json::parse(text, out, json::ParseOptions{64, true}); }
str str_of(const JV& o, const char* k) {   // strings and numbers (raw text), else empty
    const JV* v = o.get(k);
    return v && v->type == JV::String ? v->s : v && v->type == JV::Number ? v->raw : str();
}
long long num_of(const JV& o, const char* k) { return o.i64(k, 0); }

/* ----------------------------------------------------------------- http */
std::wstring W(const std::string& s) { return winutil::to_wide(s); }

str url_enc(const str& s) {
    str o; char b[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

struct Resp {
    int status = 0;            // 0 = network failure
    str error;
    str body;                  // only when no sink
    str etag, sha, mtime;
};

/* Server-wide R2 free-tier guard (see server/cloudflare/src/r2budget.js): 429 cloud-saves-paused (monthly ops) and
 * 507 cloud-saves-full (total storage). Both are NOT transient within a sync run: never retry them. */
static bool budget_paused(const Resp& r) {
    return r.status == 429 && (r.body.find("cloud-saves-paused") != str::npos || r.body.find("cloud-saves-busy-today") != str::npos);
}
static bool budget_full(const Resp& r) { return r.status == 507 && r.body.find("cloud-saves-full") != str::npos; }
static const char* budget_msg(const Resp& r) {
    return r.status == 429 && r.body.find("cloud-saves-busy-today") != str::npos ? "Cloud saves are busy today, will retry later"
         : budget_paused(r) ? "Cloud saves are paused this month" : budget_full(r) ? "Cloud saves are full" : nullptr;
}

struct Http {
    const CloudConfig& cfg;
    bool https = false;
    str host;
    INTERNET_PORT port = 0;
    str base_path;
    HINTERNET session = nullptr, conn = nullptr;
    bool ok = false;

    explicit Http(const CloudConfig& c) : cfg(c) {
        URL_COMPONENTSW uc = {};
        uc.dwStructSize = sizeof uc;
        wchar_t hostw[256] = {}, pathw[512] = {};
        uc.lpszHostName = hostw; uc.dwHostNameLength = 255;
        uc.lpszUrlPath = pathw; uc.dwUrlPathLength = 511;
        std::wstring u = W(c.base_url);
        if (!WinHttpCrackUrl(u.c_str(), 0, 0, &uc)) return;
        https = uc.nScheme == INTERNET_SCHEME_HTTPS;
        port = uc.nPort;
        host = winutil::to_utf8(hostw);
        base_path = winutil::to_utf8(pathw);
        while (!base_path.empty() && base_path.back() == '/') base_path.pop_back();
        session = WinHttpOpen(L"RetroHub-CloudSaves/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
        if (!session) return;
        WinHttpSetTimeouts(session, 8000, 8000, 30000, 30000);
        conn = WinHttpConnect(session, W(host).c_str(), port, 0);
        ok = conn != nullptr;
    }
    ~Http() { if (conn) WinHttpCloseHandle(conn); if (session) WinHttpCloseHandle(session); }

    /* One attempt. sink != null streams the body into it (only for 2xx). */
    Resp once(const str& method, const str& pq, const std::vector<str>& extra, const char* body, size_t blen,
              const str& body_sha, const std::function<bool(const char*, size_t)>& sink) {
        Resp r;
        auto ts = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        HINTERNET req = WinHttpOpenRequest(conn, W(method).c_str(), W(base_path + pq).c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { r.error = "WinHttpOpenRequest failed"; return r; }
        auto xh = [&](const char* name) {
            str pre = str(name) + ": ";
            for (auto& e : extra) if (e.compare(0, pre.size(), pre) == 0) return e.substr(pre.size());
            return str();
        };
        str clen = method == "PUT" ? std::to_string(blen) : str();   // WinHTTP sends Content-Length for PUT bodies
        str hdr = "Authorization: RHSIG uid=" + cfg.user_id + ",ts=" + std::to_string(ts) + ",sig=" +
              cloud_sign(cfg.secret, method, base_path + pq, ts, body_sha, xh("If-Match"), xh("If-None-Match"), clen,
                         xh("X-RH-Mtime")) + "\r\n";
        if (!body_sha.empty()) hdr += "X-RH-Sha: " + body_sha + "\r\n";
        for (auto& e : extra) hdr += e + "\r\n";
        std::wstring wh = W(hdr);
        BOOL sent = WinHttpSendRequest(req, wh.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, (DWORD)blen, 0);
        size_t off = 0;
        while (sent && off < blen) {
            DWORD w = 0;
            DWORD chunk = (DWORD)std::min<size_t>(blen - off, 64 * 1024);
            if (!WinHttpWriteData(req, body + off, chunk, &w) || !w) { sent = FALSE; break; }
            off += w;
        }
        if (!sent || !WinHttpReceiveResponse(req, nullptr)) {
            r.error = "network error " + std::to_string(GetLastError());
            WinHttpCloseHandle(req);
            return r;
        }
        DWORD st = 0, sz = sizeof st;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &st, &sz, nullptr);
        r.status = (int)st;
        auto qh = [&](const wchar_t* name) {
            DWORD n = 0;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, nullptr, &n, WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || n == 0) return str();
            std::wstring w(n / 2, 0);
            if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, &w[0], &n, WINHTTP_NO_HEADER_INDEX)) return str();
            w.resize(n / 2);
            return winutil::to_utf8(w);
        };
        r.etag = qh(L"ETag"); r.sha = qh(L"X-RH-Sha"); r.mtime = qh(L"X-RH-Mtime");
        std::vector<char> buf(64 * 1024);
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got)) { r.status = 0; r.error = "read failed"; break; }
            if (!got) break;
            if (sink && st >= 200 && st < 300) { if (!sink(buf.data(), got)) { r.status = 0; r.error = "sink failed"; break; } }
            else if (r.body.size() < (8u << 20)) r.body.append(buf.data(), got);
        }
        WinHttpCloseHandle(req);
        return r;
    }

    /* With retries on network errors, 5xx and 429. A sink retry re-issues the request, so the sink must be
     * restartable: it is told via reset() before each attempt. */
    Resp call(const str& method, const str& pq, const std::vector<str>& extra = {}, const char* body = nullptr,
              size_t blen = 0, const str& body_sha = str(), const std::function<bool(const char*, size_t)>& sink = nullptr,
              const std::function<std::vector<str>()>& extra_fn = nullptr) {
        Resp r;
        int tries = std::max(1, cfg.max_retries);
        for (int a = 0; a < tries; a++) {
            std::vector<str> ex = extra_fn ? extra_fn() : extra;
            r = once(method, pq, ex, body, blen, body_sha, sink);
            if (r.status != 0 && r.status < 500 && r.status != 429) return r;
            if (budget_paused(r) || budget_full(r)) return r;   // no retry storm (507 is >= 500 but permanent)
            if (a + 1 < tries) std::this_thread::sleep_for(std::chrono::milliseconds(300 << a));
        }
        return r;
    }
};

/* ----------------------------------------------------------------- state */
struct CS { str etag, sha; };
typedef std::map<str, CS> State;   // rel (as the server spells it) -> remote etag + sha known to match the mirror

/* Lives NEXT to the mirror folder: anything inside it would be merged into the vault by sync_folder. */
fs::path state_path(const fs::path& mirror) {
    fs::path m = mirror;
    if (!m.has_filename()) m = m.parent_path();
    return m.parent_path() / (m.filename().u8string() + ".cloudstate");
}

void load_state(const fs::path& mirror, State& st) {
    std::ifstream f(state_path(mirror), std::ios::binary);
    str line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = line.find('\t'), b = a == str::npos ? a : line.find('\t', a + 1);
        if (b == str::npos) continue;
        st[line.substr(0, a)] = CS{line.substr(a + 1, b - a - 1), line.substr(b + 1)};
    }
}
void save_state(const fs::path& mirror, const State& st) {
    fs::path tmp = state_path(mirror).string() + ".hubtmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        for (auto& kv : st) f << kv.first << '\t' << kv.second.etag << '\t' << kv.second.sha << '\n';
    }
    std::error_code ec;
    fs::rename(tmp, state_path(mirror), ec);
    if (ec) { fs::remove(state_path(mirror), ec); fs::rename(tmp, state_path(mirror), ec); }
}

struct RemoteFile { str key, etag, sha, mtime; long long size = 0; };

fs::path rel_path(const fs::path& root, const str& rel) { return root / fs::u8path(rel); }

bool is_states(const str& rel) {
    size_t a = rel.find('/'); if (a == str::npos) return false;
    size_t b = rel.find('/', a + 1); if (b == str::npos) return false;
    size_t c = rel.find('/', b + 1);
    str comp = rel.substr(b + 1, c == str::npos ? str::npos : c - b - 1);
    for (auto& ch : comp) ch = (char)tolower((unsigned char)ch);
    return comp == "states";
}
bool is_identity(const str& rel) {
    str l = rel.substr(0, rel.find('/'));
    for (auto& ch : l) ch = (char)tolower((unsigned char)ch);
    return l == "_identity";
}
bool wanted(const str& rel, bool states) {
    if (is_identity(rel)) return false;
    if (!states && is_states(rel)) return false;
    return rel.size() < 7 || rel.compare(rel.size() - 7, 7, ".hubtmp") != 0;
}

void set_mtime(const fs::path& p, const str& mtime) {
    if (mtime.empty()) return;
    uint64_t ft = strtoull(mtime.c_str(), nullptr, 10);
    if (!ft) return;
    HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME f; f.dwLowDateTime = (DWORD)ft; f.dwHighDateTime = (DWORD)(ft >> 32);
    SetFileTime(h, nullptr, nullptr, &f);
    CloseHandle(h);
}
str file_mtime(const fs::path& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "0";
    FILETIME f = {};
    GetFileTime(h, nullptr, nullptr, &f);
    CloseHandle(h);
    return std::to_string(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime);
}

bool fetch_list(Http& http, std::vector<RemoteFile>& out, CloudStatus* stat, str* err, int* status) {
    Resp r = http.call("GET", "/saves/list");
    if (status) *status = r.status;
    if (r.status != 200) {
        if (err) *err = r.status == 0 ? r.error : budget_msg(r) ? budget_msg(r) :
            r.status == 503 ? "cloud saves are not enabled on the server" :
            r.status == 401 ? "cloud saves rejected this account (is the friends identity registered?)" :
            r.status == 429 ? "rate limited, try again in a minute" : "server error " + std::to_string(r.status);
        return false;
    }
    JV root;
    if (!jparse(r.body, &root) || root.type != JV::Object) { if (err) *err = "bad list response"; return false; }
    const JV* files = root.get("files");
    if (!files || files->type != JV::Array) { if (err) *err = "bad list response"; return false; }
    for (auto& f : files->a) {
        RemoteFile rf;
        rf.key = str_of(f, "key"); rf.etag = str_of(f, "etag"); rf.sha = str_of(f, "sha"); rf.mtime = str_of(f, "mtime");
        rf.size = num_of(f, "size");
        if (!cloud_key_safe(rf.key)) continue;   // never trust a server-supplied path
        if (!rf.key.empty() && !rf.etag.empty()) out.push_back(rf);
    }
    if (stat) { stat->used = num_of(root, "used"); stat->quota = num_of(root, "quota"); stat->files = (int)files->a.size(); }
    return true;
}

str stamp() {
    SYSTEMTIME t; GetLocalTime(&t);
    char b[32]; snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

/* Download one object into the mirror: resumable via <file>.hubtmp + Range, verified by sha256. */
bool download(Http& http, const fs::path& mirror, const RemoteFile& rf, str* err) {
    if (!cloud_key_safe(rf.key)) { *err = "unsafe key from server"; return false; }
    if (!cloud_sha_ok(rf.sha)) { *err = "server gave no valid checksum for " + rf.key; return false; }
    fs::path dst = rel_path(mirror, rf.key);
    fs::path tmp = dst.string() + ".hubtmp";
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    for (int attempt = 0; attempt < 3; attempt++) {
        uint64_t have = fs::exists(tmp, ec) ? (uint64_t)fs::file_size(tmp, ec) : 0;
        if (have > (uint64_t)rf.size) { fs::remove(tmp, ec); have = 0; }
        std::ofstream out;
        auto reopen = [&](uint64_t keep) {
            if (out.is_open()) out.close();
            std::ios::openmode m = std::ios::binary | (keep ? std::ios::in | std::ios::out : std::ios::trunc | std::ios::out);
            out.open(tmp, m);
            if (keep && out) { out.seekp((std::streamoff)keep); }
            have = keep;
            return (bool)out;
        };
        if (!reopen(have)) { *err = "cannot write " + tmp.string(); return false; }
        if (have == (uint64_t)rf.size) {
            // already complete from a previous run: fall through to verification
        } else {
            str pq = "/saves/obj?key=" + url_enc(rf.key);
            bool first = true;
            Resp r = http.call("GET", pq, {}, nullptr, 0, "",
                [&](const char* d, size_t n) { out.write(d, (std::streamsize)n); have += n; return (bool)out; },
                [&]() {
                    // Each retry resumes where the file ended (flush so the size is right).
                    if (!first) { out.flush(); uint64_t sz = (uint64_t)fs::file_size(tmp, ec); reopen(std::min<uint64_t>(sz, have)); }
                    first = false;
                    return have ? std::vector<str>{"Range: bytes=" + std::to_string(have) + "-"} : std::vector<str>{};
                });
            out.close();
            if (r.status == 404) { *err = "vanished: " + rf.key; return false; }
            if (budget_msg(r)) { *err = budget_msg(r); return false; }
            if (r.status != 200 && r.status != 206) { *err = r.error.empty() ? "download failed (" + std::to_string(r.status) + ") " + rf.key : r.error; return false; }
            if (have != (uint64_t)rf.size) { fs::remove(tmp, ec); continue; }   // object changed mid-resume: restart clean
            if (!r.etag.empty() && r.etag != rf.etag) {}                          // newer than the listing; sha below decides
        }
        out.close();
        str got = sha_file(tmp);
        if (got == rf.sha) {
            fs::remove(dst, ec);
            fs::rename(tmp, dst, ec);
            if (ec) { *err = "cannot place " + dst.string(); return false; }
            set_mtime(dst, rf.mtime);
            return true;
        }
        fs::remove(tmp, ec);   // corrupt or object replaced while resuming: start over
    }
    *err = "checksum mismatch: " + rf.key;
    return false;
}

str default_mirror(const CloudConfig& c) {
    fs::path v = fs::u8path(vault_dir());
    while (!v.empty() && (v.filename().empty() || v.filename() == ".")) v = v.parent_path();
    return (v.parent_path() / "cloud_mirror" / fs::u8path(c.user_id.substr(0, 8))).u8string();
}

} // namespace

/* ----------------------------------------------------------------- public */
bool cloud_key_safe(const std::string& key) {
    if (key.empty() || key.size() > 512 || key[0] == '/' || key[0] == '\\') return false;
    size_t i = 0;
    while (i <= key.size()) {
        size_t j = key.find('/', i);
        if (j == str::npos) j = key.size();
        str seg = key.substr(i, j - i);
        if (seg.empty() || seg == "." || seg == "..") return false;
        i = j + 1;
    }
    for (unsigned char c : key) if (c < 0x20 || c == 0x7f || c == ':' || c == '\\') return false;
    return true;
}

bool cloud_sha_ok(const std::string& sha) {
    if (sha.size() != 64) return false;
    for (char c : sha) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string sha256_hex_of(const std::string& data) {
    Sha s; s.update(data.data(), data.size()); return s.hex();
}

std::string cloud_sign(const std::string& secret, const std::string& method, const std::string& path_and_query,
                       long long ts_ms, const std::string& body_sha, const std::string& if_match,
                       const std::string& if_none_match, const std::string& content_length, const std::string& mtime) {
    str key = sha256_hex_of(secret);   // the server stores sha256hex(secret); that is the HMAC key
    Sha h(&key);
    str msg = method + "\n" + path_and_query + "\n" + std::to_string(ts_ms) + "\n" + (body_sha.empty() ? "-" : body_sha) +
                "\n" + (if_match.empty() ? "-" : if_match) + "\n" + (if_none_match.empty() ? "-" : if_none_match) +
                "\n" + (content_length.empty() ? "-" : content_length) + "\n" + (mtime.empty() ? "-" : mtime);
    h.update(msg.data(), msg.size());
    return h.hex();
}

CloudStatus cloud_status(const CloudConfig& cfg) {
    CloudStatus s;
    Http http(cfg);
    if (!http.ok) { s.error = "bad server url"; return s; }
    std::vector<RemoteFile> files;
    int status = 0;
    if (!fetch_list(http, files, &s, &s.error, &status)) { s.unavailable = status == 503 || status == 401; return s; }
    s.enabled = true;
    return s;
}

SyncReport sync_cloud(const CloudConfig& cfg) {
    SyncReport rep;
    auto fail = [&](const str& e) { rep.ok = false; rep.error = e; return rep; };
    if (cfg.base_url.empty() || cfg.user_id.size() != 26 || cfg.secret.size() < 16) return fail("cloud saves need a friends identity");

    Http http(cfg);
    if (!http.ok) return fail("bad cloud server url");
    fs::path mirror = fs::u8path(cfg.mirror_dir.empty() ? default_mirror(cfg) : cfg.mirror_dir);
    std::error_code ec;
    fs::create_directories(mirror, ec);
    if (!fs::is_directory(mirror)) return fail("cannot create cloud mirror folder");

    std::vector<RemoteFile> remote;
    str err; int status = 0;
    if (!fetch_list(http, remote, nullptr, &err, &status)) return fail(err);

    State st;
    load_state(mirror, st);

    /* 1. download remote changes into the mirror */
    int dl_fail = 0;
    for (const RemoteFile& rf : remote) {
        if (!wanted(rf.key, cfg.include_states)) continue;
        if (rf.size < 0 || (uint64_t)rf.size > kMaxObject) { rep.log.push_back("skip " + rf.key + ": too large"); continue; }
        auto it = st.find(rf.key);
        if (it != st.end() && it->second.etag == rf.etag) continue;     // unchanged since last sync
        fs::path dst = rel_path(mirror, rf.key);
        bool exists = fs::is_regular_file(dst, ec);
        str mine = exists ? sha_file(dst) : str();
        if (exists && mine == rf.sha) { st[rf.key] = CS{rf.etag, rf.sha}; continue; }   // already identical
        if (exists && (it == st.end() || mine != it->second.sha)) {
            // The mirror holds a local edit that was never uploaded; keep it before replacing.
            fs::path keep = rel_path(mirror, "_conflicts/" + rf.key + ".cloud-" + stamp());
            fs::create_directories(keep.parent_path(), ec);
            fs::copy_file(dst, keep, fs::copy_options::overwrite_existing, ec);
            if (ec) { rep.log.push_back("skip " + rf.key + ": cannot preserve pending local copy"); dl_fail++; continue; }
            rep.log.push_back("conflict " + rf.key + ": pending local copy kept in _conflicts");
        }
        str e2;
        if (!download(http, mirror, rf, &e2)) {
            rep.log.push_back("download failed: " + e2);
            dl_fail++;
            if (e2.rfind("network", 0) == 0 || e2.rfind("download failed (0", 0) == 0) break;   // connection is gone
            continue;
        }
        st[rf.key] = CS{rf.etag, rf.sha};
        rep.pulled++;
        rep.log.push_back("cloud pull " + rf.key);
    }
    save_state(mirror, st);

    /* 2. merge vault <-> mirror with the regular folder-sync rules (conflicts included) */
    SyncReport f = sync_folder(mirror.u8string(), cfg.include_states);
    for (auto& l : f.log) rep.log.push_back(l);
    rep.conflicts += f.conflicts;
    if (!f.ok) { rep.ok = false; rep.error = f.error; save_state(mirror, st); return rep; }

    /* 3. upload mirror files that differ from what the cloud has */
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(mirror, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) files.push_back(it->path());
    }
    int up_fail = 0;
    for (const fs::path& p : files) {
        str rel = fs::relative(p, mirror, ec).generic_u8string();
        if (rel.empty() || rel[0] == '.' || !wanted(rel, cfg.include_states)) continue;
        uint64_t sz = (uint64_t)fs::file_size(p, ec);
        if (sz > kMaxObject) { rep.log.push_back("skip " + rel + ": larger than 16 MB"); continue; }
        str sha = sha_file(p);
        if (sha.empty()) { rep.log.push_back("skip " + rel + ": cannot read"); continue; }
        auto it = st.find(rel);
        if (it != st.end() && it->second.sha == sha) continue;
        std::ifstream in(p, std::ios::binary);
        str data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if ((uint64_t)data.size() != sz || sha256_hex_of(data) != sha) { rep.log.push_back("skip " + rel + ": changed while reading"); continue; }
        std::vector<str> hdr = {"X-RH-Mtime: " + file_mtime(p),
                                (it != st.end() && !it->second.etag.empty()) ? "If-Match: " + it->second.etag : str("If-None-Match: *")};
        Resp r = http.call("PUT", "/saves/obj?key=" + url_enc(rel), hdr, data.data(), data.size(), sha);
        if (r.status == 200) {
            JV j;
            str etag = (jparse(r.body, &j) && j.type == JV::Object) ? str_of(j, "etag") : str();
            st[rel] = CS{etag, sha};
            rep.pushed++;
            rep.log.push_back("cloud push " + rel);
            continue;
        }
        if (r.status == 412) {
            JV j;
            const JV* cur = (jparse(r.body, &j) && j.type == JV::Object) ? j.get("current") : nullptr;
            if (cur && cur->type == JV::Object && str_of(*cur, "sha") == sha) {    // our upload already landed (lost response)
                st[rel] = CS{str_of(*cur, "etag"), sha};
                continue;
            }
            rep.log.push_back("deferred " + rel + ": changed in the cloud meanwhile, merged on the next sync");
            st.erase(rel);   // next run will see the remote etag as new and take the conflict path
            continue;
        }
        up_fail++;
        rep.log.push_back("upload failed " + rel + ": " + (r.status == 0 ? r.error : "HTTP " + std::to_string(r.status)));
        if (budget_msg(r)) { rep.ok = false; rep.error = budget_msg(r); break; }   // stop; next scheduled sync retries
        if (r.status == 0 || r.status == 429) break;               // network down / throttled: stop, retry later
        if (r.status == 507) { rep.ok = false; rep.error = "cloud quota or file limit reached"; break; }
    }
    save_state(mirror, st);
    if (rep.ok && (dl_fail || up_fail)) {
        rep.ok = false;
        rep.error = "some files could not be synced (" + std::to_string(dl_fail + up_fail) + "); will retry next sync";
    }
    return rep;
}

} // namespace hub::saves
