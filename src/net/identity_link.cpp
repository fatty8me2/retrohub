#include "identity_link.h"
#include "net_internal.h"
#include "session.h"

#include "plat/plat.h"
#include "plat/plat_crypto.h"
#if RH_WIN
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>
#else
#include "plat/plat_http.h"
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace hubnet {
using namespace detail;   // Json, json_parse, json_escape (net_internal.h)
namespace {

namespace fs = std::filesystem;

constexpr unsigned kPbkdf2Iters = 200000;
constexpr char kAad[] = "rh-link-v1";
constexpr size_t kSaltLen = 16, kIvLen = 12, kTagLen = 16;
constexpr char kAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

bool is_b32(const std::string& s, size_t len)
{
    if (s.size() != len) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7'))) return false;
    return true;
}

#if RH_WIN
bool random_bytes(uint8_t* p, size_t n)
{
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::string b64enc(const std::vector<uint8_t>& v)
{
    if (v.empty()) return "";
    DWORD n = 0;
    CryptBinaryToStringA(v.data(), (DWORD)v.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
    std::string s(n, '\0');
    CryptBinaryToStringA(v.data(), (DWORD)v.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &s[0], &n);
    s.resize(std::strlen(s.c_str()));
    return s;
}

bool b64dec(const std::string& s, std::vector<uint8_t>* out)
{
    DWORD n = 0;
    if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr)) return false;
    out->resize(n);
    if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, out->data(), &n, nullptr, nullptr)) return false;
    out->resize(n);
    return true;
}

bool derive_key(const std::string& code, const uint8_t* salt, uint8_t key[32])
{
    BCRYPT_ALG_HANDLE h = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        return false;
    NTSTATUS st = BCryptDeriveKeyPBKDF2(h, (PUCHAR)code.data(), (ULONG)code.size(), (PUCHAR)salt, (ULONG)kSaltLen,
                                        kPbkdf2Iters, key, 32, 0);
    BCryptCloseAlgorithmProvider(h, 0);
    return BCRYPT_SUCCESS(st);
}

/* AES-256-GCM over `data` in place. encrypt: data = plaintext in, ciphertext out, tag produced.
 * decrypt: data = ciphertext in, plaintext out, tag verified. */
bool aes_gcm(bool encrypt, const uint8_t key[32], const uint8_t iv[kIvLen], std::vector<uint8_t>& data, uint8_t tag[kTagLen])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE hk = nullptr;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                                         sizeof(BCRYPT_CHAIN_MODE_GCM), 0)) &&
        BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &hk, nullptr, 0, (PUCHAR)key, 32, 0))) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = (PUCHAR)iv;
        info.cbNonce = (ULONG)kIvLen;
        info.pbAuthData = (PUCHAR)kAad;
        info.cbAuthData = (ULONG)(sizeof kAad - 1);
        info.pbTag = tag;
        info.cbTag = (ULONG)kTagLen;
        ULONG outlen = 0;
        std::vector<uint8_t> out(data.size());
        NTSTATUS st = encrypt
            ? BCryptEncrypt(hk, data.data(), (ULONG)data.size(), &info, nullptr, 0, out.data(), (ULONG)out.size(), &outlen, 0)
            : BCryptDecrypt(hk, data.data(), (ULONG)data.size(), &info, nullptr, 0, out.data(), (ULONG)out.size(), &outlen, 0);
        if (BCRYPT_SUCCESS(st)) {
            out.resize(outlen);
            data = out;
            ok = true;
        }
    }
    if (hk) BCryptDestroyKey(hk);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

#else
bool random_bytes(uint8_t* p, size_t n)
{
    return plat::random_bytes(p, n);
}

std::string b64enc(const std::vector<uint8_t>& v)
{
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < v.size(); i += 3) {
        unsigned x = (v[i] << 16) | (v[i + 1] << 8) | v[i + 2];
        o += T[x >> 18]; o += T[(x >> 12) & 63]; o += T[(x >> 6) & 63]; o += T[x & 63];
    }
    if (i + 1 == v.size()) {
        unsigned x = v[i] << 16;
        o += T[x >> 18]; o += T[(x >> 12) & 63]; o += "==";
    } else if (i + 2 == v.size()) {
        unsigned x = (v[i] << 16) | (v[i + 1] << 8);
        o += T[x >> 18]; o += T[(x >> 12) & 63]; o += T[(x >> 6) & 63]; o += '=';
    }
    return o;
}

/* Like CryptStringToBinaryA(CRYPT_STRING_BASE64): skips whitespace, optional '=' padding. */
bool b64dec(const std::string& s, std::vector<uint8_t>* out)
{
    out->clear();
    unsigned acc = 0;
    int bits = 0;
    size_t pad = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') { ++pad; continue; }
        else if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        else return false;
        if (pad) return false;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out->push_back((uint8_t)(acc >> bits)); acc &= (1u << bits) - 1; }
    }
    return true;
}

bool derive_key(const std::string& code, const uint8_t* salt, uint8_t key[32])
{
    return CCKeyDerivationPBKDF(kCCPBKDF2, code.data(), code.size(), salt, kSaltLen, kCCPRFHmacAlgSHA256, kPbkdf2Iters, key, 32) ==
           kCCSuccess;
}

/* AES-256-GCM (NIST SP 800-38D, 96-bit IV) built on single-block AES (CommonCrypto has no public GCM): CTR for the data,
 * GHASH over AAD + ciphertext for the tag. Same bytes as BCrypt's GCM. */
struct Block { uint64_t hi, lo; };

bool aes_block(const uint8_t key[32], const uint8_t in[16], uint8_t out[16])
{
    size_t moved = 0;
    return CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 32, nullptr, in, 16, out, 16, &moved) == kCCSuccess &&
           moved == 16;
}

Block load_block(const uint8_t* p)
{
    Block b{0, 0};
    for (int i = 0; i < 8; ++i) { b.hi = (b.hi << 8) | p[i]; b.lo = (b.lo << 8) | p[8 + i]; }
    return b;
}

void store_block(Block b, uint8_t* p)
{
    for (int i = 7; i >= 0; --i) { p[i] = (uint8_t)b.hi; b.hi >>= 8; p[8 + i] = (uint8_t)b.lo; b.lo >>= 8; }
}

Block gf_mul(Block x, Block h)   // x * h in GF(2^128), GCM bit order
{
    Block z{0, 0}, v = h;
    for (int i = 0; i < 128; ++i) {
        bool bit = i < 64 ? ((x.hi >> (63 - i)) & 1) : ((x.lo >> (127 - i)) & 1);
        if (bit) { z.hi ^= v.hi; z.lo ^= v.lo; }
        bool lsb = v.lo & 1;
        v.lo = (v.lo >> 1) | (v.hi << 63);
        v.hi >>= 1;
        if (lsb) v.hi ^= 0xE100000000000000ULL;
    }
    return z;
}

Block ghash_update(Block y, Block h, const uint8_t* d, size_t n)   // zero-pads the last partial block
{
    for (size_t i = 0; i < n; i += 16) {
        uint8_t blk[16] = {0};
        std::memcpy(blk, d + i, std::min<size_t>(16, n - i));
        Block b = load_block(blk);
        y.hi ^= b.hi; y.lo ^= b.lo;
        y = gf_mul(y, h);
    }
    return y;
}

bool aes_gcm(bool encrypt, const uint8_t key[32], const uint8_t iv[kIvLen], std::vector<uint8_t>& data, uint8_t tag[kTagLen])
{
    uint8_t zero[16] = {0}, hb[16];
    if (!aes_block(key, zero, hb)) return false;
    Block h = load_block(hb);

    uint8_t j0[16] = {0};
    std::memcpy(j0, iv, kIvLen);
    j0[15] = 1;
    uint8_t ctr[16];
    std::memcpy(ctr, j0, 16);
    std::vector<uint8_t> out(data.size());
    for (size_t i = 0; i < data.size(); i += 16) {
        for (int k = 15; k >= 12; --k) if (++ctr[k]) break;   // inc32
        uint8_t ks[16];
        if (!aes_block(key, ctr, ks)) return false;
        for (size_t k = 0; k < 16 && i + k < data.size(); ++k) out[i + k] = data[i + k] ^ ks[k];
    }
    const std::vector<uint8_t>& ct = encrypt ? out : data;
    Block y{0, 0};
    y = ghash_update(y, h, (const uint8_t*)kAad, sizeof kAad - 1);
    y = ghash_update(y, h, ct.data(), ct.size());
    Block lens{(uint64_t)(sizeof kAad - 1) * 8, (uint64_t)ct.size() * 8};
    y.hi ^= lens.hi; y.lo ^= lens.lo;
    y = gf_mul(y, h);
    uint8_t s[16], ej0[16], t[16];
    store_block(y, s);
    if (!aes_block(key, j0, ej0)) return false;
    for (int k = 0; k < 16; ++k) t[k] = s[k] ^ ej0[k];
    if (encrypt) {
        std::memcpy(tag, t, kTagLen);
    } else {
        unsigned diff = 0;
        for (size_t k = 0; k < kTagLen; ++k) diff |= (unsigned)(t[k] ^ tag[k]);
        if (diff) return false;
    }
    data = out;
    return true;
}

#endif

std::string identity_to_json(const Identity& id)
{
    return "{\"user_id\":" + std::string("\"") + json::escape(id.user_id) + "\",\"secret\":\"" + json::escape(id.secret) +
           "\",\"created\":" + std::to_string(id.created) + "}\n";
}

bool identity_from_json(const std::string& text, Identity* out)
{
    Json j;
    if (!json_parse(text, &j) || j.type != Json::Object) return false;
    Identity t;
    t.user_id = j.str("user_id");
    t.secret = j.str("secret");
    const Json* c = j.get("created");
    t.created = (c && c->type == Json::Number) ? (int64_t)c->n : 0;
    if (!is_b32(t.user_id, 26) || !is_b32(t.secret, 52)) return false;
    *out = t;
    return true;
}

#if RH_WIN
/* ---- HTTPS POST (WinHTTP), path like "/link/create" ---- */

struct HttpResult { int status = 0; std::string body; };

bool http_post(const std::string& server, const std::string& path, const std::string& body, HttpResult* r, std::string* err)
{
    std::string base = server.empty() ? default_server() : server;
    bool secure = true;
    for (const char* p : {"wss://", "https://"})
        if (base.rfind(p, 0) == 0) base = base.substr(std::strlen(p));
    for (const char* p : {"ws://", "http://"}) {
        if (base.rfind(p, 0) == 0) { base = base.substr(std::strlen(p)); secure = false; }
    }
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (!secure) {
        /* Credentials must not travel in clear text: plain http/ws only for loopback or an explicit dev flag. */
        std::string h = base.substr(0, base.find(':'));
        bool local = h == "localhost" || h == "127.0.0.1" || h == "[::1]";
        const char* dev = std::getenv("RETROHUB_ALLOW_INSECURE_LINK");
        if (!local && !(dev && dev[0] == '1')) { *err = "refusing an unencrypted link server"; return false; }
    }
    INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    size_t colon = base.find(':');
    if (colon != std::string::npos) {
        try {
            int pv = std::stoi(base.substr(colon + 1));
            if (pv < 1 || pv > 65535) throw std::out_of_range("port");
            port = (INTERNET_PORT)pv;
        } catch (...) { *err = "invalid server address"; return false; }
        base = base.substr(0, colon);
    }
    std::wstring whost(base.begin(), base.end()), wpath(path.begin(), path.end());

    HINTERNET hs = WinHttpOpen(L"RetroHub/link", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) { *err = "network unavailable"; return false; }
    WinHttpSetTimeouts(hs, 5000, 5000, 5000, 10000);
    HINTERNET hc = WinHttpConnect(hs, whost.c_str(), port, 0);
    HINTERNET hr = hc ? WinHttpOpenRequest(hc, L"POST", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                      : nullptr;
    bool ok = hr && WinHttpSendRequest(hr, L"Content-Type: application/json\r\n", (DWORD)-1L,
                                        (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
              WinHttpReceiveResponse(hr, nullptr);
    if (ok) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &sz, WINHTTP_NO_HEADER_INDEX);
        r->status = (int)code;
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hr, &avail) && avail) {
            std::vector<char> buf(avail);
            DWORD got = 0;
            if (!WinHttpReadData(hr, buf.data(), avail, &got)) break;
            r->body.append(buf.data(), got);
        }
    } else {
        *err = "could not reach the server";
    }
    if (hr) WinHttpCloseHandle(hr);
    if (hc) WinHttpCloseHandle(hc);
    WinHttpCloseHandle(hs);
    return ok;
}

#else
/* ---- HTTPS POST (plat::http, NSURLSession), path like "/link/create" ---- */

struct HttpResult { int status = 0; std::string body; };

bool http_post(const std::string& server, const std::string& path, const std::string& body, HttpResult* r, std::string* err)
{
    std::string base = server.empty() ? default_server() : server;
    bool secure = true;
    for (const char* p : {"wss://", "https://"})
        if (base.rfind(p, 0) == 0) base = base.substr(std::strlen(p));
    for (const char* p : {"ws://", "http://"}) {
        if (base.rfind(p, 0) == 0) { base = base.substr(std::strlen(p)); secure = false; }
    }
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (!secure) {
        /* Credentials must not travel in clear text: plain http/ws only for loopback or an explicit dev flag. */
        std::string h = base.substr(0, base.find(':'));
        bool local = h == "localhost" || h == "127.0.0.1" || h == "[::1]";
        const char* dev = std::getenv("RETROHUB_ALLOW_INSECURE_LINK");
        if (!local && !(dev && dev[0] == '1')) { *err = "refusing an unencrypted link server"; return false; }
    }
    size_t colon = base.find(':');
    if (colon != std::string::npos) {
        try {
            int pv = std::stoi(base.substr(colon + 1));
            if (pv < 1 || pv > 65535) throw std::out_of_range("port");
        } catch (...) { *err = "invalid server address"; return false; }
    }
    plat::http::Request req;
    req.method = "POST";
    req.url = std::string(secure ? "https://" : "http://") + base + path;
    req.headers = {{"User-Agent", "RetroHub/link"}, {"Content-Type", "application/json"}};
    req.body = body;
    req.connect_timeout_ms = 5000;
    req.timeout_ms = 10000;
    plat::http::Response resp;
    if (!plat::http::perform(req, &resp)) { *err = "could not reach the server"; return false; }
    r->status = resp.status;
    r->body = resp.body;
    return true;
}

#endif

/* Server error text: the "error" field, or a generic message. */
std::string server_error(const HttpResult& r)
{
    Json j;
    std::string e = (json_parse(r.body, &j) && j.type == Json::Object) ? j.str("error") : "";
    return e.empty() ? ("server returned HTTP " + std::to_string(r.status)) : e;
}

} // namespace

std::string link_normalize_code(const std::string& code)
{
    std::string s;
    for (char c : code) {
        if (c == '-' || c == ' ' || c == '\t') continue;
        s += (char)std::toupper((unsigned char)c);
    }
    if (s.size() != 8 && s.size() != 12) return "";   // 8 = temporary link code, 12 = account code (XXXX-XXXX-XXXX)
    for (char c : s)
        if (!std::strchr(kAlphabet, c) || c == '\0') return "";
    return s;
}

bool link_seal(const std::string& code, const Identity& id, LinkEnvelope* out)
{
    std::string c = link_normalize_code(code);
    if (c.empty() || !is_b32(id.user_id, 26) || !is_b32(id.secret, 52)) return false;
    uint8_t salt[kSaltLen], iv[kIvLen], key[32], tag[kTagLen];
    if (!random_bytes(salt, sizeof salt) || !random_bytes(iv, sizeof iv) || !derive_key(c, salt, key)) return false;
    std::string plain = identity_to_json(id);
    std::vector<uint8_t> data(plain.begin(), plain.end());
    if (!aes_gcm(true, key, iv, data, tag)) return false;
    data.insert(data.end(), tag, tag + kTagLen);
    out->salt = b64enc(std::vector<uint8_t>(salt, salt + kSaltLen));
    out->iv = b64enc(std::vector<uint8_t>(iv, iv + kIvLen));
    out->ct = b64enc(data);
    return true;
}

bool link_open(const std::string& code, const LinkEnvelope& env, Identity* out)
{
    std::string c = link_normalize_code(code);
    std::vector<uint8_t> salt, iv, ct;
    if (c.empty() || !b64dec(env.salt, &salt) || !b64dec(env.iv, &iv) || !b64dec(env.ct, &ct)) return false;
    if (salt.size() != kSaltLen || iv.size() != kIvLen || ct.size() <= kTagLen) return false;
    uint8_t key[32], tag[kTagLen];
    if (!derive_key(c, salt.data(), key)) return false;
    std::memcpy(tag, ct.data() + ct.size() - kTagLen, kTagLen);
    std::vector<uint8_t> data(ct.begin(), ct.end() - kTagLen);
    if (!aes_gcm(false, key, iv.data(), data, tag)) return false;   // wrong code or tampering
    return identity_from_json(std::string(data.begin(), data.end()), out);
}

bool link_create_code(const std::string& server, const Identity& id, std::string* code_out, std::string* err)
{
    std::string auth = "\"uid\":\"" + id.user_id + "\",\"secret\":\"" + id.secret + "\"";
    HttpResult r;
    std::string e;
    if (!http_post(server, "/link/create", "{" + auth + "}", &r, &e)) { *err = e; return false; }
    Json j;
    if (r.status != 200 || !json_parse(r.body, &j)) { *err = "create failed: " + server_error(r); return false; }
    std::string code = j.str("code");
    if (link_normalize_code(code).empty()) { *err = "server returned a bad code"; return false; }

    LinkEnvelope env;
    if (!link_seal(code, id, &env)) { *err = "could not encrypt the identity"; return false; }
    HttpResult p;
    std::string body = "{" + auth + ",\"code\":\"" + code + "\",\"salt\":\"" + env.salt + "\",\"iv\":\"" + env.iv +
                       "\",\"ct\":\"" + env.ct + "\"}";
    if (!http_post(server, "/link/put", body, &p, &e)) { *err = e; return false; }
    if (p.status != 200) { *err = "upload failed: " + server_error(p); return false; }
    *code_out = code;
    return true;
}

std::string sha256_hex(const std::string& s)
{
    return plat::hex(plat::hash(plat::HashAlg::Sha256, s));   // same bytes as the BCrypt SHA-256 it replaces
}

std::string random_account_code()
{
    std::string c;
    uint8_t buf[64];
    while (c.size() < 12) {
        if (!random_bytes(buf, sizeof buf)) return "";
        for (uint8_t b : buf) if (b < 248 && c.size() < 12) c += kAlphabet[b % 31];   // rejection sampling: uniform over 31 symbols
    }
    return c;
}

bool link_keep_code(const std::string& server, const Identity& id, std::string* code_out, std::string* err)
{
    std::string c = random_account_code();
    if (c.empty()) { *err = "could not make a code"; return false; }
    LinkEnvelope env;
    if (!link_seal(c, id, &env)) { *err = "could not encrypt the identity"; return false; }
    std::string body = "{\"uid\":\"" + id.user_id + "\",\"secret\":\"" + id.secret + "\",\"box\":\"" + sha256_hex(c) + "\",\"salt\":\"" +
                       env.salt + "\",\"iv\":\"" + env.iv + "\",\"ct\":\"" + env.ct + "\"}";
    HttpResult r;
    std::string e;
    if (!http_post(server, "/link/keep", body, &r, &e)) { *err = e; return false; }
    if (r.status == 429) { *err = "Too many new codes. Try again in a while."; return false; }
    if (r.status != 200) { *err = "could not save the code: " + server_error(r); return false; }
    *code_out = c.substr(0, 4) + "-" + c.substr(4, 4) + "-" + c.substr(8, 4);
    return true;
}

bool link_redeem_code(const std::string& server, const std::string& code, Identity* out, std::string* err, std::string* username_out)
{
    std::string c = link_normalize_code(code);
    if (c.empty()) { *err = "Enter the code from your other PC (XXXX-XXXX-XXXX)."; return false; }
    HttpResult r;
    std::string e;
    if (!http_post(server, "/link/redeem", "{\"code\":\"" + c + "\"}", &r, &e)) { *err = e; return false; }
    if (r.status == 409) { *err = "The other PC has not finished uploading yet. Try again in a moment."; return false; }
    if (r.status == 429) { *err = "Too many attempts. For safety, wait about an hour and try again."; return false; }
    if (r.status == 404) { *err = "Code is wrong, expired or already used."; return false; }
    Json j;
    if (r.status != 200 || !json_parse(r.body, &j)) { *err = "redeem failed: " + server_error(r); return false; }
    LinkEnvelope env{j.str("salt"), j.str("iv"), j.str("ct")};
    if (!link_open(c, env, out)) { *err = "Could not decrypt the identity. Check the code and create a new one."; return false; }
    if (username_out) *username_out = j.str("username");
    return true;
}

bool identity_export_file(const Identity& id, const std::string& path, std::string* err)
{
    if (!is_b32(id.user_id, 26) || !is_b32(id.secret, 52)) { *err = "no valid identity"; return false; }
    std::error_code ec;
    fs::create_directories(fs::u8path(path).parent_path(), ec);
    std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) { *err = "could not open the file for writing"; return false; }
    f << identity_to_json(id);
    if (!f) { *err = "write failed"; return false; }
    return true;
}

bool identity_import_file(const std::string& path, Identity* out, std::string* err)
{
    if (!identity_read_file(path, out)) { *err = "not a valid identity file"; return false; }
    return true;
}

} // namespace hubnet
